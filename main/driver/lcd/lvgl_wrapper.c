#include <sys/cdefs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "lvgl_wrapper.h"
#include "lhs154kc.h"

#ifdef TAG
#undef TAG
#endif

#define TAG "si_disp"

#ifdef LV_TICK_PERIOD_MS
#undef LV_TICK_PERIOD_MS
#endif

#define LV_TICK_PERIOD_MS 1

static uint8_t *buf_a = NULL;
static uint8_t *buf_b = NULL;
static lv_disp_draw_buf_t draw_buf = {};
static esp_timer_handle_t timer_handle = NULL;
static SemaphoreHandle_t lv_ui_task_lock = NULL;
static StaticTask_t lv_ui_task_stack;
static StackType_t *lv_ui_task_stack_buf = NULL;
static TaskHandle_t lv_ui_task_handle = NULL;

_Noreturn static void lv_ui_task_handler(void *ctx)
{
    ESP_LOGI(TAG, "UI task started");
    while (true) {
       xSemaphoreTake(lv_ui_task_lock, portMAX_DELAY);
       lv_task_handler();
       xSemaphoreGive(lv_ui_task_lock);
       vTaskDelay(1);
    }
}

static void IRAM_ATTR lv_tick_cb(void *arg)
{
    (void) arg;
    lv_tick_inc(LV_TICK_PERIOD_MS);
}

esp_err_t lvgl_disp_init()
{
    ESP_LOGI(TAG, "LVGL init");
    lv_init();

    ESP_LOGI(TAG, "Display hardware init");
    lv_st7789_init();

    buf_a = heap_caps_malloc(SI_DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (buf_a == NULL) {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
        return ESP_ERR_NO_MEM;
    }

    buf_b = heap_caps_malloc(SI_DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (buf_b == NULL) {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
        free(buf_a);
        return ESP_ERR_NO_MEM;
    }

    lv_disp_draw_buf_init(&draw_buf, buf_a, buf_b, SI_DISP_BUF_SIZE);
    static lv_disp_drv_t disp_drv = {};
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = lv_st7789_flush;
    disp_drv.hor_res = SI_DISP_HOR_SIZE;
    disp_drv.ver_res = SI_DISP_VER_SIZE;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.antialiasing = 1;

    lv_disp_drv_register(&disp_drv);

    esp_timer_create_args_t timer_args = {
            .name = "lvgl_timer",
            .callback = lv_tick_cb,
    };

    esp_err_t ret = esp_timer_create(&timer_args, &timer_handle);
    ret = ret ?: esp_timer_start_periodic(timer_handle, LV_TICK_PERIOD_MS * 1000);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set up timer");
        free(buf_a);
        free(buf_b);
        return ESP_ERR_NO_MEM;
    } else {
        ESP_LOGI(TAG, "Display init done");
    }

    lv_ui_task_stack_buf = heap_caps_calloc(1, 131072, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_32BIT);
    if (lv_ui_task_stack_buf == NULL) {
        ESP_LOGE(TAG, "Failed to set up stack for UI task");
        free(buf_a);
        free(buf_b);
        return ESP_ERR_NO_MEM;
    }

    lv_ui_task_lock = xSemaphoreCreateBinary();
    if (lv_ui_task_lock == NULL) {
        ESP_LOGE(TAG, "Failed to create UI task lock");
        free(buf_a);
        free(buf_b);
        free(lv_ui_task_stack_buf);
        return ESP_ERR_NO_MEM;
    } else {
        xSemaphoreGive(lv_ui_task_lock); // Set up initial semaphore state
    }

    lv_ui_task_handle = xTaskCreateStatic(lv_ui_task_handler, "ui_task", 131072, NULL, tskIDLE_PRIORITY + 1, lv_ui_task_stack_buf, &lv_ui_task_stack);
    if (lv_ui_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create UI task");
        free(buf_a);
        free(buf_b);
        free(lv_ui_task_stack_buf);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UI task setup done");
    return ret;
}

esp_err_t lvgl_take_lock(TickType_t ticks)
{
    if (unlikely(lv_ui_task_lock == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(lv_ui_task_lock, ticks) == pdTRUE) {
        return ESP_OK;
    } else {
        return ESP_ERR_TIMEOUT;
    }
}

void lvgl_give_lock()
{
    if (unlikely(lv_ui_task_lock == NULL)) {
        return;
    } else {
        xSemaphoreGive(lv_ui_task_lock);
    }
}
