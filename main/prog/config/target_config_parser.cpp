#include "target_config_parser.hpp"

#include <algorithm>
#include <cstring>

#include <cctype>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "yaml_doc.hpp"

static const char *TAG = "target_cfg";

using si::config::esp32_image;
using si::config::flash_algorithm;
using si::config::ram_region;
using si::config::target_config;
using si::config::target_family;
using si::config::test_item;

namespace
{

    // -------------------------------------------------------------------
    // Variant selection
    // -------------------------------------------------------------------

    esp_err_t select_variant(ryml::ConstNodeRef root, const char *variant_name, ryml::ConstNodeRef &out)
    {
        if (!root.has_child("variants")) {
            ESP_LOGE(TAG, "YAML has no 'variants' key");
            return ESP_ERR_INVALID_STATE;
        }

        ryml::ConstNodeRef variants = root["variants"];
        if (!variants.is_seq() || variants.num_children() == 0) {
            ESP_LOGE(TAG, "'variants' is not a non-empty sequence");
            return ESP_ERR_INVALID_STATE;
        }

        if (variants.num_children() == 1) {
            out = variants[0];
            return ESP_OK;
        }

        if (variant_name == nullptr || variant_name[0] == '\0') {
            ESP_LOGE(TAG, "target.yaml has %zu variants; a variant name is required", (size_t)variants.num_children());
            return ESP_ERR_INVALID_ARG;
        }

        ryml::csubstr wanted = ryml::to_csubstr(variant_name);
        for (auto const &var : variants.children()) {
            if (var.is_map() && var.has_child("name") && var["name"].val() == wanted) {
                out = var;
                return ESP_OK;
            }
        }

        ESP_LOGE(TAG, "requested variant '%s' not found", variant_name);
        return ESP_ERR_NOT_FOUND;
    }

    // -------------------------------------------------------------------
    // Base64 decoding (one calloc'd PSRAM buffer, caller owns it)
    // -------------------------------------------------------------------

