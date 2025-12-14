 #include "App_SpeakState.h"
 
 #include <stdlib.h>
 #include <stdint.h>
 
 #include "freertos/FreeRTOS.h"
 #include "freertos/task.h"
 
 #include "esp_check.h"
 #include "esp_log.h"
 
 #include "App_Speak_Sound.h"
 
 typedef struct {
     app_speak_state_cfg_t cfg;
     app_speak_state_on_change_cb_t cb;
     void *cb_ctx;
 
     TaskHandle_t task;
     volatile app_speak_state_t state;
 } speak_state_ctx_t;
 
 static speak_state_ctx_t s_ctx = {0};
 
 app_speak_state_cfg_t app_speak_state_cfg_default(void)
 {
     app_speak_state_cfg_t c = {
         .window_ms = 500,
         .frame_ms = 20,
         .th_avg_abs = 80,
         .on_need_windows = 3,
         .off_need_windows = 6,
         .task_stack = 4096,
         .task_prio = 5,
         .log_tag = "SpeakState",
         .log_state_change = true,
        .on_audio = NULL,
        .on_audio_ctx = NULL,
     };
     return c;
 }
 
 static void emit_state(app_speak_state_t st)
 {
     s_ctx.state = st;
     if (s_ctx.cfg.log_state_change) {
         ESP_LOGI(s_ctx.cfg.log_tag ? s_ctx.cfg.log_tag : "SpeakState",
                  "状态: %s", (st == APP_SPEAK_STATE_SPEAKING) ? "说话" : "闭嘴");
     }
     if (s_ctx.cb) {
         s_ctx.cb(st, s_ctx.cb_ctx);
     }
 }
 
 static void task_speak_state(void *arg)
 {
     (void)arg;
 
     const char *TAG = s_ctx.cfg.log_tag ? s_ctx.cfg.log_tag : "SpeakState";
 
     app_speak_sound_cfg_t acfg = {0};
     app_speak_sound_get_cfg(&acfg);
 
     const int frame_ms = (s_ctx.cfg.frame_ms > 0) ? s_ctx.cfg.frame_ms : 20;
     const int window_ms = (s_ctx.cfg.window_ms > 0) ? s_ctx.cfg.window_ms : 500;
 
     const int sr = (acfg.sample_rate > 0) ? acfg.sample_rate : 16000;
     const int ch = (acfg.channels > 0) ? acfg.channels : 1;
     const int bps = (acfg.bits_per_sample > 0) ? acfg.bits_per_sample : 16;
     const int bytes_per_sample = bps / 8;
 
     const int samples_per_frame = (sr * frame_ms) / 1000;
     const int bytes_per_frame = samples_per_frame * ch * bytes_per_sample;
 
     uint8_t *frame = (uint8_t *)malloc((size_t)bytes_per_frame);
     if (!frame) {
         ESP_LOGE(TAG, "alloc frame failed (%d bytes)", bytes_per_frame);
         vTaskDelete(NULL);
         return;
     }
 
     const int64_t target_samples = ((int64_t)sr * (int64_t)ch * (int64_t)window_ms) / 1000;
     if (target_samples <= 0) {
         ESP_LOGE(TAG, "bad window_ms=%d", window_ms);
         free(frame);
         vTaskDelete(NULL);
         return;
     }
 
     ESP_LOGI(TAG, "start: window=%dms frame=%dms th=%d on=%d off=%d",
              window_ms,
              frame_ms,
              s_ctx.cfg.th_avg_abs,
              s_ctx.cfg.on_need_windows,
              s_ctx.cfg.off_need_windows);
 
     // 初始状态：闭嘴
     emit_state(APP_SPEAK_STATE_SILENT);
 
     int64_t sum_abs = 0;
     int64_t n_samp = 0;
 
     bool is_speaking = false;
     int on_cnt = 0;
     int off_cnt = 0;
 
     while (1) {
         esp_err_t err = app_speak_sound_mic_read(frame, (size_t)bytes_per_frame);
         if (err != ESP_OK) {
             ESP_LOGE(TAG, "mic read failed: %s", esp_err_to_name(err));
             vTaskDelay(pdMS_TO_TICKS(50));
             continue;
         }
 
        if (s_ctx.cfg.on_audio) {
            // 注意：回调在本任务上下文执行，需尽量短小，避免阻塞 mic 读取
            s_ctx.cfg.on_audio(frame, bytes_per_frame, s_ctx.cfg.on_audio_ctx);
        }

         if (bps == 16) {
             const int16_t *x = (const int16_t *)frame;
             const int n = samples_per_frame * ch;
             for (int i = 0; i < n; ++i) {
                 int32_t v = x[i];
                 if (v < 0) v = -v;
                 sum_abs += v;
             }
             n_samp += n;
         } else {
             // 退化：按字节统计
             for (int i = 0; i < bytes_per_frame; ++i) {
                 sum_abs += frame[i];
             }
             n_samp += bytes_per_frame;
         }
 
         if (n_samp >= target_samples) {
             const int32_t avg_abs = (n_samp > 0) ? (int32_t)(sum_abs / n_samp) : 0;
             const bool voiced = (avg_abs > s_ctx.cfg.th_avg_abs);
 
             if (!is_speaking) {
                 on_cnt = voiced ? (on_cnt + 1) : 0;
                 if (on_cnt >= s_ctx.cfg.on_need_windows) {
                     is_speaking = true;
                     off_cnt = 0;
                     emit_state(APP_SPEAK_STATE_SPEAKING);
                 }
             } else {
                 off_cnt = (!voiced) ? (off_cnt + 1) : 0;
                 if (off_cnt >= s_ctx.cfg.off_need_windows) {
                     is_speaking = false;
                     on_cnt = 0;
                     emit_state(APP_SPEAK_STATE_SILENT);
                 }
             }
 
             sum_abs = 0;
             n_samp = 0;
         }
     }
 }
 
 esp_err_t app_speak_state_start(const app_speak_state_cfg_t *cfg,
                                 app_speak_state_on_change_cb_t on_change,
                                 void *cb_ctx)
 {
     ESP_RETURN_ON_FALSE(s_ctx.task == NULL, ESP_ERR_INVALID_STATE, "SpeakState", "already started");
 
     s_ctx.cfg = cfg ? *cfg : app_speak_state_cfg_default();
     if (s_ctx.cfg.window_ms <= 0) s_ctx.cfg.window_ms = 500;
     if (s_ctx.cfg.frame_ms <= 0) s_ctx.cfg.frame_ms = 20;
     if (s_ctx.cfg.th_avg_abs <= 0) s_ctx.cfg.th_avg_abs = 80;
     if (s_ctx.cfg.on_need_windows <= 0) s_ctx.cfg.on_need_windows = 3;
     if (s_ctx.cfg.off_need_windows <= 0) s_ctx.cfg.off_need_windows = 6;
     if (s_ctx.cfg.task_stack <= 0) s_ctx.cfg.task_stack = 4096;
     if (s_ctx.cfg.task_prio <= 0) s_ctx.cfg.task_prio = 5;
     if (!s_ctx.cfg.log_tag) s_ctx.cfg.log_tag = "SpeakState";
 
     s_ctx.cb = on_change;
     s_ctx.cb_ctx = cb_ctx;
     s_ctx.state = APP_SPEAK_STATE_SILENT;
 
     BaseType_t ok = xTaskCreate(task_speak_state,
                                "task_speak_state",
                                s_ctx.cfg.task_stack,
                                NULL,
                                s_ctx.cfg.task_prio,
                                &s_ctx.task);
     return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
 }
 
 app_speak_state_t app_speak_state_get(void)
 {
     return s_ctx.state;
 }
 
