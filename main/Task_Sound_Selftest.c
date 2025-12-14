#include "Task_Sound_Selftest.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"

#include "App_Speak_Sound.h"

static const char *TAG = "Task_Sound_Selftest";

static void task_entry(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "speaker selftest: play 1kHz tone (800ms) ...");
    esp_err_t err = app_speak_sound_play_tone(1000, 800);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "play tone failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "speaker selftest done");
    vTaskDelete(NULL);
}

esp_err_t task_sound_selftest_start(void)
{
    BaseType_t ok = xTaskCreate(task_entry, "task_sound_selftest", 4096, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

