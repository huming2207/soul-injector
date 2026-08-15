#pragma once

#include <esp_err.h>

#include "target_config.hpp"

/**
 * Parse /data/target.yaml into a typed, validated target_config.
 *
 * On success @p cfg is fully replaced (generation is preserved and bumped by
 * one) and @p algo_bin_out receives a heap_caps_calloc()'d PSRAM buffer with
 * the decoded flash algorithm; the caller owns that buffer and must free it
 * with heap_caps_free() on the next reload.
 *
 * On failure @p cfg is left untouched, @p algo_bin_out is set to nullptr and
 * the parser has freed anything it allocated itself.
 *
 * Schema (cortex-m family, unchanged from previous releases apart from the
 * optional `family` key):
 *
 *   family: cortex-m          # optional, default
 *   variants:
 *     - name: stm32g0b1
 *       flash_algorithms: [stm32g0]
 *       memory_map:
 *         - !Ram { range: { start: 0x20000000, end: 0x20014400 } }
 *   flash_algorithms:
 *     - name: stm32g0
 *       load_address: 0x20000000
 *       pc_init: 0x1
 *       ...
 *       instructions: <base64>
 *       flash_properties: { address_range: {...}, page_size: ..., ... }
 *   self_tests: [ { type: simple, addr: 0x..., name: ... } ]
 *
 * Schema (esp32 family):
 *
 *   family: esp32
 *   variants:
 *     - name: esp32s31
 *       chip: esp32s31
 *       flash_size_kb: 4096     # optional; absent = detect from target
 *       baud: 921600            # optional
 *       images:
 *         - { path: /data/bootloader.bin, offset: 0x0 }
 *         - { path: /data/partitions.bin, offset: 0x8000 }
 *         - { path: /data/firmware.bin, offset: 0x10000 }
 */
namespace si::config
{
    esp_err_t parse_target_yaml(const char *path, const char *variant_name, target_config &cfg, uint8_t **algo_bin_out);
}
