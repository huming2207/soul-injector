#include <esp_log.h>
#include <esp_timer.h>
#include "display_manager.hpp"

esp_err_t display_manager::init()
{
    ESP_LOGI(TAG, "Panel init");
    auto ret = panel->init();
    if (ret != ESP_OK) {
        return ret;
    }

    disp_buf_a = static_cast<uint8_t *>(heap_caps_malloc(CONFIG_SI_DISP_PANEL_BUFFER_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA));
    if (disp_buf_a == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
        return ESP_ERR_NO_MEM;
    }

    disp_buf_b = static_cast<uint8_t *>(heap_caps_malloc(CONFIG_SI_DISP_PANEL_BUFFER_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA));
    if (disp_buf_b == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
        free(disp_buf_a);
        return ESP_ERR_NO_MEM;
    }

    lv_ui_task_stack_buf = static_cast<StackType_t *>(heap_caps_calloc(1, UI_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_32BIT));
    if (lv_ui_task_stack_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to set up stack for UI task");
        free(disp_buf_a);
        free(disp_buf_b);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LVGL init");
    lv_init();

    ESP_LOGI(TAG, "Lock & task init");
    lv_ui_task_lock = xSemaphoreCreateBinary();
    if (lv_ui_task_lock == nullptr) {
        ESP_LOGE(TAG, "Failed to create UI task lock");
        free(disp_buf_a);
        free(disp_buf_b);
        free(lv_ui_task_stack_buf);
        return ESP_ERR_NO_MEM;
    } else {
        xSemaphoreGive(lv_ui_task_lock); // Set up initial semaphore state
    }

    lv_ui_task_handle = xTaskCreateStatic(lv_ui_task, "ui_task", UI_STACK_SIZE, this, tskIDLE_PRIORITY + 1, lv_ui_task_stack_buf, &lv_ui_task_stack);
    if (lv_ui_task_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to create UI task");
        free(disp_buf_a);
        free(disp_buf_b);
        vTaskDelete(lv_ui_task_handle);
        free(lv_ui_task_stack_buf);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UI task started");

    lv_disp_draw_buf_init(&draw_buf, disp_buf_a, disp_buf_b, CONFIG_SI_DISP_PANEL_BUFFER_SIZE);
    ret = ret ?: panel->setup_lvgl(&draw_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register panel");
        free(disp_buf_a);
        free(disp_buf_b);
        vTaskDelete(lv_ui_task_handle);
        free(lv_ui_task_stack_buf);
        return ret;
    }

    esp_timer_create_args_t timer_args = {
            .callback = lv_tick_cb,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "lvgl_timer",
            .skip_unhandled_events = true,
    };

    ret = ret ?: esp_timer_create(&timer_args, &timer_handle);
    ret = ret ?: esp_timer_start_periodic(timer_handle, LV_TICK_PERIOD_MS * 1000);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up timer");
        free(disp_buf_a);
        free(disp_buf_b);
        vTaskDelete(lv_ui_task_handle);
        free(lv_ui_task_stack_buf);
        return ESP_ERR_NO_MEM;
    } else {
        ESP_LOGI(TAG, "Display manager init OK");
    }

    return ret;
}

disp_panel_if *display_manager::get_panel()
{
    return panel;
}

void IRAM_ATTR display_manager::lv_tick_cb(void *arg)
{
    (void) arg;
    lv_tick_inc(1);
}

void display_manager::lv_ui_task(void *_ctx)
{
    ESP_LOGI(TAG, "UI task started");
    auto *ctx = static_cast<display_manager *>(_ctx);

    while (true) {
        xSemaphoreTake(ctx->lv_ui_task_lock, portMAX_DELAY);
        lv_task_handler();
        xSemaphoreGive(ctx->lv_ui_task_lock);
        vTaskDelay(1);
    }
}
