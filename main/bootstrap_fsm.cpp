#include <esp_event.h>
#include <nvs_flash.h>
#include <sys/stat.h>
#include "bootstrap_fsm.hpp"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include <esp_heap_caps.h>
#include "esp_sleep.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"
#include "agent_service.hpp"
#include "asset_store.hpp"
#include "fw_asset_manager.hpp"
#include "http_downloader.hpp"
#include "job_controller.hpp"
#include "offline_flasher.hpp"
#include "prog_log.hpp"
#include "qjs_binder.hpp"
#include "sidp_service.hpp"
#include "sidp_transport_cdc.hpp"
#include "swd_prog.hpp"
#include "target_lines.hpp"
#include <swd_host.h>
#include "driver/i2c_master.h"

esp_err_t bootstrap_fsm::init()
{
    ESP_LOGI(TAG, "Setting up display");
    display = display_manager::instance();
    auto &led = led_ctrl::instance();
    esp_err_t ret = display->init();
    composer = display->get_composer();
    ret = ret ?: composer->init();
    ret = ret ?: led.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up display: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    evt_group = xEventGroupCreate();
    if (evt_group == nullptr) {
        ESP_LOGE(TAG, "Can't create event group");
        return ESP_FAIL;
    }

    io_debounce_timer = xTimerCreate("io_debounce", pdMS_TO_TICKS(50), pdFALSE, this, io_debounce_handler);
    const esp_timer_create_args_t retry_args = {
        .callback = io_debounce_retry,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "io_retry",
        .skip_unhandled_events = true,
    };
    if (io_debounce_timer == nullptr || esp_timer_create(&retry_args, &io_debounce_retry_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Can't create debounce timer");
        return ESP_FAIL;
    }

    ret = setup_pm_locks();
    ret = ret ?: target_lines::init();
    ret = ret ?: setup_io_pins();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up power management: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Setting up storage");
    ret = setup_storage();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up storage: 0x%x %s", ret, esp_err_to_name(ret));
        composer->display_notice("Storage error", "Try a factory reset.");
        return ret;
    }

    ret = prog_log::instance()->init();
    if (ret != ESP_OK) {
        composer->display_notice("Log storage error", "Install the Soul Injector firmware again.");
        return ret;
    }

    // No stored job is fine: the host pushes one over SIDP.
    asset_store::remove_stale_uploads();
    fw_asset_manager::instance()->init();
    esp_err_t js_ret = qjs_binder::js_host::instance()->init();
    if (js_ret != ESP_OK && js_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "Job scripts unavailable: 0x%x %s", js_ret, esp_err_to_name(js_ret));
    }
    ret = job_controller::instance()->init(evt_group, BIT_RUN_REQUEST, BIT_TRIGGER_CHANGED);
    if (ret == ESP_OK) {
        arm_target_detect(); // only now: an edge needs the job controller and the saved trigger
    }
    ret = ret ?: setup_usb();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up USB/SIDP: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCore(fsm_task_handler, "flasher", 8192, this, tskIDLE_PRIORITY + 10, &fsm_task, swd_def::TASK_CORE);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Can't create FSM task: %u bytes of internal RAM free, largest block %u",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_FAIL;
    }

    // Runs start on edges, so the level at boot starts nothing.
    ESP_LOGI(TAG, "Detect input %s at boot, trigger %s", last_det_state ? "high" : "low",
             job_controller::trigger_name(job_controller::instance()->get_trigger()));

    ESP_LOGI(TAG, "Bootstrap init OK");
    return ESP_OK;
}

esp_err_t bootstrap_fsm::setup_pm_locks()
{
    esp_err_t ret = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "run", &run_pm_lock);
    ret = ret ?: esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &usb_pm_lock);

    // Without a plug detect pin, USB may be in use at any time.
    if (ret == ESP_OK && PLUG_DET_PIN == GPIO_NUM_NC) {
        ret = esp_pm_lock_acquire(usb_pm_lock);
    }
    return ret;
}

