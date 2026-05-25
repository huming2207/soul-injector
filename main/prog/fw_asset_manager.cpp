#define RYML_SINGLE_HDR_DEFINE_NOW
#include <ryml.hpp>

#include <cstring>
#include <esp_log.h>
#include <psa/crypto.h>
#include <esp_heap_caps.h>

#include "fw_asset_manager.hpp"

// -------------------------------------------------------------------
// YAML parsing helpers (same pattern as procedure_executor)
// -------------------------------------------------------------------

static uint32_t parse_yaml_number(ryml::ConstNodeRef node)
{
    if (node.invalid() || node.is_seed()) return 0;
    ryml::csubstr val = node.val();
    char buf[32];
    size_t len = val.len < sizeof(buf) - 1 ? val.len : sizeof(buf) - 1;
    std::memcpy(buf, val.str, len);
    buf[len] = '\0';
    return std::strtoul(buf, nullptr, 0);
}

static bool yaml_node_has_tag(ryml::ConstNodeRef node, const char *tag)
{
    return node.has_val_tag() && node.val_tag() == ryml::to_csubstr(tag);
}

esp_err_t fw_asset_manager::init()
{
    // ---------- free any previous algo binary ----------
    if (algo_bin != nullptr) {
        free(algo_bin);
        algo_bin = nullptr;
        algo_bin_len = 0;
    }

    test_items.clear();
    load_addr = 0;
    pc_init_val = 0;
    pc_uninit_val = 0;
    pc_program_page_val = 0;
    pc_erase_sector_val = 0;
    pc_erase_all_val = 0;
    pc_verify_val = 0;
    data_section_offset_val = 0;
    flash_addr_start = 0;
    flash_addr_end = 0;
    page_sz = 0;
    erased_byte = 0;
    prog_timeout = 0;
    erase_timeout = 0;
    ram_start = 0;
    ram_end = 0;

    // ---------- load the YAML file ----------
    FILE *file = fopen(TARGET_YAML_PATH, "rb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "init: cannot open %s", TARGET_YAML_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        ESP_LOGE(TAG, "init: ftell failed on %s", TARGET_YAML_PATH);
        return ESP_FAIL;
    }
    fseek(file, 0, SEEK_SET);

    auto *file_buf = static_cast<char *>(heap_caps_calloc(1, file_size + 1, MALLOC_CAP_SPIRAM));
    if (file_buf == nullptr) {
        fclose(file);
        ESP_LOGE(TAG, "init: cannot allocate file buffer (%ld bytes)", file_size);
        return ESP_ERR_NO_MEM;
    }

    size_t read_bytes = fread(file_buf, 1, file_size, file);
    fclose(file);

    if (read_bytes == 0 && file_size > 0) {
        ESP_LOGE(TAG, "init: read 0 bytes from %s", TARGET_YAML_PATH);
        free(file_buf);
        return ESP_FAIL;
    }

    // ---------- parse YAML ----------
    ryml::Tree tree;
    try {
        tree = ryml::parse_in_place(ryml::substr(file_buf, read_bytes));
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "init: YAML parse exception: %s", e.what());
        free(file_buf);
        return ESP_FAIL;
    } catch (...) {
        ESP_LOGE(TAG, "init: unknown YAML parse exception");
        free(file_buf);
        return ESP_FAIL;
    }

    ryml::ConstNodeRef root = tree.rootref();
    if (!root.is_map()) {
        ESP_LOGE(TAG, "init: YAML root is not a map");
        free(file_buf);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    // ---------- locate the first variant with flash algorithms ----------
    if (!root.has_child("variants")) {
        ESP_LOGE(TAG, "init: YAML has no 'variants' key");
        free(file_buf);
        return ESP_ERR_INVALID_STATE;
    }

    ryml::ConstNodeRef variants = root["variants"];
    if (!variants.is_seq()) {
        ESP_LOGE(TAG, "init: 'variants' is not a sequence");
        free(file_buf);
        return ESP_ERR_INVALID_STATE;
    }

    ryml::ConstNodeRef algo_node;
    bool algo_found = false;

    for (auto const &var : variants.children()) {
        if (!var.is_map()) continue;
        if (!var.has_child("flash_algorithms")) continue;

        ryml::ConstNodeRef falgo_list = var["flash_algorithms"];
        if (!falgo_list.is_seq() || falgo_list.num_children() == 0) continue;

        // Take the first algorithm name
        ryml::csubstr algo_name = falgo_list[0].val();

        // Look it up in the top-level flash_algorithms list
        if (!root.has_child("flash_algorithms")) {
            ESP_LOGE(TAG, "init: 'flash_algorithms' missing at root level");
            free(file_buf);
            return ESP_ERR_INVALID_STATE;
        }

        ryml::ConstNodeRef top_algos = root["flash_algorithms"];
        if (!top_algos.is_seq()) {
            ESP_LOGE(TAG, "init: root 'flash_algorithms' is not a sequence");
            free(file_buf);
            return ESP_ERR_INVALID_STATE;
        }

        for (auto const &algo : top_algos.children()) {
            if (!algo.is_map()) continue;
            if (!algo.has_child("name")) continue;
            if (algo["name"].val() == algo_name) {
                algo_node = algo;
                algo_found = true;
                break;
            }
        }

        if (algo_found) {
            ESP_LOGI(TAG, "init: using flash algorithm '%s'", algo_name.str, algo_name.len);
            break;
        }
    }

    if (!algo_found) {
        ESP_LOGE(TAG, "init: no usable flash algorithm found in YAML");
        free(file_buf);
        return ESP_ERR_NOT_FOUND;
    }

    // ---------- parse algorithm fields ----------
    if (!algo_node.has_child("instructions")) {
        ESP_LOGE(TAG, "init: algorithm entry missing 'instructions'");
        free(file_buf);
        return ESP_ERR_INVALID_STATE;
    }

    ryml::csubstr instr_b64 = algo_node["instructions"].val();

    load_addr            = parse_yaml_number(algo_node["load_address"]);

    auto get_offset = [&](const char *key) -> uint32_t {
        if (!algo_node.has_child(key)) return 0;
        return load_addr + parse_yaml_number(algo_node[key]);
    };

    pc_init_val             = get_offset("pc_init");
    pc_uninit_val           = get_offset("pc_uninit");
    pc_program_page_val     = get_offset("pc_program_page");
    pc_erase_sector_val     = get_offset("pc_erase_sector");
    pc_erase_all_val        = get_offset("pc_erase_all");
    pc_verify_val           = get_offset("pc_verify");
    data_section_offset_val = get_offset("data_section_offset");

    ESP_LOGI(TAG, "init: load_addr=0x%08lx pc_init=0x%08lx pc_uninit=0x%08lx pc_prog=0x%08lx pc_erase_sector=0x%08lx pc_erase_all=0x%08lx pc_verify=0x%08lx data_off=0x%08lx",
             load_addr, pc_init_val, pc_uninit_val, pc_program_page_val,
             pc_erase_sector_val, pc_erase_all_val, pc_verify_val, data_section_offset_val);

    // ---------- flash_properties ----------
    if (algo_node.has_child("flash_properties")) {
        ryml::ConstNodeRef props = algo_node["flash_properties"];

        if (props.has_child("address_range")) {
            flash_addr_start = parse_yaml_number(props["address_range"]["start"]);
            flash_addr_end   = parse_yaml_number(props["address_range"]["end"]);
        }

        page_sz      = parse_yaml_number(props["page_size"]);
        erased_byte  = parse_yaml_number(props["erased_byte_value"]);
        prog_timeout = parse_yaml_number(props["program_page_timeout"]);
        erase_timeout = parse_yaml_number(props["erase_sector_timeout"]);

        ESP_LOGI(TAG, "init: flash 0x%08lx-0x%08lx page=%lu erased=0x%02lx prog_to=%lu erase_to=%lu",
                 flash_addr_start, flash_addr_end, page_sz, erased_byte, prog_timeout, erase_timeout);
    }

    // ---------- decode base64 instructions ----------
    // Strip whitespace from Base64 (it may contain newlines if it was a block scalar)
    std::string clean_b64;
    clean_b64.reserve(instr_b64.len);
    for (size_t i = 0; i < instr_b64.len; ++i) {
        if (!std::isspace(static_cast<unsigned char>(instr_b64[i]))) {
            clean_b64 += instr_b64[i];
        }
    }

    size_t clean_len = clean_b64.length();
    size_t decode_need = (clean_len / 4) * 3;
    if (clean_len > 0 && clean_b64[clean_len - 1] == '=') decode_need--;
    if (clean_len > 1 && clean_b64[clean_len - 2] == '=') decode_need--;

    uint8_t *temp_algo_bin = static_cast<uint8_t *>(heap_caps_calloc(1, decode_need, MALLOC_CAP_SPIRAM));
    if (temp_algo_bin == nullptr) {
        ESP_LOGE(TAG, "init: cannot allocate %zu bytes for algo binary", decode_need);
        free(file_buf);
        return ESP_ERR_NO_MEM;
    }

    size_t decoded = c4::base64_decode(ryml::to_csubstr(clean_b64.c_str()), c4::blob(temp_algo_bin, decode_need));
    if (decoded == 0) {
        ESP_LOGE(TAG, "init: base64 decode produced 0 bytes");
        free(temp_algo_bin);
        free(file_buf);
        return ESP_FAIL;
    }
    size_t temp_algo_bin_len = decoded;
    ESP_LOGI(TAG, "init: decoded algo binary: %zu bytes", temp_algo_bin_len);

    // ---------- parse memory_map for Ram regions ----------
    uint32_t temp_ram_start = 0;
    uint32_t temp_ram_end = 0;

    if (root.has_child("variants")) {
        // Walk variants again to find memory_map (use the same variant we found the algo from)
        for (auto const &var : variants.children()) {
            if (!var.is_map()) continue;
            if (!var.has_child("flash_algorithms")) continue;
            if (!var.has_child("memory_map")) continue;

            ryml::ConstNodeRef mem_map = var["memory_map"];
            if (!mem_map.is_seq()) continue;

            uint32_t first_ram_start = UINT32_MAX;
            uint32_t last_ram_end = 0;

            for (auto const &mem : mem_map.children()) {
                if (!mem.is_map()) continue;
                if (!yaml_node_has_tag(mem, "!Ram")) continue;
                if (!mem.has_child("range")) continue;

                uint32_t rs = parse_yaml_number(mem["range"]["start"]);
                uint32_t re = parse_yaml_number(mem["range"]["end"]);

                if (rs < first_ram_start) first_ram_start = rs;
                if (re > last_ram_end)     last_ram_end   = re;

                ESP_LOGD(TAG, "init: RAM region 0x%08lx - 0x%08lx", rs, re);
            }

            if (first_ram_start != UINT32_MAX && last_ram_end > 0) {
                temp_ram_start = first_ram_start;
                temp_ram_end   = last_ram_end;
                ESP_LOGI(TAG, "init: RAM 0x%08lx - 0x%08lx (size %lu)", temp_ram_start, temp_ram_end, temp_ram_end - temp_ram_start);
            }
            break; // Only the variant we picked
        }

        // If we didn't find Ram via the first matching variant, try all variants
        if (temp_ram_start == 0 && temp_ram_end == 0) {
            for (auto const &var : variants.children()) {
                if (!var.is_map()) continue;
                if (!var.has_child("memory_map")) continue;
                ryml::ConstNodeRef mem_map = var["memory_map"];
                if (!mem_map.is_seq()) continue;

                uint32_t first_ram_start = UINT32_MAX;
                uint32_t last_ram_end = 0;

                for (auto const &mem : mem_map.children()) {
                    if (!mem.is_map()) continue;
                    if (!yaml_node_has_tag(mem, "!Ram")) continue;
                    if (!mem.has_child("range")) continue;

                    uint32_t rs = parse_yaml_number(mem["range"]["start"]);
                    uint32_t re = parse_yaml_number(mem["range"]["end"]);
                    if (rs < first_ram_start) first_ram_start = rs;
                    if (re > last_ram_end)     last_ram_end   = re;
                }

                if (first_ram_start != UINT32_MAX && last_ram_end > 0) {
                    temp_ram_start = first_ram_start;
                    temp_ram_end   = last_ram_end;
                    ESP_LOGI(TAG, "init: RAM 0x%08lx - 0x%08lx (from alt variant)", temp_ram_start, temp_ram_end);
                    break;
                }
            }
        }
    }

    // ---------- Commit changes to member variables ----------
    algo_bin = temp_algo_bin;
    algo_bin_len = temp_algo_bin_len;
    ram_start = temp_ram_start;
    ram_end = temp_ram_end;

    // ---------- parse self_tests ----------
    if (root.has_child("self_tests")) {
        ryml::ConstNodeRef st_node = root["self_tests"];
        if (st_node.is_seq()) {
            for (auto const &item : st_node.children()) {
                if (!item.is_map()) continue;

                flash_algo::test_item ti = {};
                ti.type = flash_algo::INTERNAL_SIMPLE_TEST;

                if (item.has_child("type")) {
                    ryml::csubstr t = item["type"].val();
                    if (t == "internal")       ti.type = flash_algo::INTERNAL_SIMPLE_TEST;
                    else if (t == "extend")    ti.type = flash_algo::INTERNAL_EXTEND_TEST;
                    else if (t == "external")  ti.type = flash_algo::EXTERNAL_TEST;
                }

                ti.id = static_cast<uint16_t>(parse_yaml_number(item["id"]));

                if (item.has_child("name")) {
                    std::memset(ti.name, 0, sizeof(ti.name));
                    ryml::csubstr n = item["name"].val();
                    size_t cp = n.len < sizeof(ti.name) - 1 ? n.len : sizeof(ti.name) - 1;
                    std::memcpy(ti.name, n.str, cp);
                    ti.name[sizeof(ti.name) - 1] = '\0';
                }

                ESP_LOGI(TAG, "init: self-test id=%u name='%s' type=%u", ti.id, ti.name, ti.type);
                test_items.emplace_back(ti);
            }
        }
    }

    free(file_buf);
    return ESP_OK;
}

