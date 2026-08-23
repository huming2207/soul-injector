#include "esp32_serial_backend.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "fw_asset_manager.hpp"

namespace
{
    const si::config::target_config &esp32_control_cfg()
    {
        return fw_asset_manager::instance()->config();
    }

    void esp32_backend_reset_target(esp_loader_port_t *port)
    {
        esp32_port_t *p = container_of(port, esp32_port_t, port);
        const si::config::target_config &cfg = esp32_control_cfg();
        const int assert = cfg.reset_assert_level == si::config::assert_level::high ? 1 : 0;
        const int deassert = cfg.reset_assert_level == si::config::assert_level::high ? 0 : 1;

        gpio_set_level(p->reset_pin, assert);
        vTaskDelay(pdMS_TO_TICKS(SERIAL_FLASHER_RESET_HOLD_TIME_MS));
        gpio_set_level(p->reset_pin, deassert);
    }

    void esp32_backend_enter_bootloader(esp_loader_port_t *port)
    {
        esp32_port_t *p = container_of(port, esp32_port_t, port);
        const si::config::target_config &cfg = esp32_control_cfg();
        const int boot_assert = cfg.boot_assert_level == si::config::assert_level::high ? 1 : 0;
        const int boot_deassert = cfg.boot_assert_level == si::config::assert_level::high ? 0 : 1;
        const int reset_assert = cfg.reset_assert_level == si::config::assert_level::high ? 1 : 0;
        const int reset_deassert = cfg.reset_assert_level == si::config::assert_level::high ? 0 : 1;

        gpio_set_level(p->boot_pin, boot_assert);
        gpio_set_level(p->reset_pin, reset_assert);
        vTaskDelay(pdMS_TO_TICKS(SERIAL_FLASHER_RESET_HOLD_TIME_MS));
        gpio_set_level(p->reset_pin, reset_deassert);
        vTaskDelay(pdMS_TO_TICKS(SERIAL_FLASHER_BOOT_HOLD_TIME_MS));
        gpio_set_level(p->boot_pin, boot_deassert);
    }
} // namespace

static const esp_loader_port_ops_t esp32_uart_ops_custom = {
    .init = esp32_uart_ops.init,
    .deinit = esp32_uart_ops.deinit,
    .enter_bootloader = esp32_backend_enter_bootloader,
    .reset_target = esp32_backend_reset_target,
    .start_timer = esp32_uart_ops.start_timer,
    .remaining_time = esp32_uart_ops.remaining_time,
    .delay_ms = esp32_uart_ops.delay_ms,
    .log = esp32_uart_ops.log,
    .log_hex = esp32_uart_ops.log_hex,
    .change_transmission_rate = esp32_uart_ops.change_transmission_rate,
    .write = esp32_uart_ops.write,
    .read = esp32_uart_ops.read,
    .spi_set_cs = esp32_uart_ops.spi_set_cs,
    .sdio_write = esp32_uart_ops.sdio_write,
    .sdio_read = esp32_uart_ops.sdio_read,
    .sdio_card_init = esp32_uart_ops.sdio_card_init,
};

static esp_err_t loader_err_to_esp(esp_loader_error_t err)
{
    switch (err) {
    case ESP_LOADER_SUCCESS:
        return ESP_OK;
    case ESP_LOADER_ERROR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case ESP_LOADER_ERROR_INVALID_MD5:
        return ESP_ERR_INVALID_CRC;
    case ESP_LOADER_ERROR_IMAGE_SIZE:
        return ESP_ERR_INVALID_SIZE;
    case ESP_LOADER_ERROR_UNSUPPORTED_CHIP:
    case ESP_LOADER_ERROR_UNSUPPORTED_FUNC:
        return ESP_ERR_NOT_SUPPORTED;
    case ESP_LOADER_ERROR_INVALID_RESPONSE:
    case ESP_LOADER_ERROR_INVALID_PARAM:
    case ESP_LOADER_ERROR_INVALID_TARGET:
        return ESP_ERR_INVALID_ARG;
    default:
        return ESP_FAIL;
    }
}

