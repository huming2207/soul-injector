#include "job_decoder.hpp"

#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <pb_decode.h>

static const char *TAG = "job_decoder";

using si::config::assert_level;
using si::config::esp32_image;
using si::config::flash_algorithm;
using si::config::target_config;
using si::config::target_family;
using si::config::test_item;

// The nanopb limits in job.options must match the arrays filled below.
static_assert(sizeof(si_job_Target::variant_name) == sizeof(target_config::variant_name));
static_assert(sizeof(si_job_FlashAlgorithm::name) == sizeof(flash_algorithm::name));
static_assert(sizeof(si_job_Esp32::chip) == sizeof(target_config::chip));
static_assert(sizeof(si_job_Esp32Image::path) == sizeof(esp32_image::path));
static_assert(sizeof(si_job_SelfTest::name) == sizeof(test_item::name));
static_assert(sizeof(si_job_Esp32::images) / sizeof(si_job_Esp32Image) == target_config::MAX_IMAGES);
static_assert(sizeof(si_job_Target::self_tests) / sizeof(si_job_SelfTest) == target_config::MAX_TESTS);
static_assert((int)si_job_SelfTest_Type_TYPE_SIMPLE == (int)test_item::INTERNAL_SIMPLE_TEST);
static_assert((int)si_job_SelfTest_Type_TYPE_EXTEND == (int)test_item::INTERNAL_EXTEND_TEST);
static_assert((int)si_job_SelfTest_Type_TYPE_POWER == (int)test_item::POWER_CONSUMPTION_TEST);

namespace
{

    struct algo_blob {
        uint8_t *buf = nullptr;
        size_t len = 0;
    };

    // -------------------------------------------------------------------
    // Decoding
    // -------------------------------------------------------------------

    bool read_file(pb_istream_t *stream, pb_byte_t *buf, size_t count)
    {
        auto *fp = static_cast<FILE *>(stream->state);
        return fread(buf, 1, count, fp) == count;
    }