esp_err_t bootstrap_fsm::setup_io_pins()
{
    ESP_LOGI(TAG, "Setting up detection pins");
    // DET_IO_PIN is active-low with no external pull-up on Rev 7.1 / Rev 8.
    gpio_config_t det_cfg = {};
    det_cfg.pin_bit_mask = BIT64(DET_IO_PIN);
    det_cfg.mode = GPIO_MODE_INPUT;
    det_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    det_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    det_cfg.intr_type = GPIO_INTR_DISABLE;

    esp_err_t ret = gpio_config(&det_cfg);
    if (PLUG_DET_PIN != GPIO_NUM_NC) {
        // PLUG_DET_PIN has its own external pull-up resistor network.
        gpio_config_t plug_cfg = {};
        plug_cfg.pin_bit_mask = BIT64(PLUG_DET_PIN);
        plug_cfg.mode = GPIO_MODE_INPUT;
        plug_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        plug_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        plug_cfg.intr_type = GPIO_INTR_DISABLE;
        ret = ret ?: gpio_config(&plug_cfg);
    }
    gpio_install_isr_service(0);
    ret = ret ?: gpio_isr_handler_add(DET_IO_PIN, io_isr_handler, this);
    if (PLUG_DET_PIN != GPIO_NUM_NC) {
        ret = ret ?: gpio_isr_handler_add(PLUG_DET_PIN, io_isr_handler, this);
    }
    ret = ret ?: esp_sleep_enable_gpio_wakeup();
    if (ret != ESP_OK) {
        return ret;
    }

    // The detect input is armed by arm_target_detect(), once job_controller is up
    last_det_state = gpio_get_level(DET_IO_PIN);
    if (PLUG_DET_PIN != GPIO_NUM_NC) {
        update_usb_power();
    }
    return ESP_OK;
}

void bootstrap_fsm::arm_target_detect()
{
    gpio_intr_disable(DET_IO_PIN); // the debounce handler may run meanwhile (USB plug)
    last_det_state = gpio_get_level(DET_IO_PIN);
    det_ready = true;
    arm_io_pin(DET_IO_PIN, last_det_state);
}

