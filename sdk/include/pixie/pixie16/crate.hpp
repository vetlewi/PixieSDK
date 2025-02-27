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

/** @file crate.hpp
 * @brief Defines functions and data structures related to handling a Pixie-16 crate.
 */

#ifndef PIXIE_CRATE_H
#define PIXIE_CRATE_H

#include <atomic>

#include <pixie/error.hpp>
#include <pixie/fw.hpp>
#include <pixie/os_compat.hpp>

#include <pixie/pixie16/backplane.hpp>
#include <pixie/pixie16/fpga.hpp>
#include <pixie/pixie16/module.hpp>

namespace xia {
namespace pixie {
/**
 * @brief Defines functions and data structures related to handling a Pixie-16 crate.
 */
namespace crate {
/*
 * Crate errors
 */
typedef pixie::error::error error;

/**
 * @brief A crate is a series of slots that contain modules.
 */
struct crate {
private:
    /*
     * Module lock
     */
    using lock_type = std::recursive_mutex;
    using lock_guard = std::lock_guard<lock_type>;

public:
    /**
     * @brief Module lock guard to prevent concurrent access to a module.
     */
    class guard {
        lock_type& lock_;
        lock_guard guard_;

    public:
        guard(crate& crate_);
        ~guard() = default;
    };

    /**
     * @brief Hold an instance while using a module to allow concurrent
     *          access to the crate.
     */
    class user {
        crate& crate_;

    public:
        user(crate& crate_);
        ~user();
    };

    /**
     * Boot settings.
     *
     * Defaults to all true and all modules.
     */
    struct boot_params {
        using range = std::vector<size_t>;

        /**
         * If true (the default) boot or module else boot only the
         * modules offline.
         */
        bool force;
        bool boot_comms; /** Clear and load the COMM FPGA */
        bool boot_fippi; /** Clear and load the FIPPI FPGA */
        bool boot_dsp; /** Reset and load the DSP */
        range modules; /** Range of modules to boot, empty is all */

        boot_params();
    };

    /**
     * Number of modules present in the crate.
     */
    size_t num_modules;

    /**
     * Crate revision
     */
    int revision;

    /**
     * The backplane
     */
    backplane::backplane backplane;

    /**
     * A crate contains a number of modules in slots. These modules are
     * online and ready for use.
     */
    module::modules modules;

    /**
     * The offline modules have been detected by are not online. This lets
     * a user check a crate and determine the state of all detected
     * modules.
     */
    module::modules offline;

    /**
     * Firmware for the crate. Check the modules for the ones they have
     * loaded.
     */
    firmware::crate firmware;

    crate();
    virtual ~crate();

    /**
     * Check the crate has been initialised and ready for use.
     * @throws pixie::error::code::crate_not_ready
     */
    void ready();

    /**
     * @brief Check if the crate is busy?
     * @return True if the crate is busy with another operation.
     */
    bool busy() const;

    /**
     * @brief Checks how many users are using the crate object.
     * @return The number of active users are in the crate when this call is made.
     */
    int users() const;

    /**
     * @brief Range checking operator to index modules based on index types.
     * @tparam T The templated index type
     * @param number The module number from the index
     * @return A pointer to the module object found at the requested index.
     */
    template<typename T>
    module::module& operator[](T number) {
        size_t number_ = static_cast<size_t>(number);
        if (number_ >= num_modules) {
            throw error(pixie::error::code::module_number_invalid,
                        "module number out of range: " + std::to_string(number));
        }
        return *(modules[number_]);
    }

    /**
     * @brief Return a module indexed by a slot.
     * @tparam T The template for various ways to determine the slot.
     * @param slot The physical slot in the crate that we'd like to access.
     * @return A pointer to the module object associated with the physical slot.
     */
    template<typename T>
    module::module_ptr find(T slot) {
        int slot_ = static_cast<int>(slot);
        auto mod = std::find_if(modules.begin(), modules.end(),
                                [slot_](const module::module_ptr m) {
                                    return m->slot == slot_; });
        if (mod == modules.end()) {
            mod = std::find_if(offline.begin(), offline.end(),
                               [slot_](const module::module_ptr m) {
                                   return m->slot == slot_; });
            if (mod == offline.end()) {
                throw error(
                    pixie::error::code::module_number_invalid, "module slot not found");
            }
        }
        return *mod;
    }