    esp_err_t decode_base64(ryml::csubstr b64, uint8_t **out, size_t &out_len)
    {
        // Strip whitespace (block scalars may contain newlines).
        size_t clean_len = 0;
        for (size_t i = 0; i < b64.len; i++) {
            if (!std::isspace(static_cast<unsigned char>(b64.str[i]))) {
                clean_len++;
            }
        }

        if (clean_len == 0) {
            ESP_LOGE(TAG, "instructions: base64 payload is empty");
            return ESP_ERR_INVALID_ARG;
        }
        if (clean_len % 4 == 1) {
            ESP_LOGE(TAG, "instructions: base64 length %zu is impossible (mod 4 == 1)", clean_len);
            return ESP_ERR_INVALID_ARG;
        }

        // Build a whitespace-free copy: block scalars may contain newlines and
        // ryml's base64 decoder does not skip whitespace itself.
        auto *clean = static_cast<char *>(heap_caps_malloc(clean_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (clean == nullptr) {
            ESP_LOGE(TAG, "instructions: cannot allocate staging buffer");
            return ESP_ERR_NO_MEM;
        }
        size_t w = 0;
        for (size_t i = 0; i < b64.len; i++) {
            if (!std::isspace(static_cast<unsigned char>(b64.str[i]))) {
                clean[w++] = b64.str[i];
            }
        }
        clean[w] = '\0';

        size_t decode_need = (clean_len / 4) * 3;
        if (clean_len > 0 && clean[clean_len - 1] == '=')
            decode_need--;
        if (clean_len > 1 && clean[clean_len - 2] == '=')
            decode_need--;
        if (decode_need == 0) {
            ESP_LOGE(TAG, "instructions: decoded size is zero");
            heap_caps_free(clean);
            return ESP_ERR_INVALID_ARG;
        }

        auto *bin = static_cast<uint8_t *>(heap_caps_calloc(1, decode_need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (bin == nullptr) {
            ESP_LOGE(TAG, "instructions: cannot allocate %zu bytes", decode_need);
            heap_caps_free(clean);
            return ESP_ERR_NO_MEM;
        }

        size_t decoded = c4::base64_decode(ryml::csubstr(clean, w), c4::blob(bin, decode_need));
        heap_caps_free(clean);
        if (decoded == 0) {
            ESP_LOGE(TAG, "instructions: base64 decode produced 0 bytes");
            heap_caps_free(bin);
            return ESP_FAIL;
        }

        *out = bin;
        out_len = decoded;
        return ESP_OK;
    }

    // -------------------------------------------------------------------
    // Self tests
    // -------------------------------------------------------------------

    esp_err_t parse_self_tests(ryml::ConstNodeRef root, target_config &cfg)
    {
        if (!root.has_child("self_tests")) {
            return ESP_OK;
        }
        ryml::ConstNodeRef st = root["self_tests"];
        if (!st.is_seq()) {
            ESP_LOGE(TAG, "'self_tests' is not a sequence");
            return ESP_ERR_INVALID_STATE;
        }

        size_t count = st.num_children();
        if (count == 0) {
            return ESP_OK;
        }
        if (count > target_config::MAX_TESTS) {
            ESP_LOGE(TAG, "%zu self tests exceed the capacity of %zu", count, target_config::MAX_TESTS);
            return ESP_ERR_INVALID_SIZE;
        }

        for (size_t i = 0; i < count; i++) {
            ryml::ConstNodeRef item = st[i];
            if (!item.is_map()) {
                ESP_LOGE(TAG, "self_tests[%zu] is not a map", i);
                return ESP_ERR_INVALID_STATE;
            }

            test_item &ti = cfg.tests[i];

            if (item.has_child("type")) {
                ryml::csubstr t = item["type"].val();
                if (t == "simple") {
                    ti.type = test_item::INTERNAL_SIMPLE_TEST;
                } else if (t == "extend") {
                    ti.type = test_item::INTERNAL_EXTEND_TEST;
                } else if (t == "power") {
                    ti.type = test_item::POWER_CONSUMPTION_TEST;
                } else {
                    ESP_LOGW(TAG, "self_tests[%zu]: unknown type '%.*s', defaulting to simple", i, (int)t.len, t.str);
                }
            }

            auto ret = yaml_doc::get_u32(item, "addr", &ti.addr);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "self_tests[%zu]: missing/invalid 'addr'", i);
                return ret;
            }

            if (item.has_child("name")) {
                ret = yaml_doc::get_str(item, "name", ti.name, sizeof(ti.name));
                if (ret != ESP_OK) {
                    return ret;
                }
            }

            ESP_LOGI(TAG, "self_tests[%zu]: addr=0x%08lx name='%s'", i, ti.addr, ti.name);
        }

        cfg.test_count = count;
        return ESP_OK;
    }

    // -------------------------------------------------------------------
    // Cortex-M parsing
    // -------------------------------------------------------------------

    esp_err_t parse_cortex_m(ryml::ConstNodeRef root, ryml::ConstNodeRef variant, target_config &cfg, uint8_t **algo_bin_out)
    {
        if (!variant.has_child("flash_algorithms")) {
            ESP_LOGE(TAG, "variant has no 'flash_algorithms' key");
            return ESP_ERR_INVALID_STATE;
        }
        ryml::ConstNodeRef falgo_list = variant["flash_algorithms"];
        if (!falgo_list.is_seq() || falgo_list.num_children() == 0) {
            ESP_LOGE(TAG, "variant 'flash_algorithms' is empty or invalid");
            return ESP_ERR_INVALID_STATE;
        }
        if (falgo_list.num_children() > 1) {
            ESP_LOGW(TAG, "variant lists %d algorithms; only the first one is used", (int)falgo_list.num_children());
        }

        ryml::csubstr algo_name = falgo_list[0].val();

        if (!root.has_child("flash_algorithms")) {
            ESP_LOGE(TAG, "'flash_algorithms' missing at root level");
            return ESP_ERR_INVALID_STATE;
        }
        ryml::ConstNodeRef top_algos = root["flash_algorithms"];
        if (!top_algos.is_seq()) {
            ESP_LOGE(TAG, "root 'flash_algorithms' is not a sequence");
            return ESP_ERR_INVALID_STATE;
        }

        ryml::ConstNodeRef algo_node;
        bool algo_found = false;
        for (auto const &algo : top_algos.children()) {
            if (algo.is_map() && algo.has_child("name") && algo["name"].val() == algo_name) {
                algo_node = algo;
                algo_found = true;
                break;
            }
        }
        if (!algo_found) {
            ESP_LOGE(TAG, "flash algorithm '%.*s' not found at root level", (int)algo_name.len, algo_name.str);
            return ESP_ERR_NOT_FOUND;
        }

        flash_algorithm &fa = cfg.algo;

        auto ret = yaml_doc::get_str(algo_node, "name", fa.name, sizeof(fa.name));
        if (ret != ESP_OK)
            return ret;

        ret = yaml_doc::get_u32(algo_node, "load_address", &fa.load_address);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "algorithm '%s': missing/invalid 'load_address'", fa.name);
            return ret;
        }

        // Function offsets are relative to load_address.
        struct pc_entry {
            const char *key;
            std::optional<uint32_t> *dst;
        };
        pc_entry pc_entries[] = {
            {"pc_init", &fa.pc_init},
            {"pc_uninit", &fa.pc_uninit},
            {"pc_program_page", &fa.pc_program_page},
            {"pc_erase_sector", &fa.pc_erase_sector},
            {"pc_erase_all", &fa.pc_erase_all},
            {"pc_verify", &fa.pc_verify},
        };
        for (auto &e : pc_entries) {
            std::optional<uint32_t> off;
            ret = yaml_doc::get_opt_u32(algo_node, e.key, off);
            if (ret != ESP_OK)
                return ret;
            if (off.has_value()) {
                *e.dst = fa.load_address + off.value();
            }
        }

        ret = yaml_doc::get_opt_u32(algo_node, "data_section_offset", fa.data_section_offset);
        if (ret != ESP_OK)
            return ret;

        if (algo_node.has_child("flash_properties")) {
            ryml::ConstNodeRef props = algo_node["flash_properties"];
            if (!props.is_map()) {
                ESP_LOGE(TAG, "'flash_properties' is not a map");
                return ESP_ERR_INVALID_STATE;
            }
            if (props.has_child("address_range")) {
                ryml::ConstNodeRef range = props["address_range"];
                uint32_t start = 0, end = 0;
                ret = yaml_doc::get_u32(range, "start", &start);
                ret = ret ?: yaml_doc::get_u32(range, "end", &end);
                if (ret != ESP_OK)
                    return ret;
                fa.flash_start = start;
                fa.flash_end = end;
            }
            ret = yaml_doc::get_opt_u32(props, "page_size", fa.page_size);
            ret = ret ?: yaml_doc::get_opt_u32(props, "erased_byte_value", fa.erased_byte_value);
            ret = ret ?: yaml_doc::get_opt_u32(props, "program_page_timeout", fa.program_page_timeout_ms);
            ret = ret ?: yaml_doc::get_opt_u32(props, "erase_sector_timeout", fa.erase_sector_timeout_ms);
            if (ret != ESP_OK)
                return ret;
        }

        if (!fa.page_size.has_value() || fa.page_size.value() == 0) {
            ESP_LOGE(TAG, "algorithm '%s': 'page_size' is required and must be non-zero", fa.name);
            return ESP_ERR_INVALID_STATE;
        }

        if (!algo_node.has_child("instructions")) {
            ESP_LOGE(TAG, "algorithm '%s': missing 'instructions'", fa.name);
            return ESP_ERR_INVALID_STATE;
        }
        ret = decode_base64(algo_node["instructions"].val(), algo_bin_out, fa.algo_bin_len);
        if (ret != ESP_OK)
            return ret;
        fa.algo_bin = *algo_bin_out;

        ESP_LOGI(
            TAG, "algorithm '%s': load=0x%08lx pc_init=%s pc_erase_all=%s bin=%zu bytes", fa.name, fa.load_address,
            fa.pc_init.has_value() ? "yes" : "no", fa.pc_erase_all.has_value() ? "yes" : "no", fa.algo_bin_len
        );

        cfg.has_algo = true;

        // ---- memory map: keep every !Ram region separate (no fake contiguous span) ----
        if (variant.has_child("memory_map")) {
            ryml::ConstNodeRef mem_map = variant["memory_map"];
            if (!mem_map.is_seq()) {
                ESP_LOGE(TAG, "'memory_map' is not a sequence");
                return ESP_ERR_INVALID_STATE;
            }

            for (auto const &mem : mem_map.children()) {
                if (!mem.is_map() || !yaml_doc::has_tag(mem, "!Ram") || !mem.has_child("range")) {
                    continue;
                }
                if (cfg.ram_region_count >= target_config::MAX_RAM_REGIONS) {
                    ESP_LOGE(TAG, "more than %zu !Ram regions; increase target_config::MAX_RAM_REGIONS", target_config::MAX_RAM_REGIONS);
                    return ESP_ERR_INVALID_SIZE;
                }

                ryml::ConstNodeRef range = mem["range"];
                ram_region &region = cfg.ram_regions[cfg.ram_region_count];
                ret = yaml_doc::get_u32(range, "start", &region.start);
                ret = ret ?: yaml_doc::get_u32(range, "end", &region.end);
                if (ret != ESP_OK)
                    return ret;
                if (region.end <= region.start) {
                    ESP_LOGE(
                        TAG, "invalid RAM region 0x%08lx-0x%08lx: end must be greater than start", region.start, region.end
                    );
                    return ESP_ERR_INVALID_SIZE;
                }
                ESP_LOGD(TAG, "RAM region 0x%08lx-0x%08lx", region.start, region.end);
                cfg.ram_region_count++;
            }

            const ram_region *largest = cfg.largest_ram_region();
            if (largest != nullptr) {
                ESP_LOGI(
                    TAG, "largest RAM region 0x%08lx-0x%08lx (%lu bytes, %zu regions total)", largest->start, largest->end, largest->size(),
                    cfg.ram_region_count
                );
            }
        }

        return ESP_OK;
    }

