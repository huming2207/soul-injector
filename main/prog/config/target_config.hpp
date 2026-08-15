#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace si::config
{

    /** Target programming family. Selected by the `family` key in target.yaml. */
    enum class target_family : uint8_t {
        /** ARM Cortex-M programmed over SWD with a probe-rs style flash algorithm. */
        swd_cortex_m = 0,
        /** Espressif target programmed over UART with esp-serial-flasher. */
        esp32_serial = 1,
    };

    inline const char *family_to_str(target_family f)
    {
        switch (f) {
        case target_family::swd_cortex_m:
            return "cortex-m (SWD)";
        case target_family::esp32_serial:
            return "esp32 (serial)";
        }
        return "unknown";
    }

    /**
 * Parse a family string from YAML. Accepts the spellings below; nullptr
 * (missing key) maps to swd_cortex_m for backward compatibility with
 * every existing target.yaml.
 */
    inline bool family_from_str(const char *str, target_family &out)
    {
        if (str == nullptr || str[0] == '\0') {
            out = target_family::swd_cortex_m;
            return true;
        }
        if (strcmp(str, "cortex-m") == 0 || strcmp(str, "swd") == 0 || strcmp(str, "arm") == 0) {
            out = target_family::swd_cortex_m;
            return true;
        }
        if (strcmp(str, "esp32") == 0 || strcmp(str, "esp32-serial") == 0 || strcmp(str, "espressif") == 0) {
            out = target_family::esp32_serial;
            return true;
        }
        return false;
    }

    /** Self test entry (top-level `self_tests` in target.yaml). */
    struct test_item {
        enum type : int32_t {
            INTERNAL_SIMPLE_TEST = 0,
            INTERNAL_EXTEND_TEST = 1,
            POWER_CONSUMPTION_TEST = 2,
        };

        type type = INTERNAL_SIMPLE_TEST;
        uint32_t addr = 0;
        char name[32] = {};
    };

    /** One contiguous RAM region from the variant memory_map (`!Ram` tagged). */
    struct ram_region {
        uint32_t start = 0;
        uint32_t end = 0;

        uint32_t size() const
        {
            return end - start;
        }

        bool contains(uint32_t addr) const
        {
            return addr >= start && addr < end;
        }

        /** True when [addr, addr+len) lies entirely inside this region, without overflow. */
        bool contains_range(uint32_t addr, uint32_t len) const
        {
            return addr >= start && len <= end - addr;
        }
    };

    /** One flash image for esp32 family targets (bootloader, partitions, app, ...). */
    struct esp32_image {
        char path[64] = {};  // e.g. "/data/bootloader.bin"
        uint32_t offset = 0; // flash offset, e.g. 0x0, 0x8000, 0x10000
    };

    /**
 * Parsed probe-rs style flash algorithm (cortex-m family only).
 *
 * All optional values use std::optional so "absent" and "zero" are distinct;
 * the old code used 0 as a not-found sentinel which made valid zero offsets
 * (e.g. pc_erase_all: 0) indistinguishable from missing configuration.
 *
 * algo_bin points at a PSRAM buffer owned by fw_asset_manager; it stays valid
 * until the next config reload and must not be freed here.
 */
    struct flash_algorithm {
        char name[32] = {};

        uint32_t load_address = 0;

        // Function entry points (load_address + offset from YAML).
        std::optional<uint32_t> pc_init;
        std::optional<uint32_t> pc_uninit;
        std::optional<uint32_t> pc_program_page;
        std::optional<uint32_t> pc_erase_sector;
        std::optional<uint32_t> pc_erase_all;
        std::optional<uint32_t> pc_verify;

        // Raw static-base value for the algorithm syscall.
        std::optional<uint32_t> data_section_offset;

        // flash_properties
        std::optional<uint32_t> flash_start; // address_range start
        std::optional<uint32_t> flash_end;   // address_range end
        std::optional<uint32_t> page_size;
        std::optional<uint32_t> erased_byte_value;
        std::optional<uint32_t> program_page_timeout_ms;
        std::optional<uint32_t> erase_sector_timeout_ms;

        const uint8_t *algo_bin = nullptr;
        size_t algo_bin_len = 0;
    };

    /**
 * Fully parsed target.yaml.
 *
 * Everything is fixed-size inline storage: the struct is trivially copyable
 * and needs no cleanup except the algo_bin buffer, which fw_asset_manager
 * allocates with heap_caps_calloc() at parse time and frees on the next
 * reload. Only the fields relevant for the selected family are populated.
 */
    struct target_config {
        static constexpr size_t MAX_RAM_REGIONS = 4;
        static constexpr size_t MAX_IMAGES = 8;
        static constexpr size_t MAX_TESTS = 16;

        target_family family = target_family::swd_cortex_m;
        char variant_name[32] = {};

        // ---- cortex-m family ----
        flash_algorithm algo = {};
        bool has_algo = false;
        ram_region ram_regions[MAX_RAM_REGIONS] = {};
        size_t ram_region_count = 0;

        // ---- esp32 family ----
        char chip[24] = {}; // target chip name, e.g. "esp32s31"
        esp32_image images[MAX_IMAGES] = {};
        size_t image_count = 0;
        std::optional<uint32_t> flash_size_kb; // absent = detect from the target
        uint32_t baud = 115200;                // programming baud rate

        // ---- common ----
        test_item tests[MAX_TESTS] = {};
        size_t test_count = 0;

        /** Bumped by one on every successful parse. Lets long-lived code detect stale config. */
        uint32_t generation = 0;

        /**
         * The RAM region that hosts the flash algorithm, selected by
         * containment: it must hold the algorithm blob at load_address and
         * (when present) contain data_section_offset, i.e. the algorithm's code
         * and static data live there. Selecting by "largest region" is wrong
         * when a bigger separate bank exists - the algorithm would be
         * validated against RAM it never occupies.
         *
         * The caller (swd_prog) additionally verifies the region can hold the
         * stack and page buffers above the algorithm. Returns nullptr when no
         * region contains the algorithm blob.
         */
        const ram_region *algo_ram_region() const
        {
            if (!has_algo) {
                return nullptr;
            }
            for (size_t i = 0; i < ram_region_count; i++) {
                const ram_region &r = ram_regions[i];
                if (!r.contains_range(algo.load_address, algo.algo_bin_len)) {
                    continue;
                }
                if (algo.data_section_offset.has_value() && !r.contains(algo.data_section_offset.value())) {
                    // Code fits but static base lives elsewhere (e.g. a separate
                    // DTCM bank); still usable - swd_prog logs a warning.
                    return &r;
                }
                return &r;
            }
            return nullptr;
        }

        /**
         * Largest contiguous !Ram region, or nullptr. Kept for diagnostics;
         * algorithm placement must go through algo_ram_region().
         */
        const ram_region *largest_ram_region() const
        {
            const ram_region *best = nullptr;
            for (size_t i = 0; i < ram_region_count; i++) {
                if (best == nullptr || ram_regions[i].size() > best->size()) {
                    best = &ram_regions[i];
                }
            }
            return best;
        }
    };

} // namespace si::config