    /** Copies the flash algorithm into one PSRAM allocation. */
    bool decode_algo_blob(pb_istream_t *stream, const pb_field_t *field, void **arg)
    {
        (void)field;
        auto *blob = static_cast<algo_blob *>(*arg);
        if (blob->buf != nullptr || stream->bytes_left == 0) {
            return false; // Present at most once and never empty.
        }

        blob->buf = static_cast<uint8_t *>(heap_caps_malloc(stream->bytes_left, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (blob->buf == nullptr) {
            return false;
        }
        blob->len = stream->bytes_left;
        return pb_read(stream, blob->buf, blob->len);
    }

    esp_err_t decode_file(const char *path, si_job_Job &job, algo_blob &blob)
    {
        FILE *fp = fopen(path, "rb");
        if (fp == nullptr) {
            ESP_LOGE(TAG, "cannot open %s", path);
            return ESP_ERR_NOT_FOUND;
        }

        fseek(fp, 0, SEEK_END);
        long file_len = ftell(fp);
        rewind(fp);
        if (file_len <= 0) {
            ESP_LOGE(TAG, "%s is empty", path);
            fclose(fp);
            return ESP_ERR_INVALID_SIZE;
        }

        pb_istream_t stream = {};
        stream.callback = read_file;
        stream.state = fp;
        stream.bytes_left = static_cast<size_t>(file_len);

        // Default initialisation keeps callbacks, so set them before decoding.
        memset(&job, 0, sizeof(job));
        job.target.cortex_m.algorithm.instructions.funcs.decode = decode_algo_blob;
        job.target.cortex_m.algorithm.instructions.arg = &blob;

        bool decoded = pb_decode(&stream, si_job_Job_fields, &job);
        fclose(fp);
        if (!decoded) {
            ESP_LOGE(TAG, "cannot decode %s: %s", path, PB_GET_ERROR(&stream));
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }

    // -------------------------------------------------------------------
    // Validation
    // -------------------------------------------------------------------

    esp_err_t validate_regions(const si_job_Target &target)
    {
        size_t ram_count = 0;
        for (pb_size_t i = 0; i < target.memory_map_count; i++) {
            const si_job_MemoryRegion &region = target.memory_map[i];
            if (region.end <= region.start) {
                ESP_LOGE(TAG, "invalid region 0x%08lx-0x%08lx", region.start, region.end);
                return ESP_ERR_INVALID_SIZE;
            }
            if (region.kind == si_job_MemoryRegion_Kind_KIND_RAM) {
                ram_count++;
            }
        }

        if (ram_count > target_config::MAX_RAM_REGIONS) {
            ESP_LOGE(TAG, "%zu RAM regions exceed the capacity of %zu", ram_count, target_config::MAX_RAM_REGIONS);
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }

    esp_err_t validate_cortex_m(const si_job_Target &target, const algo_blob &blob)
    {
        const si_job_CortexM &cortex_m = target.cortex_m;
        if (!cortex_m.has_algorithm || blob.buf == nullptr) {
            ESP_LOGE(TAG, "cortex-m job has no flash algorithm");
            return ESP_ERR_INVALID_STATE;
        }
        if (cortex_m.algorithm.page_size == 0) {
            ESP_LOGE(TAG, "algorithm '%s': page_size must be non-zero", cortex_m.algorithm.name);
            return ESP_ERR_INVALID_STATE;
        }
        return validate_regions(target);
    }

    esp_err_t validate_esp32(const si_job_Esp32 &esp32)
    {
        if (esp32.chip[0] == '\0' || esp32.baud == 0) {
            ESP_LOGE(TAG, "esp32 job needs a chip name and a baud rate");
            return ESP_ERR_INVALID_STATE;
        }
        if (esp32.images_count == 0) {
            ESP_LOGE(TAG, "esp32 job has no images");
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    esp_err_t validate_self_tests(const si_job_Target &target)
    {
        for (pb_size_t i = 0; i < target.self_tests_count; i++) {
            const si_job_SelfTest_Type type = target.self_tests[i].type;
            if (type < _si_job_SelfTest_Type_MIN || type > _si_job_SelfTest_Type_MAX) {
                ESP_LOGE(TAG, "self_tests[%u]: unknown type %d", (unsigned)i, (int)type);
                return ESP_ERR_INVALID_ARG;
            }
        }
        return ESP_OK;
    }

    /** nanopb leaves which_op at zero when a step carries an unknown op. */
    esp_err_t validate_procedure(const char *name, bool present, const si_job_Procedure &procedure)
    {
        if (!present) {
            return ESP_OK;
        }
        for (pb_size_t i = 0; i < procedure.steps_count; i++) {
            if (procedure.steps[i].which_op == 0) {
                ESP_LOGE(TAG, "%s step %u has no known operation", name, (unsigned)i);
                return ESP_ERR_INVALID_ARG;
            }
        }
        return ESP_OK;
    }

    esp_err_t validate_job(const si_job_Job &job, const algo_blob &blob)
    {
        if (job.schema_version != si_job_SchemaVersion_SCHEMA_VERSION_CURRENT) {
            ESP_LOGE(
                TAG, "job schema %d is not supported (firmware supports %d)", (int)job.schema_version,
                (int)si_job_SchemaVersion_SCHEMA_VERSION_CURRENT
            );
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (!job.has_target || job.target.has_cortex_m == job.target.has_esp32) {
            ESP_LOGE(TAG, "job must describe exactly one target family");
            return ESP_ERR_INVALID_STATE;
        }

        auto ret = validate_self_tests(job.target);
        ret = ret ?: validate_procedure("pre-program", job.has_pre_program, job.pre_program);
        ret = ret ?: validate_procedure("post-program", job.has_post_program, job.post_program);
        if (ret != ESP_OK) {
            return ret;
        }

        if (job.target.has_cortex_m) {
            return validate_cortex_m(job.target, blob);
        }
        return validate_esp32(job.target.esp32);
    }

    // -------------------------------------------------------------------
    // Publishing into target_config
    // -------------------------------------------------------------------

    std::optional<uint32_t> to_optional(bool has_value, uint32_t value)
    {
        if (!has_value) {
            return std::nullopt;
        }
        return value;
    }

    void fill_algorithm(const si_job_FlashAlgorithm &src, const uint8_t *algo_bin, size_t algo_bin_len, flash_algorithm &dst)
    {
        strlcpy(dst.name, src.name, sizeof(dst.name));
        dst.load_address = src.load_address;
        dst.pc_init = to_optional(src.has_pc_init, src.pc_init);
        dst.pc_uninit = to_optional(src.has_pc_uninit, src.pc_uninit);
        dst.pc_program_page = to_optional(src.has_pc_program_page, src.pc_program_page);
        dst.pc_erase_sector = to_optional(src.has_pc_erase_sector, src.pc_erase_sector);
        dst.pc_erase_all = to_optional(src.has_pc_erase_all, src.pc_erase_all);
        dst.pc_verify = to_optional(src.has_pc_verify, src.pc_verify);
        dst.data_section_offset = to_optional(src.has_data_section_offset, src.data_section_offset);
        dst.flash_start = to_optional(src.has_flash_start, src.flash_start);
        dst.flash_end = to_optional(src.has_flash_end, src.flash_end);
        dst.page_size = src.page_size;
        dst.erased_byte_value = to_optional(src.has_erased_byte_value, src.erased_byte_value);
        dst.program_page_timeout_ms = to_optional(src.has_program_page_timeout_ms, src.program_page_timeout_ms);
        dst.erase_sector_timeout_ms = to_optional(src.has_erase_sector_timeout_ms, src.erase_sector_timeout_ms);
        dst.algo_bin = algo_bin;
        dst.algo_bin_len = algo_bin_len;
    }

    void fill_ram_regions(const si_job_Target &src, target_config &cfg)
    {
        cfg.ram_region_count = 0;
        for (pb_size_t i = 0; i < src.memory_map_count; i++) {
            const si_job_MemoryRegion &region = src.memory_map[i];
            if (region.kind != si_job_MemoryRegion_Kind_KIND_RAM) {
                continue;
            }
            cfg.ram_regions[cfg.ram_region_count].start = region.start;
            cfg.ram_regions[cfg.ram_region_count].end = region.end;
            cfg.ram_region_count++;
        }
    }

    void fill_esp32(const si_job_Esp32 &src, target_config &cfg)
    {
        strlcpy(cfg.chip, src.chip, sizeof(cfg.chip));
        cfg.flash_size_kb = to_optional(src.has_flash_size_kb, src.flash_size_kb);
        cfg.baud = src.baud;
        cfg.reset_assert_level = src.reset_assert_high ? assert_level::high : assert_level::low;
        cfg.boot_assert_level = src.boot_assert_high ? assert_level::high : assert_level::low;

        cfg.image_count = src.images_count;
        for (pb_size_t i = 0; i < src.images_count; i++) {
            strlcpy(cfg.images[i].path, src.images[i].path, sizeof(cfg.images[i].path));
            cfg.images[i].offset = src.images[i].offset;
        }
    }

    void clear_cortex_m(target_config &cfg)
    {
        cfg.has_algo = false;
        cfg.algo = {};
        cfg.ram_region_count = 0;
    }

    void clear_esp32(target_config &cfg)
    {
        cfg.chip[0] = '\0';
        cfg.flash_size_kb = std::nullopt;
        cfg.baud = 115200;
        cfg.reset_assert_level = assert_level::low;
        cfg.boot_assert_level = assert_level::low;
        cfg.image_count = 0;
    }

    void fill_self_tests(const si_job_Target &src, target_config &cfg)
    {
        cfg.test_count = src.self_tests_count;
        for (pb_size_t i = 0; i < src.self_tests_count; i++) {
            const si_job_SelfTest &test = src.self_tests[i];
            cfg.tests[i].type = static_cast<decltype(cfg.tests[i].type)>(test.type);
            cfg.tests[i].addr = test.addr;
            strlcpy(cfg.tests[i].name, test.name, sizeof(cfg.tests[i].name));
        }
    }

} // namespace

namespace si::config
{

    esp_err_t decode_job(const char *path, si_job_Job &job, uint8_t **algo_bin_out, size_t *algo_bin_len_out)
    {
        if (path == nullptr || algo_bin_out == nullptr || algo_bin_len_out == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        *algo_bin_out = nullptr;
        *algo_bin_len_out = 0;

        algo_blob blob = {};
        auto ret = decode_file(path, job, blob);
        ret = ret ?: validate_job(job, blob);
        if (ret != ESP_OK) {
            heap_caps_free(blob.buf);
            return ret;
        }

        *algo_bin_out = blob.buf;
        *algo_bin_len_out = blob.len;
        return ESP_OK;
    }

    void fill_target_config(const si_job_Job &job, const uint8_t *algo_bin, size_t algo_bin_len, target_config &cfg)
    {
        const si_job_Target &target = job.target;
        strlcpy(cfg.variant_name, target.variant_name, sizeof(cfg.variant_name));
        fill_self_tests(target, cfg);

        if (target.has_cortex_m) {
            cfg.family = target_family::swd_cortex_m;
            cfg.has_algo = true;
            fill_algorithm(target.cortex_m.algorithm, algo_bin, algo_bin_len, cfg.algo);
            fill_ram_regions(target, cfg);
            clear_esp32(cfg);
        } else {
            cfg.family = target_family::esp32_serial;
            clear_cortex_m(cfg);
            fill_esp32(target.esp32, cfg);
        }

        cfg.generation++;
        ESP_LOGI(
            TAG, "job: family=%s variant='%s' generation=%lu self_tests=%zu", si::config::family_to_str(cfg.family), cfg.variant_name,
            (unsigned long)cfg.generation, cfg.test_count
        );
    }

} // namespace si::config
