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
    // Realtime voice（voice_rt WS 使用）
    const char *voice;
    // Realtime model（voice_rt WS 使用）
    const char *model;
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
typedef bool (*app_rb3_should_abort_cb)(void *ctx);

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
 * @brief 发送语音输入（HTTP: POST /v1/robot/voice），并按序回调输出 audio 分片
 *
 * @note 这是“整句上传”的实现：端上用 VAD 判停后把 PCM Base64 一次性提交。
 *       若后续要更低延迟/边说边回，请改用 WS（你们当前为 /v1/robot/voice_rt）。
 */
esp_err_t app_rb3_http_voice_stream(const app_rb3_cfg_t *cfg,
                                   const uint8_t *pcm,
                                   size_t pcm_len,
                                   const char *audio_format, // 例如 "pcm_16k_16bit" / "wav_16k_16bit"
                                   const char *language,     // 例如 "zh-CN"
                                   const char *req_id,
                                   const char *user_id,
                                   app_rb3_meta_t *out_meta, // 可为 NULL
                                   app_rb3_on_audio_cb on_audio,
                                   void *cb_ctx);

/**
 * @brief WebSocket 流式语音（WS: /v1/robot/voice_rt）
 *
 * @note 发送 start + 二进制音频分片 + end；接收 meta/audio/asr_text。
 *       若 should_abort 返回 true，会立即中断并关闭连接（用于打断/取消）。
 */
esp_err_t app_rb3_ws_voice_stream(const app_rb3_cfg_t *cfg,
                                 const uint8_t *pcm,
                                 size_t pcm_len,
                                 int send_chunk_bytes,      // 建议 3~8KB
                                 const char *audio_format,  // 例如 "pcm_16k_16bit"
                                 const char *language,      // 例如 "zh-CN"
                                 const char *req_id,
                                 const char *user_id,
                                 app_rb3_meta_t *out_meta,  // 可为 NULL
                                 app_rb3_on_audio_cb on_audio,
                                 void *cb_ctx,
                                 app_rb3_should_abort_cb should_abort,
                                 void *abort_ctx);

/**
 * @brief 默认配置（只填 base_url 即可用）
 */
app_rb3_cfg_t app_rb3_cfg_default(const char *base_url);

#ifdef __cplusplus
}
#endif

