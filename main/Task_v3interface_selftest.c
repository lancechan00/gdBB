#include "Task_v3interface_selftest.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "protocol_examples_common.h"

#include "App_RobotBrainV3.h"
#include "App_Speak_Sound.h"

static const char *TAG = "Task_v3interface_selftest";

typedef struct {
    size_t total;
} play_ctx_t;

static esp_err_t on_audio_pcm(const uint8_t *pcm, size_t pcm_len, bool is_last, void *ctx)
{
    play_ctx_t *pc = (play_ctx_t *)ctx;
    pc->total += pcm_len;
    // 直接播放（要求 cfg.af 选择 pcm_16k_16bit 之类的 raw PCM）
    esp_err_t err = app_speak_sound_play_pcm(pcm, pcm_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "play pcm failed: %s", esp_err_to_name(err));
        return err;
    }
    if (is_last) {
        ESP_LOGI(TAG, "audio stream done, total=%u bytes", (unsigned)pc->total);
    }
    return ESP_OK;
}

static void task_entry(void *arg)
{
    (void)arg;

    // 网络初始化（自检用，直接用 IDF 示例组件连接 Wi-Fi）
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    // 这里按你之前工程的习惯：用内网服务地址（你可后续改成 Kconfig）
    app_rb3_cfg_t cfg = app_rb3_cfg_default("http://192.168.31.193:8443");
    // 自检默认用 PCM，下行拿到啥就能播啥
    cfg.af = "pcm_16k_16bit";
    cfg.mode = "stream";
    cfg.chunk_bytes = 500;

    app_rb3_meta_t meta = {0};
    play_ctx_t pc = {0};

    ESP_LOGI(TAG, "request event=idle ...");
    esp_err_t err = app_rb3_http_event_stream(&cfg,
                                              "idle",
                                              "r_selftest",
                                              "demo",
                                              &meta,
                                              on_audio_pcm,
                                              &pc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "v3 http event failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "text=%s anim=%s motion=%s af=%s req=%s rid=%s",
                 meta.text, meta.anim, meta.motion, meta.af, meta.req, meta.rid);
    }

    vTaskDelete(NULL);
}

esp_err_t task_v3interface_selftest_start(void)
{
    BaseType_t ok = xTaskCreate(task_entry, "task_v3if_selftest", 8192, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

