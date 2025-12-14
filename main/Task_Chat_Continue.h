#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // 例如："http://192.168.31.193:8443"
    const char *base_url;
    const char *user_id;
    const char *language;   // "zh-CN"

    // VAD 参数（按你的需求默认：静音 2s 停止；有效说话 >=1s）
    int frame_ms;           // 默认 20ms
    int silence_stop_ms;    // 默认 2000ms
    int min_voice_ms;       // 默认 1000ms

    // 门限参数（简单能量 VAD）
    float noise_alpha;      // 默认 0.01
    float th_mul;           // 默认 2.2
    float th_min;           // 默认 200.0（平均绝对值门限下限）

    // 播放
    int spk_chunk_bytes;    // 默认 512（越小越容易打断）

    // 录音最大缓存（避免异常长句打爆内存）
    int max_record_ms;      // 默认 15000ms
} task_chat_continue_cfg_t;

esp_err_t task_chat_continue_start(const task_chat_continue_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