static const char *loader_err_str(esp_loader_error_t err)
{
    switch (err) {
    case ESP_LOADER_SUCCESS:
        return "success";
    case ESP_LOADER_ERROR_FAIL:
        return "fail";
    case ESP_LOADER_ERROR_TIMEOUT:
        return "timeout";
    case ESP_LOADER_ERROR_INVALID_MD5:
        return "invalid md5";
    case ESP_LOADER_ERROR_IMAGE_SIZE:
        return "image too large";
    case ESP_LOADER_ERROR_UNSUPPORTED_CHIP:
        return "unsupported chip";
    case ESP_LOADER_ERROR_UNSUPPORTED_FUNC:
        return "not supported";
    case ESP_LOADER_ERROR_INVALID_RESPONSE:
        return "invalid response";
    case ESP_LOADER_ERROR_INVALID_PARAM:
        return "invalid param";
    case ESP_LOADER_ERROR_INVALID_TARGET:
        return "invalid target";
    default:
        return "unknown";
    }
}

struct chip_name_map_t {
    const char *name;
    target_chip_t chip;
};

/** Cap for the flash_size_kb YAML override; guards the * 1024 multiplication. */
static constexpr uint32_t MAX_FLASH_SIZE_KB = 256 * 1024; // 256 MB
static constexpr uint32_t FLASH_SECTOR_SIZE = 4096;

static const chip_name_map_t chip_name_map[] = {
    {"esp8266", ESP8266_CHIP}, {"esp32", ESP32_CHIP},     {"esp32s2", ESP32S2_CHIP},   {"esp32s3", ESP32S3_CHIP},
    {"esp32c3", ESP32C3_CHIP}, {"esp32c2", ESP32C2_CHIP}, {"esp32c5", ESP32C5_CHIP},   {"esp32h2", ESP32H2_CHIP},
    {"esp32c6", ESP32C6_CHIP}, {"esp32p4", ESP32P4_CHIP}, {"esp32c61", ESP32C61_CHIP}, {"esp32s31", ESP32S31_CHIP},
};

static bool chip_from_name(const char *name, target_chip_t &out)
{
    for (auto &m : chip_name_map) {
        if (strcmp(name, m.name) == 0) {
            out = m.chip;
            return true;
        }
    }
    return false;
}

// -------------------------------------------------------------------
// Session / connection
// -------------------------------------------------------------------

esp_err_t esp32_serial_backend::begin_session()
{
    // Pre-program procedures run before detect(), so prepare only the control
    // pins here. Opening the UART or entering download mode remains detect()'s
    // responsibility.
    return prepare_control_pins();
}

esp_err_t esp32_serial_backend::prepare_control_pins()
{
    const si::config::target_config &cfg = fw_asset_manager::instance()->config();
    const gpio_num_t reset_pin = (gpio_num_t)CONFIG_ESP_SWD_NRST_PIN;
    const int reset_deasserted = cfg.reset_assert_level == si::config::assert_level::high ? 0 : 1;

    esp_err_t ret = gpio_reset_pin(reset_pin);
    ret = ret ?: gpio_set_pull_mode(reset_pin, GPIO_PULLUP_ONLY);
    ret = ret ?: gpio_set_level(reset_pin, reset_deasserted);
    ret = ret ?: gpio_set_direction(reset_pin, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "session: cannot configure target reset GPIO %d: %s", (int)reset_pin, esp_err_to_name(ret));
        return ret;
    }

#if CONFIG_ESP_SWD_BOOT_PIN >= 0
    const gpio_num_t boot_pin = (gpio_num_t)CONFIG_ESP_SWD_BOOT_PIN;
    const int boot_deasserted = cfg.boot_assert_level == si::config::assert_level::high ? 0 : 1;
    ret = gpio_reset_pin(boot_pin);
    ret = ret ?: gpio_set_pull_mode(boot_pin, GPIO_PULLUP_ONLY);
    ret = ret ?: gpio_set_level(boot_pin, boot_deasserted);
    ret = ret ?: gpio_set_direction(boot_pin, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "session: cannot configure target boot GPIO %d: %s", (int)boot_pin, esp_err_to_name(ret));
        return ret;
    }
