#include "Task_Chat_Continue.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#include "App_Speak_Sound.h"
#include "App_RobotBrainV3.h"
#include "App_SpeakState.h"

static const char *TAG = "Task_Chat_Continue";

typedef struct {
    uint8_t *pcm;
    size_t pcm_len;
    uint32_t turn_id;
} utterance_t;

typedef enum {
    CHAT_PHASE_SILENT = 0,   // 静默期：无 WS（当前仅日志模拟）
    CHAT_PHASE_WAITING = 1,  // 等待期：WS 常连但不上传（当前仅日志模拟）
    CHAT_PHASE_WAKE = 2,     // 唤醒期：持续上传（当前仅日志模拟）
} chat_phase_t;

typedef enum {
    CHAT_EVT_SPEAK_ON = 1,
    CHAT_EVT_SPEAK_OFF = 2,
} chat_evt_type_t;

typedef struct {
    chat_evt_type_t type;
    uint32_t tick;
} chat_evt_t;

typedef struct {
    task_chat_continue_cfg_t cfg;
    app_speak_sound_cfg_t audio_cfg;

    QueueHandle_t q_utt;          // utterance_t*
    QueueHandle_t q_evt;          // chat_evt_t
    RingbufHandle_t rb_play;      // raw PCM bytes

    volatile uint32_t turn_id;    // 每次开始说话 +1（用于打断/丢弃旧音频）
    volatile bool playing;
    volatile uint32_t abort_token; // 递增即可触发打断（避免 bool 粘滞）

    // VAD
    float noise;
    int start_on_frames;          // 防抖：连续多少帧算开始

    // phase
    volatile chat_phase_t phase;
    uint32_t last_activity_tick;

    // pre-roll circular buffer (PSRAM)
    uint8_t *pre_rb;
    size_t pre_cap;
    size_t pre_w;
    bool pre_full;
    size_t pre_preroll_bytes;     // 1.5s 对应 bytes

    // wake logging (rate limit)
    uint32_t wake_last_log_tick;
    size_t wake_rt_bytes_acc;
} chat_ctx_t;

typedef struct {
    chat_ctx_t *c;
    uint32_t *last_abort_seen;
} abort_ctx_t;

static bool should_abort_ws(void *ctx)
{
    abort_ctx_t *a = (abort_ctx_t *)ctx;
    if (!a || !a->c || !a->last_abort_seen) return true;
    return a->c->abort_token != *(a->last_abort_seen);
}

static float frame_mean_abs_16(const int16_t *x, int n)
{
    int64_t sum = 0;
    for (int i = 0; i < n; ++i) {
        int32_t v = x[i];
        if (v < 0) v = -v;
        sum += v;
    }
    return (n > 0) ? (float)sum / (float)n : 0.0f;
}

static esp_err_t on_audio_push_rb(const uint8_t *pcm, size_t pcm_len, bool is_last, void *ctx)
{
    (void)is_last;
    chat_ctx_t *c = (chat_ctx_t *)ctx;
    if (!c || !pcm || pcm_len == 0) return ESP_ERR_INVALID_ARG;

    // 如果已经被打断（turn_id 变化），丢弃后续 audio
    uint32_t cur_turn = c->turn_id;

    // copy to ringbuffer (ringbuf 负责分配内部节点内存)
    BaseType_t ok = xRingbufferSend(c->rb_play, pcm, pcm_len, pdMS_TO_TICKS(200));
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "play ringbuf full, drop %u bytes (turn=%" PRIu32 ")", (unsigned)pcm_len, cur_turn);
    } else {
        c->playing = true;
    }
    return ESP_OK;
}

