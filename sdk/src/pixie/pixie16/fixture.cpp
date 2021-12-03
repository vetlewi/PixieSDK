/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Copyright 2021 XIA LLC, All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/** @file fixture.cpp
 * @brief Implements per channel hardware specific support for the Pixie-16 modules.
 */

#include <algorithm>
#include <climits>
#include <cstring>

#include <pixie/error.hpp>
#include <pixie/log.hpp>
#include <pixie/util.hpp>

#include <pixie/pixie16/channel.hpp>
#include <pixie/pixie16/defs.hpp>
#include <pixie/pixie16/fixture.hpp>
#include <pixie/pixie16/memory.hpp>
#include <pixie/pixie16/module.hpp>

namespace xia {
namespace pixie {
namespace fixture {
/**
 * Average a series of numbers and record the maximum and minimum values.
 */
struct average {
    int avg;
    int max;
    int min;
    int count;
    average() : avg(0), max(INT_MIN), min(INT_MAX), count(0) {}
    void update(int val) {
        avg += val;
        max = std::max(max, val);
        min = std::min(min, val);
        ++count;
    }
    void calc() { if (count > 0) avg /= count; }
};

/**
 * linear fit using least squares. Provide linear interpolation.
 */
template<typename T>
struct linear_fit {
    std::vector<T> samples;

    /*
     * Y = kX + c
     */
    double k;
    double c;

    double sum_x;
    double sum_y;
    double sum_xy;
    double sum_x_sq;

    int count;

    linear_fit() : k(0), c(0), sum_x(0), sum_y(0), sum_xy(0), sum_x_sq(0), count(0) {}

    void update(T x, T y) {
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x_sq += x * x;
        ++count;
    }

    void calc() {
        auto divisor = (sum_x * sum_x) - (count * sum_x_sq);
        k = ((sum_x * sum_y) - (count * sum_xy)) / divisor;
        c = ((sum_x * sum_xy) - (sum_y * sum_x_sq)) / divisor;
    }

    T y(T x) {
        return (k * x) + c;
    }
};

/**
 * UserIn save to hold and restore UserIn variables.
 */
struct userin_save {
    hw::memory::dsp dsp;
    hw::address address;

    hw::word userin_0;
    hw::word userin_1;

    userin_save(pixie::module::module& module);

    void update(const hw::word& db_index, const hw::word& db_channel);

    ~userin_save();
};

/**
 * ADC trace analyize
 *
 * This is a simple approach to detecting the average voltage.
 */
struct channel_baseline {
    using bin_buckets = std::vector<int>;

    const int noise_bins = 30;

    int channel;
    int adc_bits;
    double noise_percent;

    int runs;
    int baseline;

    channel_baseline(double noise_percent = 0.5);

    void start(int channel, int adc_bits);
    void end();

    void update(const hw::adc_trace& trace);

    bool operator==(const channel_baseline& other) const;
    bool operator!=(const channel_baseline& other) const;
    bool operator==(const int bl) const;
    bool operator!=(const int bl) const;

    bin_buckets bins;
};

using channel_baselines = std::vector<channel_baseline>;

/**
 * The daughter board fixture
 */
struct db : public channel {
    /**
     * ADC swapped state.
     *
     * If the DSP is not loaded the swap state is unknown.
     */
    enum adc_swap_state {
        adc_boot_state,
        adc_unswapped,
        adc_swapped
    };
    /**
     * The number is the position. The DBs are sorted by position so
     * asking for the index by channel returns the DB number.
     */
    int number;

    /**
     * Base channel for the daughter board
     */
    int base;

    /**
     * Channel offset relative to the fixture.
     */
    int offset;

    /**
     * Dual ADC swapped state. This is true if the data is being clock clocked
     * on the wrong edge.
     */
    adc_swap_state adc_state;

    db(pixie::channel::channel& module_channel_, const hw::config& config_);

    virtual void acquire_adc() override;
    virtual void read_adc(hw::adc_word* buffer, size_t size);

    virtual void set(const std::string item, bool value);
    virtual void get(const std::string item, bool& value);
    virtual void get(const std::string item, int& value);
    virtual void get(const std::string item, double& value);