    // -------------------------------------------------------------------
    // ESP32 parsing
    // -------------------------------------------------------------------

    esp_err_t parse_assert_level(ryml::ConstNodeRef node, const char *key, si::config::assert_level &out)
    {
        if (!node.has_child(key)) {
            return ESP_OK;
        }

        ryml::csubstr val = node[key].val();
        if (val == "low") {
            out = si::config::assert_level::low;
        } else if (val == "high") {
            out = si::config::assert_level::high;
        } else {
            ESP_LOGE(TAG, "control_pins.%s must be 'low' or 'high' (got '%.*s')", key, (int)val.len, val.str);
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }

    esp_err_t parse_esp32(ryml::ConstNodeRef root, ryml::ConstNodeRef variant, target_config &cfg)
    {
        auto ret = yaml_doc::get_str(variant, "chip", cfg.chip, sizeof(cfg.chip));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp32 family requires a 'chip' name (e.g. esp32s31)");
            return ret;
        }

        std::optional<uint32_t> flash_kb;
        ret = yaml_doc::get_opt_u32(variant, "flash_size_kb", flash_kb);
        if (ret != ESP_OK)
            return ret;
        cfg.flash_size_kb = flash_kb;

        std::optional<uint32_t> baud;
        ret = yaml_doc::get_opt_u32(variant, "baud", baud);
        if (ret != ESP_OK)
            return ret;
        cfg.baud = baud.value_or(115200);

        // Board-level control pin polarity. Defaults are already set in the
        // target_config initialisers; only override fields that are present.
        if (root.has_child("control_pins")) {
            ryml::ConstNodeRef pins = root["control_pins"];
            if (!pins.is_map()) {
                ESP_LOGE(TAG, "'control_pins' is not a map");
                return ESP_ERR_INVALID_STATE;
            }
            ret = parse_assert_level(pins, "reset_assert_level", cfg.reset_assert_level);
            ret = ret ?: parse_assert_level(pins, "boot_assert_level", cfg.boot_assert_level);
            if (ret != ESP_OK)
                return ret;
        }

        if (!variant.has_child("images")) {
            ESP_LOGE(TAG, "esp32 family requires an 'images' list");
            return ESP_ERR_INVALID_STATE;
        }
        ryml::ConstNodeRef images = variant["images"];
        if (!images.is_seq() || images.num_children() == 0) {
            ESP_LOGE(TAG, "'images' is empty or invalid");
            return ESP_ERR_INVALID_STATE;
        }

        size_t count = images.num_children();
        if (count > target_config::MAX_IMAGES) {
            ESP_LOGE(TAG, "%zu images exceed the capacity of %zu", count, target_config::MAX_IMAGES);
            return ESP_ERR_INVALID_SIZE;
        }

        for (size_t i = 0; i < count; i++) {
            ryml::ConstNodeRef item = images[i];
            if (!item.is_map()) {
                ESP_LOGE(TAG, "images[%zu] is not a map", i);
                return ESP_ERR_INVALID_STATE;
            }
            ret = yaml_doc::get_str(item, "path", cfg.images[i].path, sizeof(cfg.images[i].path));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "images[%zu]: missing/invalid 'path'", i);
                return ret;
            }
            ret = yaml_doc::get_u32(item, "offset", &cfg.images[i].offset);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "images[%zu]: missing/invalid 'offset'", i);
                return ret;
            }
            ESP_LOGI(TAG, "images[%zu]: %s @ 0x%08lx", i, cfg.images[i].path, cfg.images[i].offset);
        }

        cfg.image_count = count;
        return ESP_OK;
    }

} // namespace