esp_err_t bootstrap_fsm::setup_storage()
{
    uint8_t sn_buf[16] = {0};
    uint64_t flash_uid = 0;
    esp_efuse_mac_get_default(sn_buf);
    esp_flash_read_unique_chip_id(esp_flash_default_chip, &flash_uid);
    memcpy(sn_buf + 6, &flash_uid, sizeof(uint64_t));
    snprintf(
        sn_str, sizeof(sn_str), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", sn_buf[0], sn_buf[1], sn_buf[2], sn_buf[3], sn_buf[4],
        sn_buf[5], sn_buf[6], sn_buf[7], sn_buf[8], sn_buf[9], sn_buf[10], sn_buf[11], sn_buf[12], sn_buf[13]
    );

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        ret = ret ?: nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup_storage: Failed to set up NVS: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 10,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    ret = esp_vfs_fat_spiflash_mount_rw_wl(DATA_PARTITION_PATH, DATA_PARTITION_LABEL, &mount_cfg, &wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup_storage: failed to mount %s: 0x%x %s", DATA_PARTITION_PATH, ret, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "setup_storage: init OK");
    return ret;
}

esp_err_t bootstrap_fsm::setup_usb()
{
    static char lang[2] = {0x09, 0x04};
    static const char *desc_str[5] = {
        lang,                                                        // 0: is supported language is English (0x0409)
        const_cast<char *>(CONFIG_TINYUSB_DESC_MANUFACTURER_STRING), // 1: Manufacturer
        const_cast<char *>(CONFIG_TINYUSB_DESC_PRODUCT_STRING),      // 2: Product
        sn_str,                                                      // 3: Serials, should use chip ID
        const_cast<char *>(CONFIG_TINYUSB_DESC_PRODUCT_STRING),      // 4: CDC Interface (SIDP)
    };

    ESP_LOGI(TAG, "USB CDC initialization");
    // SIDP needs device attach/detach events to notice a replugged host.
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(sidp::cdc_slip_transport::device_event_callback);
    tusb_cfg.task.size = 8192;
    tusb_cfg.descriptor.string = static_cast<const char **>(desc_str);
    tusb_cfg.descriptor.string_count = std::size(desc_str);

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "setup_usb: failed at tinyusb_driver_install: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

    return sidp_service::instance()->init(sn_str);
}

void bootstrap_fsm::fsm_task_handler(void *_ctx)
{
    if (_ctx == nullptr) {
        return;
    }

    auto *ctx = static_cast<bootstrap_fsm *>(_ctx);

    ctx->composer->display_init();

    while (true) {
        ctx->run_fsm_task();
    }
}

void bootstrap_fsm::run_fsm_task()
{
    // Run requests from SIDP and the trigger's edges on the detect input wake
    // the task; with TRIGGER_DETECT_SWD it also wakes to probe for a target.
    auto *jobs = job_controller::instance();
    const bool polling = jobs->get_trigger() == si_manage_Trigger_TRIGGER_DETECT_SWD;
    EventBits_t bits = xEventGroupWaitBits(evt_group, BIT_DETECTED | BIT_RUN_REQUEST | BIT_TRIGGER_CHANGED, pdTRUE, pdFALSE,
                                           polling ? pdMS_TO_TICKS(SWD_POLL_MS) : portMAX_DELAY);
    if (bits & BIT_TRIGGER_CHANGED) {
        swd_present = false; // a new setting starts from "no target": one there now runs
        swd_absent_since_us = 0;
    }
    if ((bits & (BIT_DETECTED | BIT_RUN_REQUEST)) == 0) {
        if (!polling || !swd_target_arrived()) {
            return;
        }
    }

    // job_controller has already claimed a requested run; a detected
    // target only runs when the trigger asks for automatic runs.
    if ((bits & BIT_RUN_REQUEST) == 0 && !jobs->begin_auto_run() &&
        !(jobs->auto_run_waits_for_debug() && agent_service::instance()->yield_for_run(TRAP_YIELD_WAIT_MS) && jobs->begin_auto_run())) {
        show_auto_run_refused();
        return;
    }

    run_job();
    // An edge during the run is dropped, not queued; and the target was there
    // for the run, so SWD polling waits for it to go away before the next one.
    xEventGroupClearBits(evt_group, BIT_DETECTED);
    swd_present = true;
    swd_absent_since_us = 0;
}

bool bootstrap_fsm::swd_target_arrived()
{
    if (!job_controller::instance()->swd_poll_wanted() || swd_reset_held()) {
        return false; // no job, a run, or GDB, a session or a trap has the target (or will): leave the lines alone
    }
    // Claimed, so a session can't start between connect() and isolate() and
    // have its lines cut under it
    auto *jobs = job_controller::instance();
    if (!jobs->begin_probe()) {
        return false;
    }
    uint32_t idcode = 0;
    esp_err_t ret = target_lines::probe(&idcode);
    jobs->end_probe();
    if (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_INVALID_STATE) {
        return false; // another task is on the bus: no news
    }
    if (ret == ESP_OK) {
        swd_absent_since_us = 0;
        if (!swd_present) {
            swd_present = true;
            ESP_LOGI(TAG, "SWD: a target answers (DP IDCODE 0x%08lx)", idcode);
            return true;
        }
        return false;
    }
    if (swd_present) {
        int64_t now = esp_timer_get_time();
        if (swd_absent_since_us == 0) {
            swd_absent_since_us = now;
        } else if (now - swd_absent_since_us >= SWD_GONE_MS * 1000LL) {
            swd_present = false;
            swd_absent_since_us = 0;
            ESP_LOGI(TAG, "SWD: the target went away");
        }
    }
    return false;
}

void bootstrap_fsm::show_auto_run_refused()
{
    esp_err_t log_ret = prog_log::instance()->check_space();
    if (log_ret == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "Target connected, log full");
        composer->display_notice("Log is full", "Connect to the computer to collect the log.");
    } else if (log_ret != ESP_OK) {
        ESP_LOGE(TAG, "Target connected, log write failed");
        composer->display_notice("Log error", "Restart the Soul Injector.");
    } else {
        ESP_LOGI(TAG, "Target connected, no automatic job armed");
    }
}