#endif

    return ESP_OK;
}

esp_err_t esp32_serial_backend::pulse_reset()
{
    const si::config::target_config &cfg = fw_asset_manager::instance()->config();
    const gpio_num_t reset_pin = (gpio_num_t)CONFIG_ESP_SWD_NRST_PIN;
    const int reset_asserted = cfg.reset_assert_level == si::config::assert_level::high ? 1 : 0;
    const int reset_deasserted = cfg.reset_assert_level == si::config::assert_level::high ? 0 : 1;

    esp_err_t ret = gpio_set_level(reset_pin, reset_asserted);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(SERIAL_FLASHER_RESET_HOLD_TIME_MS));
    return gpio_set_level(reset_pin, reset_deasserted);
}

esp_err_t esp32_serial_backend::validate_images(uint32_t limit)
{
    struct image_range {
        uint32_t sector_start;
        uint32_t sector_end;
    };

    const si::config::target_config &cfg = fw_asset_manager::instance()->config();
    image_range ranges[si::config::target_config::MAX_IMAGES] = {};

    for (size_t i = 0; i < cfg.image_count; i++) {
        const si::config::esp32_image &image = cfg.images[i];
        struct stat st = {};
        if (stat(image.path, &st) != 0) {
            ESP_LOGE(TAG, "detect: cannot stat image %s", image.path);
            return ESP_ERR_NOT_FOUND;
        }
        if (st.st_size <= 0 || static_cast<uint64_t>(st.st_size) > UINT32_MAX - 3u) {
            ESP_LOGE(TAG, "detect: image %s has invalid size %lld", image.path, static_cast<long long>(st.st_size));
            return ESP_ERR_INVALID_SIZE;
        }
        if ((image.offset & 3u) != 0) {
            ESP_LOGE(TAG, "detect: image %s offset 0x%08lx is not 4-byte aligned", image.path, image.offset);
            return ESP_ERR_INVALID_ARG;
        }

        const uint32_t real_len = static_cast<uint32_t>(st.st_size);
        const uint32_t padded_len = (real_len + 3u) & ~3u;
        if (image.offset > limit || padded_len > limit - image.offset) {
            ESP_LOGE(
                TAG, "detect: image %s (%lu bytes) at 0x%08lx does not fit in %lu bytes of flash", image.path, (unsigned long)padded_len,
                image.offset, (unsigned long)limit
            );
            return ESP_ERR_INVALID_SIZE;
        }

        const uint64_t byte_end = static_cast<uint64_t>(image.offset) + padded_len;
        const uint64_t sector_end = (byte_end + FLASH_SECTOR_SIZE - 1u) & ~(static_cast<uint64_t>(FLASH_SECTOR_SIZE) - 1u);
        ranges[i] = {
            .sector_start = image.offset & ~(FLASH_SECTOR_SIZE - 1u),
            .sector_end = static_cast<uint32_t>(sector_end),
        };

        for (size_t prev = 0; prev < i; prev++) {
            if (ranges[i].sector_start < ranges[prev].sector_end && ranges[prev].sector_start < ranges[i].sector_end) {
                ESP_LOGE(TAG, "detect: image %s overlaps an erase sector used by %s", image.path, cfg.images[prev].path);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    return ESP_OK;
}

esp_err_t esp32_serial_backend::connect_locked()
{
    if (connected) {
        return ESP_OK;
    }
    flash_limit = 0;

    const si::config::target_config &cfg = fw_asset_manager::instance()->config();

    port = {};
    port.port.ops = &esp32_uart_ops_custom;
    port.baud_rate = 115200; // ROM loader sync rate; raised after connect
    port.uart_port = CONFIG_SI_ESP32_TARGET_UART_NUM;
    port.uart_rx_pin = (gpio_num_t)CONFIG_SI_ESP32_TARGET_UART_RX_PIN;
    port.uart_tx_pin = (gpio_num_t)CONFIG_SI_ESP32_TARGET_UART_TX_PIN;
    // The ESP32 target shares the programming header reset/boot signals with
    // the SWD target connector.
    port.reset_pin = (gpio_num_t)CONFIG_ESP_SWD_NRST_PIN;
#if CONFIG_ESP_SWD_BOOT_PIN >= 0
    port.boot_pin = (gpio_num_t)CONFIG_ESP_SWD_BOOT_PIN;
#else
    port.boot_pin = GPIO_NUM_NC;
#endif

    esp_loader_error_t err = esp_loader_init_serial(&loader, &port.port);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "detect: esp_loader_init_serial failed: %s", loader_err_str(err));
        return ESP_FAIL;
    }

    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    err = esp_loader_connect_with_stub(&loader, &args);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "detect: connect failed: %s", loader_err_str(err));
        esp_loader_deinit(&loader);
        return loader_err_to_esp(err);
    }

    target_chip_t actual = esp_loader_get_target(&loader);
    target_chip_t wanted = ESP_UNKNOWN_CHIP;
    if (!chip_from_name(cfg.chip, wanted)) {
        ESP_LOGE(TAG, "detect: unknown chip name in target.yaml: '%s'", cfg.chip ? cfg.chip : "?");
        esp_loader_deinit(&loader);
        return ESP_ERR_INVALID_STATE;
    }
    if (actual != wanted) {
        ESP_LOGE(TAG, "detect: target chip mismatch: connected %d but target.yaml says %s (%d)", (int)actual, cfg.chip, (int)wanted);
        esp_loader_deinit(&loader);
        return ESP_ERR_INVALID_STATE;
    }

    // Match esptool's policy: constrain writes to the smaller of the configured
    // size and the physical size when detection succeeds. The configured size
    // is only authoritative when physical detection is unavailable.
    uint32_t detected_size = 0;
    uint32_t configured_size = 0;
    if (cfg.flash_size_kb.has_value()) {
        if (cfg.flash_size_kb.value() == 0 || cfg.flash_size_kb.value() > MAX_FLASH_SIZE_KB) {
            ESP_LOGE(
                TAG, "detect: flash_size_kb %lu KB is outside the 1-%lu KB range", (unsigned long)cfg.flash_size_kb.value(),
                (unsigned long)MAX_FLASH_SIZE_KB
            );
            esp_loader_deinit(&loader);
            return ESP_ERR_INVALID_SIZE;
        }
        configured_size = cfg.flash_size_kb.value() * 1024;

        err = esp_loader_flash_detect_size(&loader, &detected_size);
        if (err == ESP_LOADER_SUCCESS) {
            flash_limit = std::min(configured_size, detected_size);
            if (detected_size != configured_size) {
                ESP_LOGW(
                    TAG, "detect: configured flash size %lu differs from detected size %lu; limiting access to %lu bytes",
                    (unsigned long)configured_size, (unsigned long)detected_size, (unsigned long)flash_limit
                );
            }
        } else {
            flash_limit = configured_size;
            ESP_LOGW(
                TAG, "detect: flash size detection failed (%s); using configured size %lu bytes", loader_err_str(err), (unsigned long)flash_limit
            );
        }

        /*
         * WORKAROUND: esp-serial-flasher has no public API to override the
         * flash size used for bounds checks and SPI parameters.
         * esp_loader_flash_detect_size() only writes the caller's variable,
         * and a later esp_loader_flash_start() re-detects (or falls back to
         * DEFAULT_FLASH_SIZE) because loader->_target_flash_size is still 0
         * after connect_with_stub(). Assign the private field directly until
         * an upstream setter (esp_loader_set_flash_size) is agreed on:
         * https://github.com/espressif/esp-serial-flasher issue TBD.
         *
         * _Static_assert pins this hack to the field being exactly where we
         * think it is; if the struct layout changes, the build fails here
         * instead of silently corrupting an unrelated field.
         */
        static_assert(offsetof(esp_loader_t, _target_flash_size) > 0, "esp_loader_t layout changed? _target_flash_size missing");
        loader._target_flash_size = flash_limit;
    } else {
        err = esp_loader_flash_detect_size(&loader, &detected_size);
        if (err != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "detect: flash size detection failed: %s", loader_err_str(err));
            esp_loader_deinit(&loader);
            return loader_err_to_esp(err);
        }
        flash_limit = detected_size;
        loader._target_flash_size = flash_limit; // same workaround, detected value
    }
    ESP_LOGI(TAG, "detect: chip %s, accessible flash %lu KB", cfg.chip, (unsigned long)(flash_limit / 1024));

    auto validate_ret = validate_images(flash_limit);
    if (validate_ret != ESP_OK) {
        esp_loader_deinit(&loader);
        flash_limit = 0;
        return validate_ret;
    }

    // Raise the transfer rate once the stub is running.
    if (cfg.baud > 115200) {
        err = esp_loader_change_transmission_rate(&loader, cfg.baud);
        if (err != ESP_LOADER_SUCCESS) {
            /*
             * The library commands the TARGET to change rate first and only
             * then reconfigures the host UART; on failure the two sides may
             * be left at different baud rates, so warn-and-continue would
             * desynchronize every later command. Tear the connection down;
             * the FSM retry loop reconnects from a clean state.
             */
            ESP_LOGE(TAG, "detect: cannot change baud to %lu, failing connection", (unsigned long)cfg.baud);
            esp_loader_deinit(&loader);
            flash_limit = 0;
            return loader_err_to_esp(err);
        }
    }

    connected = true;
    return ESP_OK;
}

