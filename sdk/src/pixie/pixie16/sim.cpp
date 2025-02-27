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

/** @file sim.cpp
 * @brief Implements a Pixie-16 simulation framework to facilitate testing
 */

#include <algorithm>
#include <cstring>
#include <fstream>

#include <pixie/log.hpp>
#include <pixie/util.hpp>

#include <pixie/pixie16/sim.hpp>

namespace xia {
namespace pixie {
namespace sim {
module_defs mod_defs;

struct fixture : public xia::pixie::fixture::module {
    fixture(xia::pixie::module::module& module_);
    virtual ~fixture() override;
    virtual void open() override;
    virtual void close() override;
    virtual void initialize() override;
    virtual void online() override;
    virtual void forced_offline() override;
    virtual void fgpa_comms_loaded() override;
    virtual void fgpa_fippi_loaded() override;
    virtual void dsp_loaded() override;
    virtual void boot() override;
    virtual void erase_values() override;
    virtual void init_values() override;
    virtual void erase_channels() override;
    virtual void init_channels() override;
    virtual void sync_hw() override;
    virtual void sync_vars() override;
    virtual void set_dacs() override;
    virtual void get_traces() override;
    virtual void adjust_offsets() override;
    virtual void tau_finder() override;
};

fixture::fixture(xia::pixie::module::module& module__)
    : xia::pixie::fixture::module(module__) {
    label = "sim";
}
fixture::~fixture() {}
void fixture::open() {}
void fixture::close() {}
void fixture::initialize() {}
void fixture::online() {}
void fixture::forced_offline() {}
void fixture::fgpa_comms_loaded() {}
void fixture::fgpa_fippi_loaded() {}
void fixture::dsp_loaded() {}
void fixture::boot() {}
void fixture::erase_values() {}
void fixture::init_values() {}
void fixture::erase_channels() {}
void fixture::init_channels() { xia::pixie::fixture::module::init_channels(); }
void fixture::sync_hw() {}
void fixture::sync_vars() {}
void fixture::set_dacs() {}
void fixture::get_traces() {}
void fixture::adjust_offsets() {}
void fixture::tau_finder() {}

module::module(xia::pixie::backplane::backplane& backplane_) : xia::pixie::module::module(backplane_) {}

module::~module() {}

void module::open(size_t device_number) {
    if (vmaddr != nullptr) {
        throw error(number, slot, error::code::module_already_open, "module has a vaddr");
    }

    for (auto& mod_def : mod_defs) {
        if (mod_def.num_channels != 0 && device_number == mod_def.device_number) {
            xia_log(log::info) << "sim: module: open: device=" << device_number;

            pci_memory = std::make_unique<uint8_t[]>(pci_addr_space_size);
            vmaddr = pci_memory.get();

            std::memset(vmaddr, 0, pci_addr_space_size);

            set_bus_device_number(device_number);
            slot = mod_def.slot;
            revision = mod_def.revision;
            eeprom_format = mod_def.eeprom_format;
            serial_num = mod_def.serial_num;
            num_channels = mod_def.num_channels;
            hw::config config;
            config.adc_bits = mod_def.adc_bits;
            config.adc_msps = mod_def.adc_msps;
            config.adc_clk_div = mod_def.adc_clk_div;
            config.fpga_clk_mhz = mod_def.adc_msps / mod_def.adc_clk_div;
            eeprom.configs.resize(num_channels, config);

            var_defaults = mod_def.var_defaults;

            fixtures = std::make_shared<fixture>(*this);

            present_ = true;
            return;
        }
    }

    throw error(number, slot, error::code::module_initialize_failure, "no device found");
}

void module::close() {
    xia_log(log::info) << "sim: module: close";
    present_ = false;
    vmaddr = nullptr;
    pci_memory.release();
}

void module::probe() {
    xia_log(log::info) << "sim: module: probe";
    online_ = dsp_online = fippi_fpga = comms_fpga = false;
    erase_values();
    erase_channels();
    init_values();
    init_channels();
    online_ = dsp_online = fippi_fpga = comms_fpga = true;
    fixtures->online();
}

void module::boot(bool boot_comms, bool boot_fippi, bool boot_dsp) {
    xia_log(log::info) << "sim: module: boot";
    online_ = false;
    if (boot_comms) {
        comms_fpga = true;
        fixtures->fgpa_comms_loaded();
    }
    if (boot_fippi) {
        fippi_fpga = true;
        fixtures->fgpa_fippi_loaded();
    }
    if (boot_dsp) {
        dsp_online = true;
        fixtures->dsp_loaded();
    }
    init_values();
    init_channels();
    online_ = comms_fpga && fippi_fpga && dsp_online;
}

void module::initialize() {}

void module::init_values() {
    pixie::module::module::init_values();
    if (!var_defaults.empty()) {
        load_var_defaults(var_defaults);
    }
}

void module::load_var_defaults(std::istream& input) {
    for (std::string line; std::getline(input, line);) {
        line = line.substr(0, line.find('#', 0));
        if (!line.empty()) {
            util::trim(line);
            util::strings label_value;
            util::split(label_value, line, '=');
            if (label_value.size() == 2) {
                label_value[1] = label_value[1].substr(0, label_value[1].find('(', 0));
                if (param::is_module_var(label_value[0])) {
                    param::module_var var = param::lookup_module_var(label_value[0]);
                    size_t index = static_cast<size_t>(var);
                    param::value_type value = std::stoul(label_value[1]);
                    module_vars[index].value[0].value = value;
                    module_vars[index].value[0].dirty = true;
                    xia_log(log::debug)
                        << "sim: module: mod var: " << label_value[0] << '=' << label_value[1];
                } else if (param::is_channel_var(label_value[0])) {
                    param::channel_var var = param::lookup_channel_var(label_value[0]);
                    size_t index = static_cast<size_t>(var);
                    param::value_type value = std::stoul(label_value[1]);
                    for (size_t channel = 0; channel < num_channels; ++channel) {
                        channels[channel].vars[index].value[0].value = value;
                        channels[channel].vars[index].value[0].dirty = true;
                    }
                    xia_log(log::debug)
                        << "sim: module: chan var: " << label_value[0] << '=' << label_value[1];
                }
            }
        }
    }
}

void module::load_var_defaults(const std::string& file) {
    xia_log(log::info) << "sim: module: load var defaults: " << file;

    std::ifstream input(file, std::ios::in | std::ios::binary);
    if (!input) {
        throw error(number, slot, error::code::file_read_failure,
                    std::string("module var defaults open: ") + file + ": " + std::strerror(errno));
    }

    load_var_defaults(input);

    input.close();
}

void crate::add_module() {
    xia_log(log::info) << "sim: module: add";
    modules.push_back(std::make_unique<module>(backplane));
}

module_def::module_def()
    : device_number(0), slot(0), revision(0), eeprom_format(0), serial_num(0), num_channels(0),
      adc_bits(0), adc_msps(0), adc_clk_div(0) {}

void load_module_defs(const std::string mod_def_file) {
    xia_log(log::info) << "sim: load module defs: " << mod_def_file;
    std::ifstream input(mod_def_file, std::ios::in | std::ios::binary);
    if (!input) {
        throw error(error::code::file_read_failure, std::string("module def file open: ") +
                                                        mod_def_file + ": " + std::strerror(errno));
    }
    load_module_defs(input);
    input.close();
    xia_log(log::info) << "sim: module defs: " << mod_defs.size();
}

void load_module_defs(std::istream& input) {
    for (std::string line; std::getline(input, line);) {
        if (!line.empty()) {
            add_module_def(line, ',');
        }
    }
}

void add_module_def(const std::string mod_desc, const char delimiter) {
    util::strings fields;
    util::split(fields, mod_desc, delimiter);

    module_def mod_def;

    for (auto field : fields) {
        util::strings label_value;
        util::split(label_value, field, '=');
        if (label_value.size() != 2) {
            throw error(error::code::invalid_value, "invalid module definition: " + field);
        }

        try {
            if (label_value[0] == "device-number") {
                mod_def.device_number = std::stoul(label_value[1]);
            } else if (label_value[0] == "slot") {
                mod_def.slot = std::stoul(label_value[1]);
            } else if (label_value[0] == "revision") {
                mod_def.revision = std::stoul(label_value[1]);
            } else if (label_value[0] == "eeprom-format") {
                mod_def.eeprom_format = std::stoul(label_value[1]);
            } else if (label_value[0] == "serial-num") {
                mod_def.serial_num = std::stoul(label_value[1]);
            } else if (label_value[0] == "num-channels") {
                mod_def.num_channels = std::stoul(label_value[1]);
            } else if (label_value[0] == "adc-bits") {
                mod_def.adc_bits = std::stoul(label_value[1]);
            } else if (label_value[0] == "adc-msps") {
                mod_def.adc_msps = std::stoul(label_value[1]);
            } else if (label_value[0] == "adc-clk-div") {
                mod_def.adc_clk_div = std::stoul(label_value[1]);
            } else if (label_value[0] == "var-defaults") {
                mod_def.var_defaults = label_value[1];
            } else {
                throw error(error::code::invalid_value, "invalid module definition: " + field);
            }
        } catch (error&) {
            throw;
        } catch (...) {
            throw error(error::code::invalid_value,
                        "invalid module definition: bad value: " + label_value[1]);
        }
    }

    xia_log(log::info) << "sim: module desc: add: " << mod_desc;

    mod_defs.push_back(mod_def);
}
}  // namespace sim
}  // namespace pixie
}  // namespace xia
