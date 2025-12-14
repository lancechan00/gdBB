#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate;        // Hz, e.g. 16000
    int channels;           // 1 or 2
    int bits_per_sample;    // 16
    int volume;             // 0..100
    int mic_gain_db;        // codec dependent, best-effort
} app_speak_sound_cfg_t;

/**
 * @brief 初始化音频子系统（BSP + ES8311 + esp_codec_dev）
 *
 * - 内部会调用 Waveshare BSP: bsp_audio_init() / bsp_audio_codec_*_init()
 * - 内部会 open speaker/mic 设备
 */
esp_err_t app_speak_sound_init(const app_speak_sound_cfg_t *cfg);

/**
 * @brief 获取当前音频配置（init 后有效）
 */
void app_speak_sound_get_cfg(app_speak_sound_cfg_t *out_cfg);

/**
 * @brief 播放一段正弦测试音
 */
esp_err_t app_speak_sound_play_tone(int freq_hz, int duration_ms);

/**
 * @brief 录音到用户缓冲（最多录 duration_ms 或 buf 满）
 *
 * @param buf      输出 PCM 数据缓冲
 * @param buf_bytes 缓冲大小（字节）
 * @param out_bytes 实际录到的字节数（可为 NULL）
 */
esp_err_t app_speak_sound_record(void *buf, size_t buf_bytes, size_t *out_bytes, int duration_ms);

/**
 * @brief 播放 PCM 缓冲
 */
esp_err_t app_speak_sound_play_pcm(const void *buf, size_t bytes);

#ifdef __cplusplus
}
#endif
