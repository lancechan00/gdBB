#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // 例如："http://192.168.31.193:8443" 或 "https://xxx"
    const char *base_url;
    // 默认 "/v1/robot/event"
    const char *event_path;
    // 默认音频格式（建议自检用 pcm，避免端上 MP3 解码依赖）
    const char *af;
    // "stream" 或 "single"
    const char *mode;
    // 下行 audio 分片大小（Base64 前原始字节数），默认 500
    int chunk_bytes;
    // HTTP 超时
    int timeout_ms;
} app_rb3_cfg_t;

typedef struct {
    char req[32];
    char rid[64];
    char anim[32];
    char motion[32];
    char af[32];
    char text[256];
} app_rb3_meta_t;

typedef esp_err_t (*app_rb3_on_audio_cb)(const uint8_t *pcm, size_t pcm_len, bool is_last, void *ctx);

/**
 * @brief 发送 v3 服务端事件请求（HTTP: POST /v1/robot/event），并按序回调输出 audio 分片
 *
 * @note 本实现是“驱动层”封装：负责 HTTP、JSON 解析、Base64 解码。
 *       播放/队列/状态机由上层 task 负责。
 */
esp_err_t app_rb3_http_event_stream(const app_rb3_cfg_t *cfg,
                                   const char *event_name,
                                   const char *req_id,
                                   const char *user_id,
                                   app_rb3_meta_t *out_meta, // 可为 NULL
                                   app_rb3_on_audio_cb on_audio,
                                   void *cb_ctx);

/**
 * @brief 默认配置（只填 base_url 即可用）
 */
app_rb3_cfg_t app_rb3_cfg_default(const char *base_url);

#ifdef __cplusplus
}
#endif