    /**
     * @brief Initialise the crate and get it ready.
     * @param reg_trace When true enables enhanced diagnostic information to be output.
     */
    void initialize(bool reg_trace = false);

    /**
     * @brief Shutdown the crate and close all open modules.
     */
    void shutdown();

    /**
     * @brief Mark a module as offline and move to the offline module list.
     *
     * This invalidates any iterators you may hold to the modules and offline
     * containers.
     *
     * @param module Module number.
     */
    void set_offline(const int module);

    /**
     * @brief Mark a module as offline and move to the offline module list.
     *
     * This invalidates any iterators you may hold to the modules and offline
     * containers.
     *
     * @param module Pointer to the module object that should be set offline.
     */
    void set_offline(module::module_ptr module);

    /**
     * @brief Checks if all the modules are online.
     *
     * @return True if all modules are online else false if a module is not online
     */
    bool probe();

    /**
     * @brief Boot all modules by loading the firmware onto the hardware.
     *
     * @param params The crate boot parameters
     */
    void boot(const boot_params& params = boot_params());

    /**
     * @brief Assign numbers to the modules by slot.
     *
     * Modules not in the map are closed or optionally forced offline. Modules
     * not in the slot map by default will be closed and so available for use
     * by another processes.
     *
     * @param numbers A list of slots that should be assigned.
     * @param close Close slots not in the listed else force offline. Default = True.
     */
    void assign(const module::number_slots& numbers, bool close = true);

    /**
     * @brief Associates the firmware with modules in the crate.
     */
    void set_firmware();

    /**
     * @brief Import a configuration. Returning a list of loaded modules.
     * @param[in] json_file The path to the JSON configuration file to load.
     * @param[out] loaded The list of modules that received configurations.
     */
    void import_config(const std::string json_file, module::number_slots& loaded);

    /**
     * @brief Initializes the module's analog front end after importing a configuration.
     * @see xia::pixie::module::sync_hw
     * For each online module, the function executes the xia::pixie::module::sync_hw function with
     * the default values.
     */
    void initialize_afe();

    /**
     * @brief Export the active module configurations to a file.
     * @param json_file Path to the file that will hold the configurations.
     */
    void export_config(const std::string json_file);

    /**
     * @brief Move offline modules from the online list to offline.
     */
    void move_offlines();

    /**
     * @brief Output the crate details.
     */
    void output(std::ostream& out) const;

    /**
     * @brief Report the crate details.
     */
    void report(std::ostream& out) const;

protected:
    virtual void add_module();

private:
    /*
     * Check the module slots.
     */
    void check_slots();

    /*
     * Check the crate revisions all match.
     */
    void check_revision();

    /*
     * Crate lock
     */
    lock_type lock_;

    /*
     * Crate ready.
     */
    std::atomic_bool ready_;

    /*
     * Number of active users in the
     */
    std::atomic_int users_;
};

/**
 * @brief A module handle prevents concurrent access to a module.
 *
 * This allows you to access a module during while operating on it. The crate
 * has this user register and the module is locked while this object exists.
 */
struct module_handle {
    enum checks {
        online,
        present,
        none
    };
    template<typename T> module_handle(crate& crate_, T number,
                                       checks check = online);
    ~module_handle() = default;

    module::module& operator*() {
        return handle;
    }
    module::module* operator->() {
        return &handle;
    }

private:
    module::module& handle;
    crate::user user;
    module::module::guard guard;
};
/**
 * Support a range of indexing types.
 */
template<typename T>
module_handle::module_handle(crate& crate_, T number, checks check)
    : handle(crate_[number]), user(crate_), guard(handle) {
    crate_.ready();
    switch (check) {
    case online:
        if (!handle.online()) {
            throw error(pixie::error::code::module_offline, "module-handle: module not online");
        }
        break;
    case present:
        if (!handle.present()) {
            throw error(pixie::error::code::module_offline, "module-handle: module not present");
        }
        break;
    default:
        break;
    }
}
}  // namespace crate
}  // namespace pixie
}  // namespace xia

/**
 * @brief Output stream operator for crates
 */
std::ostream& operator<<(std::ostream& out, const xia::pixie::crate::crate& crate);

#endif  // PIXIE_CRATE_H