namespace si::config
{

    esp_err_t parse_target_yaml(const char *path, const char *variant_name, target_config &cfg, uint8_t **algo_bin_out)
    {
        if (algo_bin_out == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        *algo_bin_out = nullptr;

        yaml_doc doc;
        auto ret = yaml_doc::load(path, doc);
        if (ret != ESP_OK) {
            return ret;
        }

        ryml::ConstNodeRef root = doc.root();

        // Parse into a temporary; only publish on full success.
        target_config tmp = {};
        tmp.generation = cfg.generation; // preserved, bumped below

        if (root.has_child("family")) {
            ryml::csubstr f = root["family"].val();
            char buf[32];
            size_t len = std::min(f.len, sizeof(buf) - 1);
            memcpy(buf, f.str, len);
            buf[len] = '\0';

            if (!family_from_str(buf, tmp.family)) {
                ESP_LOGE(TAG, "unknown family '%s' (expected cortex-m or esp32)", buf);
                return ESP_ERR_INVALID_ARG;
            }
        }

        ryml::ConstNodeRef variant;
        ret = select_variant(root, variant_name, variant);
        if (ret != ESP_OK) {
            return ret;
        }

        if (variant.has_child("name")) {
            ret = yaml_doc::get_str(variant, "name", tmp.variant_name, sizeof(tmp.variant_name));
            if (ret != ESP_OK) {
                return ret;
            }
        }

        uint8_t *new_algo_bin = nullptr;
        switch (tmp.family) {
        case target_family::swd_cortex_m:
            ret = parse_cortex_m(root, variant, tmp, &new_algo_bin);
            break;
        case target_family::esp32_serial:
            ret = parse_esp32(root, variant, tmp);
            break;
        }
        if (ret != ESP_OK) {
            if (new_algo_bin != nullptr) {
                heap_caps_free(new_algo_bin);
            }
            return ret;
        }

        ret = parse_self_tests(root, tmp);
        if (ret != ESP_OK) {
            if (new_algo_bin != nullptr) {
                heap_caps_free(new_algo_bin);
            }
            return ret;
        }

        tmp.generation = cfg.generation + 1;
        cfg = tmp;
        *algo_bin_out = new_algo_bin;

        ESP_LOGI(
            TAG, "parsed %s: variant '%s', generation %lu, %zu self tests", path, cfg.variant_name, (unsigned long)cfg.generation, cfg.test_count
        );
        return ESP_OK;
    }

} // namespace si::config