esp_err_t esp32_serial_backend::detect()
{
    return connect_locked();
}

esp_err_t esp32_serial_backend::erase()
{
    // esp_loader_flash_start erases exactly the regions each image lands in,
    // so a separate erase pass is unnecessary here.
    return ESP_OK;
}

// -------------------------------------------------------------------
// Program / verify
// -------------------------------------------------------------------

esp_err_t esp32_serial_backend::program_one_image(const char *path, uint32_t offset, uint32_t *written_len)
{
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "program: cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(file, 0, SEEK_END);
    long file_len = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (file_len < 0) {
        ESP_LOGE(TAG, "program: cannot get size of %s", path);
        fclose(file);
        return ESP_FAIL;
    }
    if (file_len == 0) {
        ESP_LOGE(TAG, "program: %s is empty", path);
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    if (static_cast<unsigned long>(file_len) > UINT32_MAX - 3) {
        ESP_LOGE(TAG, "program: %s (%ld bytes) is too large", path, file_len);
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * esp_loader_flash_start() requires a 4-byte aligned image size. Like
     * esptool (pad_to(data, 4) with 0xFF), only the 1-3 byte tail is padded;
     * the MD5 accumulated by the library covers file bytes plus that exact
     * padding, and flash_finish() checks it on the target.
     */
    const uint32_t real_len = static_cast<uint32_t>(file_len);
    const uint32_t padded_len = (real_len + 3) & ~3u;
    const uint32_t pad_len = padded_len - real_len;
    if ((offset & 3u) != 0 || offset > flash_limit || padded_len > flash_limit - offset) {
        ESP_LOGE(TAG, "program: %s no longer fits at 0x%08lx", path, offset);
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_loader_flash_cfg_t flash_cfg = {
        .offset = offset,
        .image_size = padded_len,
        .block_size = sizeof(block),
        .skip_verify = false, // MD5 verification in flash_finish
        ._state = {._sequence_number = 0, ._md5_context = {}},
    };

    esp_loader_error_t err = esp_loader_flash_start(&loader, &flash_cfg);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "program: flash_start failed for %s @ 0x%08lx: %s", path, offset, loader_err_str(err));
        fclose(file);
        return loader_err_to_esp(err);
    }

    /*
     * Read the file with exact-length discipline. esptool slurps the whole
     * file into memory before flashing, so a mid-stream truncation cannot
     * happen there; streaming here means we must detect it instead. Every
     * fread must return every requested byte while file bytes remain - a
     * short read is an I/O error or a file that changed under us, never a
     * legitimate final chunk. The only allowed padding is the precomputed
     * 0xFF tail above.
     */
    int64_t ts = esp_timer_get_time();
    uint32_t file_remaining = real_len;
    while (file_remaining > 0) {
        uint32_t want = std::min((size_t)file_remaining, sizeof(block));
        size_t got = fread(block, 1, want, file);
        if (got != want) {
            ESP_LOGE(
                TAG, "program: short read on %s at %lu/%lu bytes%s", path, (unsigned long)(real_len - file_remaining), (unsigned long)real_len,
                ferror(file) ? " (I/O error)" : " (file truncated?)"
            );
            fclose(file);
            return ESP_FAIL;
        }

        uint32_t send_len = want;
        if (file_remaining == want && pad_len > 0) {
            // Final file chunk: append the deterministic 0xFF tail padding.
            memset(block + want, 0xFF, pad_len);
            send_len += pad_len;
        }

        err = esp_loader_flash_write(&loader, &flash_cfg, block, send_len);
        if (err != ESP_LOADER_SUCCESS) {
            ESP_LOGE(
                TAG, "program: flash_write failed for %s at %lu/%lu: %s", path, (unsigned long)(real_len - file_remaining), (unsigned long)real_len,
                loader_err_str(err)
            );
            fclose(file);
            return loader_err_to_esp(err);
        }
        file_remaining -= want;
    }
    fclose(file);

    err = esp_loader_flash_finish(&loader, &flash_cfg);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "program: flash_finish (MD5 verify) failed for %s: %s", path, loader_err_str(err));
        return loader_err_to_esp(err);
    }

    ts = esp_timer_get_time() - ts;
    double speed = padded_len / (static_cast<double>(ts) / 1000000.0);
    ESP_LOGI(TAG, "program: %s -> 0x%08lx, %lu bytes, %.2f bytes/sec, MD5 OK", path, offset, (unsigned long)real_len, speed);

    if (written_len != nullptr) {
        *written_len = real_len;
    }
    return ESP_OK;
}

