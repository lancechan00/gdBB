/*
 * gdBB - audio selftest app_main
 *
 * - 先保留 esp_secure_cert_mgr/DS 读取能力（后续用于 HTTPS/WSS 加速等）
 * - 先实现音频链路自检：
 *   1) Task_Sound_Selftest: 播放 1kHz 测试音
 *   2) Task_Speak_Selftest: 录 5 秒并回放
 */

#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

// 保留加密/DS 相关（后续用于 HTTPS/WSS/签名加速）
#include "rsa_sign_alt.h"
#include "esp_secure_cert_read.h"

#include "App_Speak_Sound.h"
#include "Task_Sound_Selftest.h"
#include "Task_Speak_Selftest.h"
#include "Task_v3interface_selftest.h"
#include "Task_Chat_Continue.h"

static const char *TAG = "gdBB_main";

static void security_ds_sanity_check(void)
{
    // DS ctx 由 esp_secure_cert_mgr 管理，不要 free
    esp_ds_data_ctx_t *ds = esp_secure_cert_get_ds_ctx();
    if (!ds) {
        ESP_LOGW(TAG, "DS ctx not found (check secure cert partition/provisioning)");
    } else {
        ESP_LOGI(TAG, "DS ctx ok");
    }

    // 设备证书由库分配，使用后释放
    char *device_cert = NULL;
    uint32_t len = 0;
    esp_err_t ret = esp_secure_cert_get_device_cert(&device_cert, &len);
    if (ret == ESP_OK && device_cert && len > 0) {
        ESP_LOGI(TAG, "device cert ok, len=%" PRIu32, len);
        free(device_cert);
    } else {
        ESP_LOGW(TAG, "device cert not available yet: %s", esp_err_to_name(ret));
        if (device_cert) free(device_cert);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "startup, free heap=%" PRIu32 ", idf=%s",
             esp_get_free_heap_size(), esp_get_idf_version());

    ESP_ERROR_CHECK(nvs_flash_init());
    security_ds_sanity_check();

    // 初始化音频（BSP + esp_codec_dev）
    app_speak_sound_cfg_t cfg = {
        .sample_rate = 16000,
        .channels = 1,
        .bits_per_sample = 16,
        .volume = 95,
        .mic_gain_db = 42,
    };
    ESP_ERROR_CHECK(app_speak_sound_init(&cfg));

    // 扬声器测试音
    ESP_ERROR_CHECK(task_sound_selftest_start());
    vTaskDelay(pdMS_TO_TICKS(1200));

    // v3 HTTP 接口自检：你已验证 OK，这里先注释，专注测试麦克风
    // ESP_ERROR_CHECK(task_v3interface_selftest_start());

    // 麦克风自检：录 5 秒并回放（验证 RX->TX）
    // ESP_ERROR_CHECK(task_speak_selftest_start());

    // 连续语音助手（当前：WS 逻辑先用日志模拟，不做真实连接/发送）
    task_chat_continue_cfg_t chat_cfg = {
        .base_url = "http://192.168.31.193:8443",
        .user_id = "demo",
        .language = "zh-CN",
        .frame_ms = 20,
        .silence_stop_ms = 2000,
        .min_voice_ms = 1000,
        .noise_alpha = 0.01f,
        .th_mul = 2.2f,
        .th_min = 200.0f,
        .spk_chunk_bytes = 512,
        .max_record_ms = 15000,
    };
    ESP_ERROR_CHECK(task_chat_continue_start(&chat_cfg));
}