// -------------------------------------------------------------------
// Public getters
// -------------------------------------------------------------------

esp_err_t fw_asset_manager::get_algo_bin(uint8_t *algo, size_t len, size_t *actual_len, uint32_t *start_addr)
{
    if (algo == nullptr || len == 0) {
        if (actual_len != nullptr) *actual_len = algo_bin_len;
        if (start_addr != nullptr) *start_addr = load_addr;
        return (algo_bin_len > 0) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    size_t copy = (algo_bin_len < len) ? algo_bin_len : len;
    std::memcpy(algo, algo_bin, copy);

    if (actual_len != nullptr) *actual_len = copy;
    if (start_addr != nullptr) *start_addr = load_addr;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_ram_start_addr(uint32_t *out) const
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = ram_start;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_ram_size_byte(uint32_t *out) const
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = ram_end - ram_start;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_flash_size_byte(uint32_t *out)
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = flash_addr_end - flash_addr_start;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_pc_init(uint32_t *out)
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    if (pc_init_val == 0) return ESP_ERR_NOT_FOUND;
    *out = pc_init_val;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_pc_uninit(uint32_t *out)
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    if (pc_uninit_val == 0) return ESP_ERR_NOT_FOUND;
    *out = pc_uninit_val;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_pc_program_page(uint32_t *out)
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    if (pc_program_page_val == 0) return ESP_ERR_NOT_FOUND;
    *out = pc_program_page_val;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_pc_erase_sector(uint32_t *out)
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    if (pc_erase_sector_val == 0) return ESP_ERR_NOT_FOUND;
    *out = pc_erase_sector_val;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_pc_erase_all(uint32_t *out)
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    if (pc_erase_all_val == 0) return ESP_ERR_NOT_FOUND;
    *out = pc_erase_all_val;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_pc_verify(uint32_t *out)
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    if (pc_verify_val == 0) return ESP_ERR_NOT_FOUND;
    *out = pc_verify_val;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_data_section_offset(uint32_t *out)
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    if (data_section_offset_val == 0) return ESP_ERR_NOT_FOUND;
    *out = data_section_offset_val;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_flash_start_addr(uint32_t *out) const
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = flash_addr_start;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_flash_end_addr(uint32_t *out) const
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = flash_addr_end;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_page_size(uint32_t *out) const
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = page_sz;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_erased_byte_val(uint32_t *out) const
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = erased_byte;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_program_page_timeout(uint32_t *out) const
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = prog_timeout;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_erase_sector_timeout(uint32_t *out) const
{
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = erase_timeout;
    return ESP_OK;
}

esp_err_t fw_asset_manager::get_sector_size(uint32_t *out) const
{
    // The old code returned dev_descr.page_size here (which was confusingly named sector_size).
    // Keep the same behaviour for compatibility: return page_sz.
    if (out == nullptr) return ESP_ERR_INVALID_ARG;
    *out = page_sz;
    return ESP_OK;
}

std::vector<flash_algo::test_item> &fw_asset_manager::get_test_items()
{
    return test_items;
}

// -------------------------------------------------------------------
// Static helpers (unchanged from original)
// -------------------------------------------------------------------

bool fw_asset_manager::check_fw_bin_hash(uint8_t *sha_expected, size_t len)
{
    if (len < 32 || sha_expected == nullptr) {
        ESP_LOGE(TAG, "Invalid or incomplete SHA2! len=%u", len);
        return false;
    }

    uint8_t sha_actual[32] = {};
    auto ret = get_sha256_from_file(FIRMWARE_PATH, sha_actual);
    if (ret != ESP_OK) return false;

    return std::memcmp(sha_actual, sha_expected, std::min(sizeof(sha_actual), len)) == 0;
}

bool fw_asset_manager::check_algo_bin_hash(uint8_t *sha_expected, size_t len)
{
    if (len < 32 || sha_expected == nullptr) {
        ESP_LOGE(TAG, "Invalid or incomplete SHA2! len=%u", len);
        return false;
    }

    uint8_t sha_actual[32] = {};
    auto ret = get_sha256_from_file(TARGET_YAML_PATH, sha_actual);
    if (ret != ESP_OK) return false;

    return std::memcmp(sha_actual, sha_expected, std::min(sizeof(sha_actual), len)) == 0;
}

esp_err_t fw_asset_manager::get_sha256_from_file(const char *path, uint8_t *out)
{
    if (path == nullptr) return ESP_ERR_INVALID_ARG;

    FILE *fp = fopen(path, "r+");
    if (fp == nullptr) {
        ESP_LOGE(TAG, "Can't open file at %s", path);
        return ESP_ERR_INVALID_STATE;
    }

    fseek(fp, 0, SEEK_END);
    size_t file_len = ftell(fp);
    rewind(fp);

    psa_hash_operation_t operation = psa_hash_operation_init();
    size_t out_len;

    if (psa_hash_setup(&operation, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PSA hash setup failed");
        fclose(fp);
        return ESP_FAIL;
    }

    size_t pos = 0;
    while (pos < file_len) {
        uint8_t blk[512] = {};
        size_t read_len = std::min(sizeof(blk), file_len - pos);

        size_t actual_read = fread(blk, 1, read_len, fp);
        if (actual_read < 1) {
            ESP_LOGE(TAG, "Failed to read stuff");
            break;
        }

        if (psa_hash_update(&operation, blk, read_len) != PSA_SUCCESS) {
            ESP_LOGE(TAG, "Failed to update SHA256 digest");
            fclose(fp);
            psa_hash_abort(&operation);
            return ESP_FAIL;
        }

        pos += actual_read;
    }

    if (psa_hash_finish(&operation, out, 32, &out_len) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to finalise SHA256 digest");
        psa_hash_abort(&operation);
        fclose(fp);
        return ESP_FAIL;
    }

    fclose(fp);
    return ESP_OK;
}
