#pragma once

#include <cstdint>

#include <esp_err.h>
#include <esp_loader.h>
#include <esp32_port.h>

#include "target_backend.hpp"

/**
 * Espressif implementation of target_backend using esp-serial-flasher over
 * UART SLIP (ROM download mode + flasher stub).
 *
 * Programming flow per connect:
 *  detect()  - enter bootloader (BOOT + NRST via the host SWD header pins),
 *              esp_loader_connect_with_stub, match the chip against the
 *              target.yaml `chip` name, constrain access to the smaller of
 *              configured and detected flash sizes, validate image ranges,
 *              then optionally raise the baud rate.
 *  erase()   - no-op: esp_loader_flash_start erases the regions it writes.
 *  program() - stream every configured image (bootloader, partition table,
 *              app, ...) with esp_loader_flash_start/write/finish; finish()
 *              MD5-verifies each image on the target side.
 *  verify()  - read the images back with esp_loader_flash_read and compare.
 *
 * UART pins/peri come from Kconfig (SI_ESP32_TARGET_UART_*); reset/boot pins
 * reuse the SWD header signals (ESP_SWD_NRST_PIN / ESP_SWD_BOOT_PIN).
 */
class esp32_serial_backend : public target_backend
{
public:
    static esp32_serial_backend *instance()
    {
        static esp32_serial_backend _instance;
        return &_instance;
    }

    esp32_serial_backend(esp32_serial_backend const &) = delete;
    void operator=(esp32_serial_backend const &) = delete;

    const char *name() override
    {
        return "esp32 (serial)";
    }

    esp_err_t begin_session() override;
    esp_err_t detect() override;
    esp_err_t erase() override;
    esp_err_t program(uint32_t *written_len) override;
    esp_err_t verify(uint32_t written_len) override;
    esp_err_t self_test(const si::config::test_item &item, uint32_t *func_return_val) override;
    esp_err_t release_transport() override;
    esp_err_t reset_target() override;
    esp_err_t reinit_debug() override;
    esp_err_t halt_target() override;
    esp_err_t wait_halt() override;
    esp_err_t read_mem32(uint32_t addr, uint32_t *val) override;
    esp_err_t write_mem32(uint32_t addr, uint32_t val) override;

private:
    esp32_serial_backend() = default;

    esp_err_t program_one_image(const char *path, uint32_t offset, uint32_t *written_len);
    esp_err_t verify_one_image(const char *path, uint32_t offset);
    esp_err_t connect_locked();
    esp_err_t validate_images(uint32_t flash_limit);
    esp_err_t prepare_control_pins();
    esp_err_t pulse_reset();

    esp32_port_t port = {};
    esp_loader_t loader = {};
    bool connected = false;
    uint32_t flash_limit = 0;

    /** Static staging blocks (no heap in the hot path; singleton lives in .bss). */
    uint8_t block[4096] = {};
    uint8_t verify_block[4096] = {};

    static const constexpr char *TAG = "esp32_backend";
};
