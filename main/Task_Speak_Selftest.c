#include "Task_Speak_Selftest.h"

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "App_Speak_Sound.h"

static const char *TAG = "Task_Speak_Selftest";

static void task_entry(void *arg)
{
    (void)arg;

    app_speak_sound_cfg_t cfg = {0};
    app_speak_sound_get_cfg(&cfg);
    if (cfg.sample_rate <= 0) cfg.sample_rate = 16000;
    if (cfg.channels <= 0) cfg.channels = 1;
    if (cfg.bits_per_sample <= 0) cfg.bits_per_sample = 16;

    const int record_ms = 5000;
    const size_t bytes_per_frame = (size_t)cfg.channels * (size_t)(cfg.bits_per_sample / 8);
    const size_t frames = (size_t)cfg.sample_rate * (size_t)record_ms / 1000;
    const size_t buf_bytes = frames * bytes_per_frame;

    ESP_LOGI(TAG, "record %d ms: %d Hz ch=%d bits=%d => buf=%u bytes",
             record_ms, cfg.sample_rate, cfg.channels, cfg.bits_per_sample, (unsigned)buf_bytes);

    void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_DEFAULT);
    }
    if (!buf) {
        ESP_LOGE(TAG, "alloc record buffer failed: %u bytes", (unsigned)buf_bytes);
        vTaskDelete(NULL);
        return;
    }
    memset(buf, 0, buf_bytes);

    // 录音
    ESP_LOGI(TAG, "start recording...");
    size_t got = 0;
    esp_err_t err = app_speak_sound_record(buf, buf_bytes, &got, record_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "record failed: %s", esp_err_to_name(err));
        heap_caps_free(buf);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "record done, bytes=%u", (unsigned)got);

    vTaskDelay(pdMS_TO_TICKS(200));

    // 简单统计：RMS + 前 8 个采样，判断是否全 0 / 太小
    if (got >= sizeof(int16_t)) {
        int16_t *p16 = (int16_t *)buf;
        size_t samples = got / sizeof(int16_t);
        size_t n = samples > 4000 ? 4000 : samples;
        int64_t sum_sq = 0;
        for (size_t i = 0; i < n; ++i) {
            int32_t v = p16[i];
            sum_sq += (int64_t)v * v;
        }
        float rms = 0.0f;
        if (n > 0) {
            rms = sqrtf((float)sum_sq / (float)n);
        }
        ESP_LOGI(TAG, "record stats: samples=%u rms=%.1f first8=[%d,%d,%d,%d,%d,%d,%d,%d]",
                 (unsigned)samples, rms,
                 samples > 0 ? p16[0] : 0,
                 samples > 1 ? p16[1] : 0,
                 samples > 2 ? p16[2] : 0,
                 samples > 3 ? p16[3] : 0,
                 samples > 4 ? p16[4] : 0,
                 samples > 5 ? p16[5] : 0,
                 samples > 6 ? p16[6] : 0,
                 samples > 7 ? p16[7] : 0);

        // 软件增益（x10，限幅）让回放更接近日常听感；如有明显爆音再调小
        const int gain = 10;
        for (size_t i = 0; i < samples; ++i) {
            int32_t v = (int32_t)p16[i] * gain;
            if (v > INT16_MAX) v = INT16_MAX;
            if (v < INT16_MIN) v = INT16_MIN;
            p16[i] = (int16_t)v;
        }
    }

    // 回放
    ESP_LOGI(TAG, "start playback...");
    err = app_speak_sound_play_pcm(buf, got);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "playback failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "playback done");
    }

    heap_caps_free(buf);
    vTaskDelete(NULL);
}

esp_err_t task_speak_selftest_start(void)
{
    BaseType_t ok = xTaskCreate(task_entry, "task_speak_selftest", 8192, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

