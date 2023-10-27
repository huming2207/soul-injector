#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include "display_manager.hpp"
#include "ui_sm_114.hpp"

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
    lv_ui_task_lock = xSemaphoreCreateRecursiveMutex();
    if (lv_ui_task_lock == nullptr) {
        ESP_LOGE(TAG, "Failed to create UI task lock");
        free(disp_buf_a);
        free(disp_buf_b);
        free(lv_ui_task_stack_buf);
        return ESP_ERR_NO_MEM;
    }

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

    ui_queue = xQueueCreate(3, sizeof(ui_state::queue_item));
    if (ui_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to set up UI queue");
        esp_timer_delete(timer_handle);
        free(disp_buf_a);
        free(disp_buf_b);
        vTaskDelete(lv_ui_task_handle);
        free(lv_ui_task_stack_buf);
        return ESP_ERR_NO_MEM;
    }


    lv_ui_task_handle = xTaskCreateStatic(lv_ui_task, "ui_task", UI_STACK_SIZE, this, tskIDLE_PRIORITY + 1, lv_ui_task_stack_buf, &lv_ui_task_stack);
    if (lv_ui_task_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to create UI task");
        esp_timer_delete(timer_handle);
        vQueueDelete(ui_queue);
        free(disp_buf_a);
        free(disp_buf_b);
        vTaskDelete(lv_ui_task_handle);
        free(lv_ui_task_stack_buf);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UI task init OK");

    ESP_LOGI(TAG, "Draw some stuff");
    acquire_lock();

    composer.init();


//    auto *label = lv_label_create(lv_disp_get_scr_act(panel->get_lv_disp()));
//    lv_obj_set_pos(label, 0, 0);
//    lv_label_set_text(label, "Hello world\nBy Jackson M Hu\nST7789V, 1.14 inch");
//    lv_obj_set_size(label, 135, 240);
    give_lock();
    ESP_LOGI(TAG, "Draw OK");

    return ret;
}

disp_panel_if *display_manager::get_panel()
{
    return panel;
}

void IRAM_ATTR display_manager::lv_tick_cb(void *arg)
{
    (void) arg;
    lv_tick_inc(LV_TICK_PERIOD_MS);
}

void display_manager::lv_ui_task(void *_ctx)
{
    ESP_LOGI(TAG, "UI task started");
    auto *ctx = static_cast<display_manager *>(_ctx);

    while (true) {
        xSemaphoreTakeRecursive(ctx->lv_ui_task_lock, portMAX_DELAY);
        uint32_t next_delay = lv_task_handler();
        xSemaphoreGiveRecursive(ctx->lv_ui_task_lock);

        uint32_t next_tick = 0;
        if (likely(next_delay >= LV_TASK_MAX_IDLE_MS)) {
            next_tick = 1;
        } else if (next_delay <= (1000 / configTICK_RATE_HZ)) {
            next_tick = pdMS_TO_TICKS(LV_TASK_MAX_IDLE_MS);
        } else {
            next_tick = pdMS_TO_TICKS(next_delay);
        }

        vTaskDelay(next_tick);
    }
}

esp_err_t display_manager::acquire_lock(uint32_t timeout_ms)
{
    TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lv_ui_task_lock, timeout_ticks) == pdTRUE ? ESP_OK : ESP_FAIL;
}

void display_manager::give_lock()
{
    xSemaphoreGiveRecursive(lv_ui_task_lock);
}

QueueHandle_t display_manager::get_ui_queue()
{
    return ui_queue;
}