static void flush_play_rb(chat_ctx_t *c)
{
    if (!c || !c->rb_play) return;
    size_t item_size = 0;
    void *item = NULL;
    while ((item = xRingbufferReceive(c->rb_play, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(c->rb_play, item);
    }
}

static void task_play(void *arg)
{
    chat_ctx_t *c = (chat_ctx_t *)arg;
    const int chunk = (c->cfg.spk_chunk_bytes > 0) ? c->cfg.spk_chunk_bytes : 512;
    uint32_t last_abort = c->abort_token;

    while (1) {
        // 若收到打断请求，即使当前无音频也要清一次队列
        if (c->abort_token != last_abort) {
            flush_play_rb(c);
            c->playing = false;
            last_abort = c->abort_token;
            vTaskDelay(pdMS_TO_TICKS(20)); // 让 DMA 自然消耗一点点，降低爆音概率
        }

        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(c->rb_play, &item_size, pdMS_TO_TICKS(200));
        if (!item) {
            c->playing = false;
            continue;
        }

        // 分小块写，便于“说话即打断”
        size_t off = 0;
        while (off < item_size) {
            if (c->abort_token != last_abort) {
                break;
            }
            size_t n = item_size - off;
            if (n > (size_t)chunk) n = (size_t)chunk;
            (void)app_speak_sound_spk_write(item + off, n);
            off += n;
        }

        vRingbufferReturnItem(c->rb_play, item);

        if (c->abort_token != last_abort) {
            flush_play_rb(c);
            c->playing = false;
            last_abort = c->abort_token;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void prebuf_write(chat_ctx_t *c, const uint8_t *data, size_t len)
{
    if (!c || !c->pre_rb || c->pre_cap == 0 || !data || len == 0) return;
    // 静默期不存：按需求“静默->唤醒时无前1.5s”
    if (c->phase == CHAT_PHASE_SILENT) return;

    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        size_t space = c->pre_cap - c->pre_w;
        if (n > space) n = space;
        memcpy(c->pre_rb + c->pre_w, data + off, n);
        c->pre_w += n;
        off += n;
        if (c->pre_w >= c->pre_cap) {
            c->pre_w = 0;
            c->pre_full = true;
        }
    }
}

static void on_speak_state_change(app_speak_state_t st, void *ctx)
{
    chat_ctx_t *c = (chat_ctx_t *)ctx;
    if (!c || !c->q_evt) return;
    chat_evt_t ev = {
        .type = (st == APP_SPEAK_STATE_SPEAKING) ? CHAT_EVT_SPEAK_ON : CHAT_EVT_SPEAK_OFF,
        .tick = xTaskGetTickCount(),
    };
    (void)xQueueSend(c->q_evt, &ev, 0);
}

static void on_speak_audio_frame(const uint8_t *pcm, int pcm_len, void *ctx)
{
    chat_ctx_t *c = (chat_ctx_t *)ctx;
    if (!c || !pcm || pcm_len <= 0) return;

    prebuf_write(c, pcm, (size_t)pcm_len);

    // 唤醒期：模拟“实时上传”日志（限频，避免刷屏）
    if (c->phase == CHAT_PHASE_WAKE) {
        c->wake_rt_bytes_acc += (size_t)pcm_len;
        uint32_t now = xTaskGetTickCount();
        if (now - c->wake_last_log_tick >= pdMS_TO_TICKS(500)) {
            c->wake_last_log_tick = now;
            ESP_LOGI(TAG, "唤醒期: 模拟实时上传中... +%u bytes", (unsigned)c->wake_rt_bytes_acc);
            c->wake_rt_bytes_acc = 0;
        }
    }
}

static void task_net(void *arg)
{
    chat_ctx_t *c = (chat_ctx_t *)arg;
    // 先不做真实 WS：只用日志模拟三态 + “前1.5s + 实时”发送顺序
    c->phase = CHAT_PHASE_WAITING;
    c->last_activity_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "状态切换: 启动 -> 等待期（WS常连但不上传，开始循环存音频）");
    ESP_LOGI(TAG, "规则: 等待期空闲60s -> 静默期；静默期说话 -> 唤醒期(无前1.5s)；等待期说话 -> 唤醒期(先前1.5s再实时)；说完->等待期(end不关WS)");

    const uint32_t idle_to_silent_ms = 60000;
    const TickType_t wait_ticks = pdMS_TO_TICKS(200);

    while (1) {
        chat_evt_t ev = {0};
        TickType_t now = xTaskGetTickCount();
        if (xQueueReceive(c->q_evt, &ev, wait_ticks) == pdTRUE) {
            uint32_t tnow = xTaskGetTickCount();
            if (ev.type == CHAT_EVT_SPEAK_ON) {
                if (c->phase == CHAT_PHASE_WAITING) {
                    c->phase = CHAT_PHASE_WAKE;
                    c->last_activity_tick = tnow;
                    c->wake_last_log_tick = tnow;
                    c->wake_rt_bytes_acc = 0;
                    ESP_LOGI(TAG, "状态切换: 等待期 -> 唤醒期");
                    ESP_LOGI(TAG, "模拟发送: 先发前1.5s缓存 bytes=%u，再发实时数据", (unsigned)c->pre_preroll_bytes);
                } else if (c->phase == CHAT_PHASE_SILENT) {
                    c->phase = CHAT_PHASE_WAKE;
                    c->last_activity_tick = tnow;
                    c->wake_last_log_tick = tnow;
                    c->wake_rt_bytes_acc = 0;
                    ESP_LOGI(TAG, "状态切换: 静默期 -> 唤醒期");
                    ESP_LOGI(TAG, "模拟发送: 静默期无前1.5s，直接发实时数据");
                } else {
                    c->last_activity_tick = tnow;
                }
            } else if (ev.type == CHAT_EVT_SPEAK_OFF) {
                if (c->phase == CHAT_PHASE_WAKE) {
                    c->phase = CHAT_PHASE_WAITING;
                    c->last_activity_tick = tnow;
                    ESP_LOGI(TAG, "状态切换: 唤醒期 -> 等待期");
                    ESP_LOGI(TAG, "模拟发送: end（保持WS，不关闭；回到等待期继续循环存音频）");
                } else {
                    c->last_activity_tick = tnow;
                }
            }
        } else {
            // timeout：检查等待期是否进入静默
            if (c->phase == CHAT_PHASE_WAITING) {
                uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(now - c->last_activity_tick);
                if (elapsed_ms >= idle_to_silent_ms) {
                    c->phase = CHAT_PHASE_SILENT;
                    ESP_LOGI(TAG, "状态切换: 等待期 -> 静默期（空闲>=60s，模拟关闭WS/不保持连接）");
                }
            }
        }
    }
}

static void task_vad(void *arg)
{
    chat_ctx_t *c = (chat_ctx_t *)arg;

    const int frame_ms = (c->cfg.frame_ms > 0) ? c->cfg.frame_ms : 20;
    const int sr = (c->audio_cfg.sample_rate > 0) ? c->audio_cfg.sample_rate : 16000;
    const int ch = (c->audio_cfg.channels > 0) ? c->audio_cfg.channels : 1;
    const int bps = (c->audio_cfg.bits_per_sample > 0) ? c->audio_cfg.bits_per_sample : 16;
    const int bytes_per_sample = bps / 8;
    const int samples_per_frame = (sr * frame_ms) / 1000;
    const int bytes_per_frame = samples_per_frame * ch * bytes_per_sample;

    const int silence_stop_ms = (c->cfg.silence_stop_ms > 0) ? c->cfg.silence_stop_ms : 2000;
    const int min_voice_ms = (c->cfg.min_voice_ms > 0) ? c->cfg.min_voice_ms : 1000;
    const int max_record_ms = (c->cfg.max_record_ms > 0) ? c->cfg.max_record_ms : 15000;

    const int stop_silence_frames = silence_stop_ms / frame_ms;
    const int min_voice_frames = min_voice_ms / frame_ms;
    const int max_frames = max_record_ms / frame_ms;

    const float alpha = (c->cfg.noise_alpha > 0.0f && c->cfg.noise_alpha < 1.0f) ? c->cfg.noise_alpha : 0.01f;
    const float th_mul = (c->cfg.th_mul > 0.5f) ? c->cfg.th_mul : 2.2f;
    const float th_min = (c->cfg.th_min > 0.0f) ? c->cfg.th_min : 200.0f;

    ESP_LOGI(TAG, "VAD cfg: frame=%dms bytes/frame=%d stop_silence=%dframes(=%dms) min_voice=%dframes(=%dms)",
             frame_ms, bytes_per_frame, stop_silence_frames, silence_stop_ms, min_voice_frames, min_voice_ms);

    uint8_t *frame = (uint8_t *)malloc((size_t)bytes_per_frame);
    if (!frame) {
        ESP_LOGE(TAG, "alloc frame failed");
        vTaskDelete(NULL);
        return;
    }

    // 预缓冲：用于“说话不足 1s 视为无效”，先缓存到达 1s 才真正发起一次请求
    const int prebuf_frames = min_voice_frames;
    const size_t prebuf_cap = (size_t)prebuf_frames * (size_t)bytes_per_frame;
    uint8_t *prebuf = (uint8_t *)malloc(prebuf_cap);
    if (!prebuf) {
        ESP_LOGE(TAG, "alloc prebuf failed");
        free(frame);
        vTaskDelete(NULL);
        return;
    }

    bool in_speech = false;
    int on_cnt = 0;
    int off_cnt = 0;
    int voice_frames = 0;
    int total_frames = 0;

    size_t prebuf_len = 0;
    uint8_t *rec = NULL;
    size_t rec_len = 0;
    size_t rec_cap = 0;
    bool accepted = false; // 已达到 >=1s，有效输入

    // 噪声底初始化：给个非零，避免一开始门限为 0
    if (c->noise < 1.0f) c->noise = 300.0f;

    while (1) {
        esp_err_t err = app_speak_sound_mic_read(frame, (size_t)bytes_per_frame);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mic read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        float level = 0.0f;
        if (bps == 16) {
            level = frame_mean_abs_16((const int16_t *)frame, samples_per_frame * ch);
        } else {
            // 目前工程只用 16bit，其他先用退化估计
            const uint8_t *p = frame;
            int64_t sum = 0;
            for (int i = 0; i < bytes_per_frame; ++i) sum += p[i];
            level = (float)sum / (float)bytes_per_frame;
        }

        float th = c->noise * th_mul;
        if (th < th_min) th = th_min;
        const bool voice = (level > th);

        if (!in_speech) {
            // 静音期更新噪声底
            c->noise = c->noise * (1.0f - alpha) + level * alpha;

            if (voice) {
                on_cnt++;
            } else {
                on_cnt = 0;
            }

            if (on_cnt >= c->start_on_frames) {
                // 进入说话
                in_speech = true;
                c->turn_id++;                 // 新一轮 turn
                c->abort_token++;              // 若正在播放，立刻打断
                flush_play_rb(c);

                // reset
                off_cnt = 0;
                voice_frames = 0;
                total_frames = 0;
                prebuf_len = 0;
                accepted = false;

                if (rec) {
                    free(rec);
                    rec = NULL;
                }
                rec_len = 0;
                rec_cap = 0;

                ESP_LOGI(TAG, "speech start (turn=%" PRIu32 ", noise=%.1f th=%.1f)", c->turn_id, c->noise, th);
            }
            continue;
        }

        // in_speech == true：持续缓存帧
        total_frames++;
        if (voice) {
            voice_frames++;
            off_cnt = 0;
        } else {
            off_cnt++;
        }

        // 先塞 prebuf（最多 1 秒）
        if (!accepted && prebuf_len + (size_t)bytes_per_frame <= prebuf_cap) {
            memcpy(prebuf + prebuf_len, frame, (size_t)bytes_per_frame);
            prebuf_len += (size_t)bytes_per_frame;
        }

        // 达到 >=1s 认为有效：把 prebuf + 后续帧都累计成一次 utterance（整句上传）
        if (!accepted && voice_frames >= min_voice_frames) {
            accepted = true;
            // 把 prebuf 转入 rec
            rec_cap = prebuf_len + (size_t)(bytes_per_frame * 10); // 先给点余量
            rec = (uint8_t *)malloc(rec_cap);
            if (!rec) {
                ESP_LOGE(TAG, "alloc rec failed, drop utterance");
                // 回到静默
                in_speech = false;
                on_cnt = 0;
                continue;
            }
            memcpy(rec, prebuf, prebuf_len);
            rec_len = prebuf_len;
            ESP_LOGI(TAG, "speech accepted (>= %dms), start buffering/upload later", min_voice_ms);
        }

        // accepted 后，把当前帧继续 append 到 rec
        if (accepted) {
            if (rec_len + (size_t)bytes_per_frame > rec_cap) {
                size_t nc = rec_cap ? rec_cap * 2 : (size_t)bytes_per_frame * 64;
                while (nc < rec_len + (size_t)bytes_per_frame) nc *= 2;
                // cap：max_record_ms
                const size_t hard_cap = (size_t)max_frames * (size_t)bytes_per_frame;
                if (nc > hard_cap) nc = hard_cap;
                if (rec_len + (size_t)bytes_per_frame > nc) {
                    // 到达硬上限，强制结束
                    off_cnt = stop_silence_frames;
                } else {
                    uint8_t *p = (uint8_t *)realloc(rec, nc);
                    if (p) {
                        rec = p;
                        rec_cap = nc;
                    } else {
                        // 内存不足，强制结束
                        off_cnt = stop_silence_frames;
                    }
                }
            }
            if (rec && rec_len + (size_t)bytes_per_frame <= rec_cap) {
                memcpy(rec + rec_len, frame, (size_t)bytes_per_frame);
                rec_len += (size_t)bytes_per_frame;
            }
        }

        // 结束条件：静音连续 2s 或超过最大时长
        if (off_cnt >= stop_silence_frames || total_frames >= max_frames) {
            const bool valid = (voice_frames >= min_voice_frames);
            if (!valid) {
                // 无效：说话不到 1s，直接回到静默
                ESP_LOGI(TAG, "speech too short (<%dms), ignore and back to idle", min_voice_ms);
                if (rec) {
                    free(rec);
                    rec = NULL;
                }
            } else {
                // 投递给 NET 任务
                utterance_t utt = {
                    .pcm = rec,
                    .pcm_len = rec_len,
                    .turn_id = c->turn_id,
                };
                rec = NULL; // ownership moved
                if (xQueueSend(c->q_utt, &utt, pdMS_TO_TICKS(50)) != pdTRUE) {
                    ESP_LOGW(TAG, "utt queue full, drop");
                    free(utt.pcm);
                } else {
                    ESP_LOGI(TAG, "speech end, send to server: %u bytes (turn=%" PRIu32 ")", (unsigned)utt.pcm_len, utt.turn_id);
                }
            }

            // reset to idle
            in_speech = false;
            on_cnt = 0;
            off_cnt = 0;
            voice_frames = 0;
            total_frames = 0;
            prebuf_len = 0;
            accepted = false;
        }
    }
}

static task_chat_continue_cfg_t cfg_default(void)
{
    task_chat_continue_cfg_t c = {
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
    return c;
}

esp_err_t task_chat_continue_start(const task_chat_continue_cfg_t *cfg)
{
    chat_ctx_t *c = (chat_ctx_t *)calloc(1, sizeof(chat_ctx_t));
    ESP_RETURN_ON_FALSE(c, ESP_ERR_NO_MEM, TAG, "alloc ctx failed");

    c->cfg = cfg ? *cfg : cfg_default();
    app_speak_sound_get_cfg(&c->audio_cfg);

    c->q_evt = xQueueCreate(8, sizeof(chat_evt_t));
    ESP_RETURN_ON_FALSE(c->q_evt, ESP_ERR_NO_MEM, TAG, "create q_evt failed");

    // 播放 ringbuffer：先给 64KB，足够缓存短句 TTS，后续可调大
    c->rb_play = xRingbufferCreate(64 * 1024, RINGBUF_TYPE_BYTEBUF);
    ESP_RETURN_ON_FALSE(c->rb_play, ESP_ERR_NO_MEM, TAG, "create rb_play failed");

    // pre-roll buffer：默认存 5 秒，取其中前 1.5 秒用于“提前上传”
    const int sr = (c->audio_cfg.sample_rate > 0) ? c->audio_cfg.sample_rate : 16000;
    const int ch = (c->audio_cfg.channels > 0) ? c->audio_cfg.channels : 1;
    const int bps = (c->audio_cfg.bits_per_sample > 0) ? c->audio_cfg.bits_per_sample : 16;
    const int bytes_per_sample = bps / 8;
    const size_t bytes_per_sec = (size_t)sr * (size_t)ch * (size_t)bytes_per_sample;
    c->pre_cap = bytes_per_sec * 5; // 5s
    c->pre_preroll_bytes = (bytes_per_sec * 1500) / 1000; // 1.5s
    c->pre_rb = (uint8_t *)heap_caps_malloc(c->pre_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c->pre_rb) {
        ESP_LOGW(TAG, "PSRAM alloc prebuf failed, fallback to internal heap (%u bytes)", (unsigned)c->pre_cap);
        c->pre_rb = (uint8_t *)malloc(c->pre_cap);
    }
    ESP_RETURN_ON_FALSE(c->pre_rb, ESP_ERR_NO_MEM, TAG, "alloc prebuf failed");
    c->pre_w = 0;
    c->pre_full = false;
    c->phase = CHAT_PHASE_WAITING;

    // 启动 SpeakState：由它独占 mic_read；Continue 通过回调拿到音频帧与说话状态
    app_speak_state_cfg_t scfg = app_speak_state_cfg_default();
    scfg.window_ms = 500;
    scfg.frame_ms = 20;
    scfg.th_avg_abs = 60;          // 默认：avg_abs > 90 才算有声
    scfg.on_need_windows = 3;      // 0.5s*4=2s
    scfg.off_need_windows = 6;     // 0.5s*6=3s
    scfg.log_state_change = false; // 由 Continue 统一打印“静默/等待/唤醒”
    scfg.on_audio = on_speak_audio_frame;
    scfg.on_audio_ctx = c;
    ESP_RETURN_ON_ERROR(app_speak_state_start(&scfg, on_speak_state_change, c), TAG, "start speak state failed");

    BaseType_t ok1 = xTaskCreate(task_play, "task_chat_play", 4096, c, 6, NULL);
    BaseType_t ok2 = xTaskCreate(task_net, "task_chat_state", 6144, c, 5, NULL);
    ESP_RETURN_ON_FALSE(ok1 == pdPASS && ok2 == pdPASS, ESP_FAIL, TAG, "create task failed");

    ESP_LOGI(TAG, "Task_Chat_Continue started, base_url=%s", c->cfg.base_url ? c->cfg.base_url : "(null)");
    return ESP_OK;
}