esp_err_t esp32_serial_backend::verify_one_image(const char *path, uint32_t offset)
{
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "verify: cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(file, 0, SEEK_END);
    long file_len = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (file_len < 0) {
        ESP_LOGE(TAG, "verify: cannot get size of %s", path);
        fclose(file);
        return ESP_FAIL;
    }
    if (file_len == 0) {
        ESP_LOGE(TAG, "verify: %s is empty", path);
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    if (static_cast<unsigned long>(file_len) > UINT32_MAX - 3) {
        ESP_LOGE(TAG, "verify: %s (%ld bytes) is too large", path, file_len);
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * Verify exactly file_len bytes plus the deterministic 0xFF tail padding
     * (esptool's verify_flash compares md5(pad_to(data, 4)) the same way).
     * The read loop never treats "fread returned 0" as success: every chunk
     * must deliver every requested byte, and ferror() is checked after the
     * loop so a truncated or erroring source fails the verify loudly.
     */
    const uint32_t real_len = static_cast<uint32_t>(file_len);
    const uint32_t padded_len = (real_len + 3) & ~3u;
    const uint32_t pad_len = padded_len - real_len;
    if ((offset & 3u) != 0 || offset > flash_limit || padded_len > flash_limit - offset) {
        ESP_LOGE(TAG, "verify: %s no longer fits at 0x%08lx", path, offset);
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t offset_cursor = offset;
    uint32_t file_remaining = real_len;
    while (file_remaining > 0) {
        uint32_t want = std::min((size_t)file_remaining, sizeof(block));
        size_t got = fread(block, 1, want, file);
        if (got != want) {
            ESP_LOGE(
                TAG, "verify: short read on %s at %lu/%lu bytes%s", path, (unsigned long)(real_len - file_remaining), (unsigned long)real_len,
                ferror(file) ? " (I/O error)" : " (file truncated?)"
            );
            fclose(file);
            return ESP_FAIL;
        }

        uint32_t cmp_len = want;
        if (file_remaining == want && pad_len > 0) {
            // Final chunk: also compare the 0xFF tail that was programmed.
            memset(block + want, 0xFF, pad_len);
            cmp_len += pad_len;
        }

        esp_loader_error_t err = esp_loader_flash_read(&loader, verify_block, offset_cursor, cmp_len);
        if (err != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "verify: flash_read failed for %s @ 0x%08lx: %s", path, offset_cursor, loader_err_str(err));
            fclose(file);
            return loader_err_to_esp(err);
        }
        if (memcmp(block, verify_block, cmp_len) != 0) {
            ESP_LOGE(TAG, "verify: mismatch for %s @ 0x%08lx", path, offset_cursor);
            fclose(file);
            return ESP_ERR_INVALID_CRC;
        }

        offset_cursor += cmp_len;
        file_remaining -= want;
    }

    bool io_error = ferror(file) != 0;
    fclose(file);
    if (io_error) {
        ESP_LOGE(TAG, "verify: I/O error on %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "verify: %s @ 0x%08lx, %lu bytes OK", path, offset, (unsigned long)real_len);
    return ESP_OK;
}
esp_err_t esp32_serial_backend::program(uint32_t *written_len)
{
    auto ret = connect_locked();
    if (ret != ESP_OK) {
        return ret;
    }

    const si::config::target_config &cfg = fw_asset_manager::instance()->config();
    uint32_t total = 0;
    for (size_t i = 0; i < cfg.image_count; i++) {
        uint32_t one = 0;
        ret = program_one_image(cfg.images[i].path, cfg.images[i].offset, &one);
        if (ret != ESP_OK) {
            return ret;
        }
        total += one;
    }

    if (written_len != nullptr) {
        *written_len = total;
    }
    return ESP_OK;
}

esp_err_t esp32_serial_backend::verify(uint32_t /*written_len*/)
{
    auto ret = connect_locked();
    if (ret != ESP_OK) {
        return ret;
    }

    const si::config::target_config &cfg = fw_asset_manager::instance()->config();
    for (size_t i = 0; i < cfg.image_count; i++) {
        ret = verify_one_image(cfg.images[i].path, cfg.images[i].offset);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t esp32_serial_backend::self_test(const si::config::test_item & /*item*/, uint32_t * /*func_return_val*/)
{
    return ESP_ERR_NOT_SUPPORTED;
}

// -------------------------------------------------------------------
// Transport control / unsupported ops
// -------------------------------------------------------------------

esp_err_t esp32_serial_backend::release_transport()
{
    if (connected) {
        // Reset the target into normal boot, then drop the UART transport.
        esp_loader_reset_target(&loader);
        esp_loader_deinit(&loader);
        connected = false;
    }
    flash_limit = 0;
    return ESP_OK;
}

esp_err_t esp32_serial_backend::reset_target()
{
    if (connected) {
        // Also clears the library's stub/SPI state before pulsing reset.
        esp_loader_reset_target(&loader);
        return ESP_OK;
    }

    // A pre-program reset happens before detect() has opened the serial
    // loader. Drive the control pin directly in that case.
    esp_err_t ret = prepare_control_pins();
    return ret == ESP_OK ? pulse_reset() : ret;
}

esp_err_t esp32_serial_backend::reinit_debug()
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp32_serial_backend::halt_target()
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp32_serial_backend::wait_halt()
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp32_serial_backend::read_mem32(uint32_t addr, uint32_t *val)
{
    if (val == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!connected) {
        ESP_LOGE(TAG, "read_mem32: target not connected; register access is only available after detect()");
        return ESP_ERR_INVALID_STATE;
    }

    esp_loader_error_t err = esp_loader_read_register(&loader, addr, val);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "read_mem32: register read @ 0x%08lx failed: %s", addr, loader_err_str(err));
    }
    return loader_err_to_esp(err);
}

esp_err_t esp32_serial_backend::write_mem32(uint32_t addr, uint32_t val)
{
    if (!connected) {
        ESP_LOGE(TAG, "write_mem32: target not connected; register access is only available after detect()");
        return ESP_ERR_INVALID_STATE;
    }

    esp_loader_error_t err = esp_loader_write_register(&loader, addr, val);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "write_mem32: register write @ 0x%08lx failed: %s", addr, loader_err_str(err));
    }
    return loader_err_to_esp(err);
}
