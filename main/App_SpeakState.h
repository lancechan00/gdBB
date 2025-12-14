 #pragma once
 
 #include <stdbool.h>
 #include "esp_err.h"
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 
 typedef enum {
     APP_SPEAK_STATE_SILENT = 0,
     APP_SPEAK_STATE_SPEAKING = 1,
 } app_speak_state_t;
 
 typedef void (*app_speak_state_on_change_cb_t)(app_speak_state_t state, void *ctx);
typedef void (*app_speak_state_on_audio_cb_t)(const uint8_t *pcm, int pcm_len, void *ctx);
 
 typedef struct {
     // 窗口/帧
     int window_ms;               // 默认 500ms（0.5s）
     int frame_ms;                // 默认 20ms
 
     // 阈值：窗口 avg_abs > th_avg_abs 认为“有声”
     int th_avg_abs;              // 默认 80
 
     // 状态机持续窗口数
     int on_need_windows;         // 默认 3（0.5s*3=1.5s）
     int off_need_windows;        // 默认 6（0.5s*6=3s）
 
     // 任务参数
     int task_stack;              // 默认 4096
     int task_prio;               // 默认 5
     const char *log_tag;         // 默认 "SpeakState"
     bool log_state_change;       // 默认 true

    // 可选：每次成功读取一帧麦克风数据都会回调（回调需尽量轻量，勿阻塞）
    app_speak_state_on_audio_cb_t on_audio;
    void *on_audio_ctx;
 } app_speak_state_cfg_t;
 
 app_speak_state_cfg_t app_speak_state_cfg_default(void);
 
 /**
  * @brief 启动说话状态检测任务（单例）。
  *
  * - 状态变化时：可回调 + 可打 log
  * - 需要先 app_speak_sound_init()，因为内部用 app_speak_sound_mic_read()
  */
 esp_err_t app_speak_state_start(const app_speak_state_cfg_t *cfg,
                                 app_speak_state_on_change_cb_t on_change,
                                 void *cb_ctx);
 
 app_speak_state_t app_speak_state_get(void);
 
 #ifdef __cplusplus
 }
 #endif
 