void bootstrap_fsm::run_job()
{
    auto *flasher = offline_flasher::instance();
    int64_t start_us = esp_timer_get_time();
    esp_pm_lock_acquire(run_pm_lock);
    target_lines::end_boot_hold(); // the run takes the target, as it is
    target_lines::connect();

    flasher->init();
    esp_err_t ret = ESP_ERR_NOT_FINISHED;
    while (ret == ESP_ERR_NOT_FINISHED) {
        ret = flasher->handle_states();
        vTaskDelay(1);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Done flashing!");
    } else {
        ESP_LOGE(TAG, "Something went wrong");
    }

    qjs_binder::js_host::instance()->end_run(); // hash stays for the record
    target_lines::isolate();
    esp_pm_lock_release(run_pm_lock);
    uint32_t duration_ms = (esp_timer_get_time() - start_us) / 1000;
    job_controller::instance()->finish_run(ret, flasher->get_failed_state(), duration_ms);
}

void bootstrap_fsm::io_isr_handler(void *_ctx)
{
    auto *ctx = static_cast<bootstrap_fsm *>(_ctx);

    // Level interrupts keep firing: mask them until the debounce timer re-arms.
    gpio_intr_disable(DET_IO_PIN);
    if (PLUG_DET_PIN != GPIO_NUM_NC) {
        gpio_intr_disable(PLUG_DET_PIN);
    }

    BaseType_t higher_priority_waken = pdFALSE;
    if (xTimerStartFromISR(ctx->io_debounce_timer, &higher_priority_waken) != pdPASS) {
        // Timer queue full: without the debounce the pins would stay masked for good
        esp_timer_start_once(ctx->io_debounce_retry_timer, RETRY_US);
    }
    if (higher_priority_waken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void bootstrap_fsm::io_debounce_retry(void *_ctx)
{
    auto *ctx = static_cast<bootstrap_fsm *>(_ctx);
    if (xTimerStart(ctx->io_debounce_timer, 0) != pdPASS) {
        esp_timer_start_once(ctx->io_debounce_retry_timer, RETRY_US);
    }
}

void bootstrap_fsm::io_debounce_handler(TimerHandle_t timer_handle)
{
    auto *ctx = static_cast<bootstrap_fsm *>(pvTimerGetTimerID(timer_handle));
    ctx->update_target_detect();
    if (PLUG_DET_PIN != GPIO_NUM_NC) {
        ctx->update_usb_power();
    }
}

void bootstrap_fsm::arm_io_pin(gpio_num_t pin, int level)
{
    // Light sleep only wakes on GPIO levels, so wait for the opposite level:
    // it interrupts when awake and wakes the chip when asleep.
    gpio_wakeup_enable(pin, level ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    gpio_intr_enable(pin);
}

void bootstrap_fsm::update_target_detect()
{
    if (!det_ready) {
        return; // a USB plug edge before the detect input is armed
    }
    bool state = gpio_get_level(DET_IO_PIN);
    if (state != last_det_state) {
        last_det_state = state;
        si_manage_Trigger trigger = job_controller::instance()->get_trigger();
        bool run = (!state && trigger == si_manage_Trigger_TRIGGER_DETECT_FALLING) ||
                   (state && trigger == si_manage_Trigger_TRIGGER_DETECT_RISING);
        // run_fsm_task drops an edge that came during a run; say so here, where it's seen.
        const bool busy = run && job_controller::instance()->is_running();
        ESP_LOGI(TAG, "Detect input %s%s", state ? "rose" : "fell", busy ? ": ignored, a run is in progress" : run ? ": run" : "");
        if (run) {
            xEventGroupSetBits(evt_group, BIT_DETECTED);
        }
    }
    arm_io_pin(DET_IO_PIN, state);
}

void bootstrap_fsm::update_usb_power()
{
    bool powered = gpio_get_level(PLUG_DET_PIN);
    if (powered != usb_powered) {
        usb_powered = powered;
        if (powered) {
            ESP_LOGI(TAG, "USB power connected, light sleep off");
            esp_pm_lock_acquire(usb_pm_lock);
        } else {
            ESP_LOGI(TAG, "On battery, light sleep allowed");
            esp_pm_lock_release(usb_pm_lock);
        }
    }
    arm_io_pin(PLUG_DET_PIN, powered);
}