    virtual void report(std::ostream& out) const override;
};

/**
 * The DB04 fixture
 */
struct db04 : public db {
    /*
     * The DAC output has a filter with an RC 1/e settling time of 47
     * ms. Lets wait a after setting it for the signal to settle.
     */
    const int dac_settle_time_ms = 250;

    db04(pixie::channel::channel& module_channel_, const hw::config& config_);

    virtual void set_dac(param::value_type value) override;

    virtual void get(const std::string item, bool& value);
    virtual void get(const std::string item, int& value);
};

/**
 * Module has AFE DB fixtures
 */
struct afe_dbs : public module {
    enum limits {
        max_dbs = 4
    };

    /*
     * These are development modes.
     */
    const bool adc_swap_verify = true;
    const bool dac_adc_ratio = false;

    std::array<hw::word, max_dbs> adcctrl;

    afe_dbs(pixie::module::module& module_);

    virtual void fgpa_fippi_loaded() override;
    virtual void boot() override;
    virtual void init_channels() override;

    virtual void set_dacs() override;
    virtual void get_traces() override;
    virtual void adjust_offsets() override;

    void calc_dac_adc_ratio();
};

static void unsupported_op(const std::string what) {
    throw error::error(error::code::internal_failure, "invalid fixture op: " + what);
}

static void wait_dac_settle_period(pixie::module::module& mod) {
    /*
     * Find the longest DB settling period and wait that period of time.
     *
     * @note this is only ever called for an `afe-dbs` module fixture and
     *       the default daughter board returns 0.
     */
    int settle_period = 0;
    for (auto& channel : mod.channels) {
        int channel_settle_period;
        channel.fixture->get("DAC_SETTLE_PERIOD", channel_settle_period);
        settle_period = std::max(settle_period, channel_settle_period);
    }
    log(log::debug) << pixie::module::module_label(mod, "afe-dbs: dac-settle-wait")
                    << "period=" << settle_period << " msecs";
    if (settle_period > 0) {
        hw::wait(settle_period * 1000);
    }
}

static void set_channel_voffset(pixie::module::module& mod, double voffset, int step) {
    for (auto chan = 0; chan < mod.num_channels; chan += step) {
        mod.channels[chan].voffset(voffset);
    }
    mod.set_dacs();
    wait_dac_settle_period(mod);
}

static void analyze_channel_baselines(pixie::module::module& mod,
                                      channel_baselines& baselines,
                                      const int traces = 1) {
    baselines.resize(mod.num_channels);
    for (auto& channel : mod.channels) {
        baselines[channel.number].start(channel.number, channel.fixture->config.adc_bits);
    }
    for (int t = 0; t < traces; ++t) {
        mod.get_traces();
        for (auto& channel : mod.channels) {
            xia::pixie::hw::adc_trace trace;
            mod.read_adc(channel.number, trace, false);
            baselines[channel.number].update(trace);
        }
    }
    for (auto& bl : baselines) {
        bl.end();
        log(log::debug) << pixie::module::module_label(mod, "afe-dbs: analyze-baselines")
                        << "channel=" << bl.channel
                        << " baseline=" << bl.baseline;
    }
}

userin_save::userin_save(pixie::module::module& module) : dsp(module) {
    address = module.module_var_descriptors[int(param::module_var::UserIn)].address;
    userin_0 = dsp.read(0, address);
    userin_1 = dsp.read(1, address);
}

void userin_save::update(const hw::word& db_index, const hw::word& db_channel) {
    dsp.write(0, address, db_index);
    dsp.write(1, address, db_channel);
}

userin_save::~userin_save() {
    dsp.write(0, address, userin_0);
    dsp.write(1, address, userin_1);
}

channel_baseline::channel_baseline(double noise_percent_)
    : channel(-1), adc_bits(0), noise_percent(noise_percent_), runs(0), baseline(-1) {
    if (noise_percent > 100) {
        noise_percent = 100;
    } else if (noise_percent < 0) {
        noise_percent = 0;
    }
}

void channel_baseline::start(int channel_, int adc_bits_) {
    channel = channel_;
    adc_bits = adc_bits_;
    bins.clear();
    bins.resize(1 << adc_bits);
    runs = 0;
    baseline = -1;
}

void channel_baseline::end() {
    /*
     * Find the bin with the most values, the signal spent the most time at
     * that voltage. Average a number of bins either side;
     */
    auto max = std::max_element(bins.begin(), bins.end());
    int max_bin = std::distance(bins.begin(), max);
    int from_bin = std::max(max_bin - noise_bins, 0);
    int to_bin = std::min(max_bin + noise_bins, int(bins.size()));
    int sum = 0;
    int samples = 0;
    for (int b = from_bin; b < to_bin; ++b) {
        sum += b * bins[b];
        samples += bins[b];
    }
    baseline = sum / samples;
}

void channel_baseline::update(const hw::adc_trace& trace) {
    ++runs;
    for (auto sample : trace) {
        if (sample > bins.size()) {
            sample = bins.size() - 1;
        }
        ++bins[sample];
    }
}

bool channel_baseline::operator==(const channel_baseline& other) const
{
    return baseline == other.baseline;
}

bool channel_baseline::operator!=(const channel_baseline& other) const
{
    return *this != other.baseline;
}

bool channel_baseline::operator==(const int bl) const
{
    int range = 1;
    if (noise_percent > 0) {
        range = int((1 << adc_bits) * (noise_percent / 100));
    }
    return baseline >= (bl - range) && baseline <= (bl + range);
}

bool channel_baseline::operator!=(const int bl) const
{
    return !(*this == bl);
}

db::db(pixie::channel::channel& module_channel_, const hw::config& config_)
    : channel(module_channel_, config_) {
    pixie::module::module& mod = get_module();
    label = hw::get_module_fixture_label(config_.fixture);
    number = mod.eeprom.db_find(module_channel.number);
    base =  mod.eeprom.db_channel_base(number);
    offset = static_cast<int>(module_channel.number) - base;
    adc_state = adc_boot_state;
}

void db::acquire_adc() {
    pixie::module::module& module = get_module();
    {
        userin_save userins(module);
        userins.update(number, offset);
        hw::run::control_run_on_dsp(module, hw::run::control_task::get_traces);
    }
    /*
     * Make sure the buffer is the maximum size a user can ask for. The
     * allocation is only done if the buffer is used and it cannot be released
     * because the user can ask to read it at any time.
     */
    const size_t size = module_channel.fixture->config.max_adc_trace_length;
    if (module_channel.adc_trace.size() != size) {
        module_channel.adc_trace.resize(size);
    }
    hw::memory::dsp dsp(module);
    hw::adc_trace_buffer adc_trace;
    dsp.read(hw::memory::IO_BUFFER_ADDR, adc_trace, size / 2);
    for (size_t w = 0; w < size / 2; ++w) {
        module_channel.adc_trace[w * 2] = hw::adc_word(adc_trace[w] & 0xffff);
        module_channel.adc_trace[w * 2 + 1] = hw::adc_word((adc_trace[w] >> 16) & 0xffff);
    }
}

void db::read_adc(hw::adc_word* buffer, size_t size) {
    const size_t copy_size =
        (size < module_channel.adc_trace.size() ?
         size : module_channel.adc_trace.size()) * sizeof(hw::adc_word);
    memcpy(buffer, module_channel.adc_trace.data(), copy_size);
}

void db::set(const std::string item, bool value) {
    if (item == "ADC_SWAP") {
        if (adc_state == adc_boot_state) {
            adc_state = value ? adc_swapped : adc_unswapped;
        }
    } else {
        channel::set(item, value);
    }
}

void db::get(const std::string item, bool& value) {
    if (item == "ADC_SWAP") {
        value = (adc_state == adc_swapped);
    } else {
        channel::get(item, value);
    }
}

void db::get(const std::string item, int& value) {
    if (item == "DB_NUMBER") {
        value = number;
    } else if (item == "DB_OFFSET") {
        value = offset;
    } else if (item == "DAC_SETTLE_PERIOD") {
        value = 0;
    } else {
        channel::get(item, value);
    }
}

void db::get(const std::string item, double& value) {
    channel::get(item, value);
}

void db::report(std::ostream& out) const {
    channel::report(out);
    out << "DB Number      : " << number << std::endl
        << "DB Base        : " << base << std::endl
        << "DB Offset      : " << offset << std::endl
        << "ADC swap state : ";
    switch (adc_state) {
    case adc_boot_state:
        out << "boot state";
        break;
    case adc_unswapped:
        out << "not swapped";
        break;
    case adc_swapped:
        out << "swapped";
        break;
    }
    out << std::endl;
}

db04::db04(pixie::channel::channel& module_channel_, const hw::config& config_)
    : db(module_channel_, config_) {
}

void db04::set_dac(param::value_type value) {
    pixie::module::module& mod = get_module();
    if (value > 65535) {
        throw error::error(error::code::invalid_value,
                           pixie::module::module_label(mod, "DB04") + "invalid DAC offset: channel=" +
                           std::to_string(module_channel.number));
    }
    /*
     * Select the module port
     */
    mod.select_port(number + 1);
    /*
     * Address bit 1 selects DAC for the upper 4 channels. Clear bit 0 and set
     * bit 1 if the DB channel offset is less than 4.
     */
    hw::word dac_addr = 0x20 | ((offset < 4 ? 1 : 0) << 1);
    /*
     * Compensate for PCB ADC swapping:
     *
     * Channel offset DAC Output
     *     0, 4           B (1)
     *     1, 5           C (2)
     *     2, 6           A (0)
     *     3, 7           D (3)
     */
    hw::word dac_ctrl = 0x30;
    switch (offset) {
    case 0:
    case 4:
        dac_ctrl += 1;
        break;
    case 1:
    case 5:
        dac_ctrl += 2;
        break;
    case 2:
    case 6:
        dac_ctrl += 0;
        break;
    case 3:
    case 7:
        dac_ctrl += 3;
        break;
    default:
        break;
    }
    /*
     * CFG_DAC expacts [addr(8), ctrl(8), data(16)]
     */
    const hw::word dac = (dac_addr << 24) | (dac_ctrl << 16) | value;
    log(log::debug) << pixie::module::module_label(mod, "fixture: db04")
                    << "db=" << number
                    << " db_channel=" << offset
                    << std::hex
                    << " dac_addr=0x" << dac_addr
                    << " dac_ctrl=0x" << dac_ctrl
                    << " dac_value=0x" << value
                    << " write=0x" << dac;
    mod.write_word(hw::device::CFG_DAC, dac);
     /*
     * It takes about 4ms to clock out the 32 bits
     */
    hw::wait(6000);
}

void db04::get(const std::string item, bool& value) {
    if (item == "HAS_OFFSET_DAC") {
        value = true;
    } else {
        db::get(item, value);
    }
}

void db04::get(const std::string item, int& value) {
    if (item == "DAC_SETTLE_PERIOD") {
        value = dac_settle_time_ms;
    } else {
        db::get(item, value);
    }
}

afe_dbs::afe_dbs(pixie::module::module& module__)
    : module(module__), adcctrl { } {
    log(log::debug) << pixie::module::module_label(module_, "fixture: afe_dbs")
                    << "create";
}

void afe_dbs::fgpa_fippi_loaded() {
    adcctrl = { };
}

void afe_dbs::boot() {
    log(log::debug) << pixie::module::module_label(module_, "fixture: afe_dbs")
                    << "boot";

    util::timepoint tp(true);

    /*
     * Set the voffset for all channels to the low rail
     */
    channel_baselines bl_same;
    set_channel_voffset(module_, -1.5, 1);
    analyze_channel_baselines(module_, bl_same);

    /*
     * Move the voffset for the even channels to high rail
     */
    channel_baselines bl_moved;
    set_channel_voffset(module_, 1.5, 2);
    analyze_channel_baselines(module_, bl_moved);

    /*
     * Check all the channels and swap of the ADCs if required.
     */
    for (int chan = 0; chan < static_cast<int>(module_.num_channels); ++chan) {
        bool swapped = false;
        if ((chan % 2) == 0) {
            if (bl_same[chan] == bl_moved[chan]) {
                swapped = true;
            }
        } else {
            if (bl_same[chan] != bl_moved[chan]) {
                swapped = true;
            }
        }
        module_.channels[chan].fixture->set("ADC_SWAP", swapped);
        if (swapped) {
            int chan_db;
            int chan_offset;
            module_.channels[chan].fixture->get("DB_NUMBER", chan_db);
            module_.channels[chan].fixture->get("DB_OFFSET", chan_offset);
            if (chan_db >= max_dbs) {
                throw pixie::module::error(module_.number, module_.slot,
                                           pixie::module::error::code::module_initialize_failure,
                                           "invalid DB number for channel: " + std::to_string(chan));
            }
            hw::word last_adcctrl = adcctrl[chan_db];
            adcctrl[chan_db] |= 1 << (chan_offset / 2);
            log(log::debug) << pixie::module::module_label(module_, "fixture: afe_dbs")
                            << "boot: adc_swap: db="
                            << chan_db << " offset=" << chan_offset
                            << " adcctrl=0x" << std::hex << adcctrl[chan_db];
            if (adcctrl[chan_db] != last_adcctrl) {
                hw::memory::fippi fippi(module_);
                fippi.write(hw::fippi_addr(chan_db, hw::fippi::ADCCTRL), adcctrl[chan_db]);
            }
        }
    }

    /*
     * Verify.
     */
    bool failed = false;
    if (adc_swap_verify) {
        channel_baselines bl_verify;
        analyze_channel_baselines(module_, bl_verify);
        for (int chan = 0; chan < module_.num_channels; ++chan) {
            if ((chan % 2) == 0) {
                if (bl_same[chan] == bl_verify[chan]) {
                    failed = true;
                    log(log::error) << pixie::module::module_label(module_, "fixture: afe_dbs")
                                    << "boot: ADC swap failed: " << chan;
                }
            } else {
                if (bl_same[chan] != bl_verify[chan]) {
                    failed = true;
                    log(log::error) << pixie::module::module_label(module_, "fixture: afe_dbs")
                                    << "boot: ADC swap failed: " << chan;
                }
            }
        }
    }

    set_channel_voffset(module_, 0, 1);

    if (failed) {
        throw pixie::module::error(module_.number, module_.slot,
                                   pixie::module::error::code::module_initialize_failure,
                                   "DB AE ADC swap failure");
    }

    calc_dac_adc_ratio();

    log(log::debug) << pixie::module::module_label(module_, "fixture: afe_dbs")
                    << "boot: duration=" << tp;
}

void afe_dbs::init_channels() {
    log(log::debug) << pixie::module::module_label(module_, "fixture: afe_dbs")
                    << "init-channels: create channel fixtures";
    for (int chan = 0; chan < static_cast<int>(module_.num_channels); ++chan) {
        module_.channels[chan].fixture =
            fixture::make(module_.channels[chan], module_.eeprom.configs[chan]);
    }
}

void afe_dbs::set_dacs() {
    for (auto& channel : module_.channels) {
        auto dac_offset = module_.read_var(param::channel_var::OffsetDAC, channel.number);
        channel.fixture->set_dac(dac_offset);
    }
}

void afe_dbs::get_traces() {
    for (auto& channel : module_.channels) {
        channel.fixture->acquire_adc();
    }
}

void afe_dbs::adjust_offsets() {
    log(log::debug) << pixie::module::module_label(module_, "fixture: afe_dbs")
                    << "adjust-offsets";

    const double voffset_start_voltage = 0.0;
    const int dac_slope_learn_steps = 200;
    const int linear_fit_samples = 2;
    const int runs = 10;

    util::timepoint tp(true);

    /*
     * Remove any offset
     */
    set_channel_voffset(module_, voffset_start_voltage, 1);

    std::vector<double> bl_percents;
    std::vector<int> offsetdacs;
    std::vector<bool> has_offset_dacs
        ;
    for (auto& channel : module_.channels) {
        bl_percents.push_back(channel.baseline_percent());
        offsetdacs.push_back(
            module_.read_var(param::channel_var::OffsetDAC, channel.number));
        bool has_offset_dac;
        channel.fixture->get("HAS_OFFSET_DAC", has_offset_dac);
        has_offset_dacs.push_back(has_offset_dac);
    }

    using bl_linear_fit = linear_fit<int>;
    std::vector<bl_linear_fit> bl_fits(module_.num_channels);

    bool run_again = true;
    for (int run = 0; run_again && run < runs; ++run) {
        log(log::debug) << pixie::module::module_label(module_, "fixture: afe_dbs")
                        << "adjust-offsets: run=" << run;
        run_again = false;
        channel_baselines baselines;
        analyze_channel_baselines(module_, baselines, 1);
        for (int chan = 0; chan < module_.num_channels; ++chan) {
            auto& channel = module_.channels[chan];
            if (has_offset_dacs[chan]) {
                auto& bl = baselines[chan];
                const int adc_target = int((1 << bl.adc_bits) * (bl_percents[chan] / 100));
                int dac = offsetdacs[chan];
                log(log::debug) << pixie::module::module_label(module_, "fixture: afe_dbs")
                                << "adjust-offsets: channel=" << chan
                                << " adc-target=" << adc_target
                                << " bl=" << bl.baseline
                                << " offset-dac=" << dac;
                /*
                 * The compare includes the noise margin set in the baseline
                 */
                if (bl != adc_target) {
                    bl_linear_fit& bl_fit = bl_fits[chan];
                    bl_fit.update(bl.baseline, dac);
                    if (bl_fit.count < linear_fit_samples) {
                        if (adc_target > bl.baseline) {
                            dac -= dac_slope_learn_steps;
                        } else {
                            dac += dac_slope_learn_steps;
                        }
                    } else {
                        bl_fit.calc();
                        log(log::debug) << pixie::module::module_label(module_, "fixture: afe_dbs")
                                        << "adjust-offsets: update: channel=" << chan
                                        << " " << bl_fit.k << "X + " << bl_fit.c;
                        dac = bl_fit.y(adc_target);
                    }
                    log(log::debug) << pixie::module::module_label(module_, "fixture: afe_dbs")
                                    << "adjust-offsets: update: channel=" << chan
                                    << " adc-error=" << adc_target - bl.baseline
                                    << " dac-error=" << offsetdacs[chan] - dac
                                    << " dac=" << dac;
                    offsetdacs[chan] = dac;
                    channel.fixture->set_dac(dac);
                    run_again = true;
                }
            }
        }
        if (run_again) {
            /*
             * Wait until the signal settles after the update.
             */
            wait_dac_settle_period(module_);
        }
    }
    for (int chan = 0; chan < module_.num_channels; ++chan) {
        module_.write_var(param::channel_var::OffsetDAC, offsetdacs[chan], chan);
    }
    log(log::debug) << pixie::module::module_label(module_, "fixture: afe_dbs")
                    << "adjust-offsets: duration=" << tp;
}

void afe_dbs::calc_dac_adc_ratio() {
    if (dac_adc_ratio) {
        log(log::warning) << pixie::module::module_label(module_, "fixture: afe_dbs")
                          << "dac/adc ratio: delta mapping running";

        const int dac_bits = 16;
        const int dac_step_count = 256;
        const int dac_steps = (1 << dac_bits) / dac_step_count;
        const int dac_delta_threshold = 50;
        const int adc_delta_threshold = 400;

        /*
         * Collect ADC baselines for a range of DAC steps across the DAC step
         * range.
         */
        using dac_step_baselines = std::vector<channel_baselines>;
        dac_step_baselines channel_dac_steps(dac_steps);

        for (int dac_step = 0; dac_step < dac_steps; ++dac_step) {
            const int dac = dac_step * dac_step_count;
            log(log::warning) << pixie::module::module_label(module_, "fixture: afe_dbs")
                              << "dac/adc ratio: dac=" << dac_step;
            for (auto& channel : module_.channels) {
                channel.fixture->set_dac(dac);
            }
            wait_dac_settle_period(module_);
            analyze_channel_baselines(module_, channel_dac_steps[dac_step], 1);
        }

        /*
         * Collect the delta or difference bteween the steps and the average of
         * the steps will produce the ratio for a channel. Average the channels
         * to get a module value.
         */
        using channel_deltas = std::vector<average>;
        using rail = std::vector<int>;

        channel_deltas deltas(module_.num_channels);
        rail bottom_rail(module_.num_channels, 0);
        rail top_rail(module_.num_channels, channel_dac_steps.size() - 1);

        /*
         * Ignore DAC steps that have hit the rails.
         */
        for (int chan = 0; chan < module_.num_channels; ++chan) {
            for (int dac_step = 0; dac_step < dac_steps - 1; ++dac_step) {
                auto bl_0 = channel_dac_steps[dac_step][chan].baseline;
                auto bl_1 = channel_dac_steps[dac_step + 1][chan].baseline;
                auto delta = bl_1 - bl_0;
                if (delta > dac_delta_threshold) {
                    bottom_rail[chan] = dac_step;
                    break;
                }
            }
            for (int dac_step = dac_steps - 2; dac_step >= 0; --dac_step) {
                auto bl_0 = channel_dac_steps[dac_step][chan].baseline;
                auto bl_1 = channel_dac_steps[dac_step + 1][chan].baseline;
                auto delta = bl_1 - bl_0;
                if (delta > dac_delta_threshold) {
                    top_rail[chan] = dac_step;
                    break;
                }
            }
            log(log::warning) << pixie::module::module_label(module_, "fixture: afe_dbs")
                              << "dac/adc ratio: adc rails: channel=" << chan
                              << " bottom=" << bottom_rail[chan] * dac_steps
                              << " top=" << top_rail[chan] * dac_steps;
            for (int dac_step = bottom_rail[chan]; dac_step < top_rail[chan] - 1; ++dac_step) {
                auto bl_0 = channel_dac_steps[dac_step][chan].baseline;
                auto bl_1 = channel_dac_steps[dac_step + 1][chan].baseline;
                auto delta = bl_1 - bl_0;
                deltas[chan].update(delta);
                log(log::warning) << pixie::module::module_label(module_, "fixture: afe_dbs")
                                  << "dac/adc ratio: adc delta: channel=" << chan
                                  << " dac=" << dac_step * dac_step_count
                                  << " adc-bl=" << bl_0
                                  << " delta=" << delta;
            }
            deltas[chan].calc();
        }

        average module_delta;
        average bottom_rail_avg;
        average top_rail_avg;

        for (int chan = 0; chan < module_.num_channels; ++chan) {
            /*
             * Convert the rails to DAC values from the DAC steps we have used.
             */
            bottom_rail[chan] *= dac_step_count;
            top_rail[chan] *= dac_step_count;
            bottom_rail_avg.update(bottom_rail[chan]);
            top_rail_avg.update(top_rail[chan]);
            if (deltas[chan].max < adc_delta_threshold) {
                module_delta.update(deltas[chan].avg);
            }
            log(log::warning) << pixie::module::module_label(module_, "fixture: afe_dbs")
                              << "dac/adc ratio: adc delta: channel=" << chan
                              << " avg=" << deltas[chan].avg << '/' << dac_step_count
                              << " max=" << deltas[chan].max
                              << " min=" << deltas[chan].min
                              << " rails=[" << bottom_rail[chan] << ',' << top_rail[chan] << ']';
        }
        module_delta.calc();
        bottom_rail_avg.calc();
        top_rail_avg.calc();
        log(log::warning) << pixie::module::module_label(module_, "fixture: afe_dbs")
                          << "dac/adc ratio: adc delta: module: serial-num=" << module_.serial_num
                          << " delta-adc: avg=" << module_delta.avg << '/' << dac_step_count
                          << " max=" << module_delta.max
                          << " min=" << module_delta.min;
        log(log::warning) << pixie::module::module_label(module_, "fixture: afe_dbs")
                          << "dac/adc ratio: adc delta: module: serial-num=" << module_.serial_num
                          << " rail: bottom: avg=" << bottom_rail_avg.avg
                          << " max=" << bottom_rail_avg.max
                          << " min=" << bottom_rail_avg.min;
        log(log::warning) << pixie::module::module_label(module_, "fixture: afe_dbs")
                          << "dac/adc ratio: adc delta: module: serial-num=" << module_.serial_num
                          << " rail: top: avg=" << top_rail_avg.avg
                          << " max=" << top_rail_avg.max
                          << " min=" << top_rail_avg.min;
    }
}

channel::channel(pixie::channel::channel& module_channel_, const hw::config& config_)
    : module_channel(module_channel_), label("motherboard"),
      config(config_) {
}

channel::~channel() {
    close();
}

void channel::open() {
    /* Do nothing */
}

void channel::close() {
    /* Do nothing */
}

void channel::set_dac(param::value_type ) {
    unsupported_op("set DAC is using the DSP");
}

void channel::adjust_offsetdac() {
    unsupported_op("adjust offsetdac is using the DSP");
}

void channel::acquire_adc() {
    unsupported_op("ADC acquire is using the DSP");
}

void channel::read_adc(hw::adc_word* , size_t ) {
    unsupported_op("read ADC is using the DSP");
}

void channel::report(std::ostream& out) const {
    out << "Fixture        : " << label << std::endl;
    config.report(out);
}

void channel::set(const std::string item, bool ) {
    unsupported_op("not set support: bool: " + item);
}

void channel::set(const std::string item, int ) {
    unsupported_op("no set support: int: " + item);
}

void channel::set(const std::string item, double ) {
    unsupported_op("no set support: double: " + item);
}

void channel::set(const std::string item, hw::word ) {
    unsupported_op("no set support: hw::word: " + item);
}

void channel::get(const std::string item, bool& ) {
    unsupported_op("no get support: bool: " + item);
}

void channel::get(const std::string item, int& ) {
    unsupported_op("no get support: int: " + item);
}

void channel::get(const std::string item, double& ) {
    unsupported_op("no get support: double: " + item);
}

void channel::get(const std::string item, hw::word& ) {
    unsupported_op("no get support: hw::word: " + item);
}

pixie::module::module& channel::get_module() {
    return module_channel.module;
}

module::module(pixie::module::module& module__)
    : module_(module__), label("none") {
    /* Do nothing */
}

module::~module() {
    /* Do nothing */
}

void module::open() {
    /* Do nothing */
}

void module::close() {
    /* Do nothing */
}

void module::initialize() {
    /* Do nothing */
}

void module::online() {
    /* Do nothing */
}

void module::forced_offline() {
    /* Do nothing */
}

void module::fgpa_comms_loaded() {
    /* Do nothing */
}

void module::fgpa_fippi_loaded() {
    /* Do nothing */
}

void module::dsp_loaded() {
    /* Do nothing */
}

void module::boot() {
    /* Do nothing */
}

void module::erase_values() {
    /* Do nothing */
}

void module::init_values() {
    /* Do nothing */
}

void module::erase_channels() {
    /* Do nothing */
}

void module::init_channels() {
    log(log::debug) << pixie::module::module_label(module_, "fixture: module")
                    << "init-channels: create channel fixtures";
    for (auto chan = 0; chan < module_.num_channels; ++chan) {
        module_.channels[chan].fixture =
            fixture::make(module_.channels[chan], module_.eeprom.configs[chan]);
    }
}

void module::sync_hw() {
    /* Do nothing */
}

void module::sync_vars() {
    /* Do nothing */
}

void module::set_dacs() {
    unsupported_op("set DACs is using the DSP");
}

void module::get_traces() {
    unsupported_op("get traces is using the DSP");
}

void module::adjust_offsets() {
    unsupported_op("adjust offsets is using the DSP");
}

channel_ptr make(pixie::channel::channel& module_channel, const hw::config& config) {
    channel_ptr chan_fixture;
    switch (config.fixture) {
    case hw::module_fixture::DB04:
        chan_fixture = std::make_shared<db04>(module_channel, config);
        break;
    default:
        chan_fixture = std::make_shared<channel>(module_channel, config);
        break;
    }
    chan_fixture->open();
    return chan_fixture;
}

module_ptr make(pixie::module::module& module_) {
    module_ptr mod_fixtures;
    switch (module_.get_rev_tag()) {
    case hw::rev_tag::rev_H:
        mod_fixtures = std::make_shared<afe_dbs>(module_);
        break;
    default:
        mod_fixtures = std::make_shared<module>(module_);
        break;
    }
    return mod_fixtures;
}

};  // namespace fixture
};  // namespace pixie
};  // namespace xia
