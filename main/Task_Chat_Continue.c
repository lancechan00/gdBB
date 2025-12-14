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
    CHAT_PHASE_PLAYBACK = 3, // 播放期：有下行音频在播，播完再回等待期（用于抑制回声触发唤醒）
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

    QueueHandle_t q_evt;          // chat_evt_t
    RingbufHandle_t rb_play;      // raw PCM bytes

    volatile uint32_t turn_id;    // 每次开始说话 +1（用于打断/丢弃旧音频）
    volatile bool playing;
    volatile uint32_t abort_token; // 递增即可触发打断（避免 bool 粘滞）

    // phase
    volatile chat_phase_t phase;
    uint32_t last_activity_tick;

    // audio circular buffer (PSRAM): 始终循环存储麦克风 PCM
    uint8_t *pre_rb;
    size_t pre_cap;
    size_t pre_preroll_bytes;     // 1.5s 对应 bytes
    volatile uint64_t pre_seq_w;  // 已写入总字节数（单调递增）
    uint64_t send_seq_r;          // 唤醒期发送指针（单调递增，<= pre_seq_w）
    size_t bytes_per_sec;         // sr*ch*bps/8

    // send pacing / backlog control
    uint32_t last_catchup_log_tick;

    // WS session (keep-alive in WAITING)
    app_rb3_ws_sess_t *ws;

    // play buffering control
    volatile uint32_t play_bytes_in;
    uint32_t play_prefill_bytes; // 至少缓存多少再开始播（默认 1s）
    uint32_t play_low_wm_bytes;  // 低水位：降到此以下才恢复快速入队
    uint32_t play_high_wm_bytes; // 高水位：超过则对下行做背压
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

// forward decl: used by on_audio_push_rb_track()
static esp_err_t on_audio_push_rb(const uint8_t *pcm, size_t pcm_len, bool is_last, void *ctx);

static bool is_playback_active(chat_ctx_t *c)
{
    if (!c) return false;
    // playing=true 表示 task_play 近期/当前在写 spk；
    // play_bytes_in>0 表示 ringbuf 里仍有待播数据。
    if (c->playing) return true;
    if (__atomic_load_n(&c->play_bytes_in, __ATOMIC_RELAXED) > 0) return true;
    return false;
}

typedef struct {
    chat_ctx_t *c;
    bool *got_audio;
} dl_audio_ctx_t;

static esp_err_t on_audio_push_rb_track(const uint8_t *pcm, size_t pcm_len, bool is_last, void *ctx)
{
    dl_audio_ctx_t *d = (dl_audio_ctx_t *)ctx;
    if (d && d->got_audio && pcm && pcm_len > 0) {
        *(d->got_audio) = true;
    }
    return on_audio_push_rb(pcm, pcm_len, is_last, d ? d->c : NULL);
}

static esp_err_t on_audio_push_rb(const uint8_t *pcm, size_t pcm_len, bool is_last, void *ctx)
{
    (void)is_last;
    chat_ctx_t *c = (chat_ctx_t *)ctx;
    if (!c || !pcm || pcm_len == 0) return ESP_ERR_INVALID_ARG;

    // 如果已经被打断（turn_id 变化），丢弃后续 audio
    uint32_t cur_turn = c->turn_id;

    // 若上层已触发打断，尽快退出（让 ws_recv 结束）
    uint32_t abort0 = c->abort_token;

    // 背压：如果播放缓冲高于高水位，先等它消耗到低水位再继续入队
    // 目的：避免“服务端灌得太快 -> ringbuf 满 -> 丢块 -> 听起来卡”
    while (__atomic_load_n(&c->play_bytes_in, __ATOMIC_RELAXED) > c->play_high_wm_bytes) {
        if (c->abort_token != abort0) return ESP_ERR_INVALID_STATE;
        vTaskDelay(pdMS_TO_TICKS(20));
        if (__atomic_load_n(&c->play_bytes_in, __ATOMIC_RELAXED) <= c->play_low_wm_bytes) break;
    }

    // copy to ringbuffer (ringbuf 负责分配内部节点内存)
    // 不再“满就丢”：而是等一等（同时让 TCP 背压生效）
    for (;;) {
        if (c->abort_token != abort0) return ESP_ERR_INVALID_STATE;
        BaseType_t ok = xRingbufferSend(c->rb_play, pcm, pcm_len, pdMS_TO_TICKS(200));
        if (ok == pdTRUE) {
            (void)__atomic_fetch_add(&c->play_bytes_in, (uint32_t)pcm_len, __ATOMIC_RELAXED);
            c->playing = true;
            return ESP_OK;
        }
        // 仍然满：稍等再试（避免刷屏，只在满时偶尔提示）
        ESP_LOGW(TAG, "play ringbuf full, wait... drop=0 bytes (turn=%" PRIu32 ")", cur_turn);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void flush_play_rb(chat_ctx_t *c)
{
    if (!c || !c->rb_play) return;
    size_t item_size = 0;
    void *item = NULL;
    while ((item = xRingbufferReceive(c->rb_play, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(c->rb_play, item);
    }
    __atomic_store_n(&c->play_bytes_in, 0, __ATOMIC_RELAXED);
}

static void task_play(void *arg)
{
    chat_ctx_t *c = (chat_ctx_t *)arg;
    const int chunk = (c->cfg.spk_chunk_bytes > 0) ? c->cfg.spk_chunk_bytes : 512;
    uint32_t last_abort = c->abort_token;
    bool prefilled = false;

    while (1) {
        // 若收到打断请求，即使当前无音频也要清一次队列
        if (c->abort_token != last_abort) {
            flush_play_rb(c);
            c->playing = false;
            last_abort = c->abort_token;
            prefilled = false;
            vTaskDelay(pdMS_TO_TICKS(20)); // 让 DMA 自然消耗一点点，降低爆音概率
        }

        // 至少缓存一定数据再开始播放（降低网络抖动导致的卡顿）
        if (!prefilled) {
            uint32_t inb = __atomic_load_n(&c->play_bytes_in, __ATOMIC_RELAXED);
            if (inb < c->play_prefill_bytes) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            prefilled = true;
            ESP_LOGI(TAG, "play prefill ok: %u bytes, start playback", (unsigned)inb);
        }

        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(c->rb_play, &item_size, pdMS_TO_TICKS(200));
        if (!item) {
            c->playing = false;
            prefilled = false; // underrun：等下次再凑够 prefill
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
        if (item_size > 0) {
            (void)__atomic_fetch_sub(&c->play_bytes_in, (uint32_t)item_size, __ATOMIC_RELAXED);
        }

        if (c->abort_token != last_abort) {
            flush_play_rb(c);
            c->playing = false;
            last_abort = c->abort_token;
            prefilled = false;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void prebuf_write(chat_ctx_t *c, const uint8_t *data, size_t len)
{
    if (!c || !c->pre_rb || c->pre_cap == 0 || !data || len == 0) return;

    // 统一策略：无论等待期/唤醒期/静默期，都持续循环存音频
    uint64_t seq = __atomic_load_n(&c->pre_seq_w, __ATOMIC_RELAXED);
    size_t w = (size_t)(seq % c->pre_cap);

    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        size_t space = c->pre_cap - w;
        if (n > space) n = space;
        memcpy(c->pre_rb + w, data + off, n);
        w += n;
        off += n;
        if (w >= c->pre_cap) w = 0;
    }

    // 写入完成后再推进 seq，读线程只会读 <= seq 的数据
    __atomic_store_n(&c->pre_seq_w, seq + len, __ATOMIC_RELEASE);
}

static size_t prebuf_copy(chat_ctx_t *c, uint64_t seq, uint8_t *dst, size_t len)
{
    if (!c || !c->pre_rb || !dst || len == 0 || c->pre_cap == 0) return 0;

    size_t off = 0;
    size_t r = (size_t)(seq % c->pre_cap);
    while (off < len) {
        size_t n = len - off;
        size_t space = c->pre_cap - r;
        if (n > space) n = space;
        memcpy(dst + off, c->pre_rb + r, n);
        r += n;
        off += n;
        if (r >= c->pre_cap) r = 0;
    }
    return len;
}

static void on_speak_state_change(app_speak_state_t st, void *ctx)
{
    chat_ctx_t *c = (chat_ctx_t *)ctx;
    if (!c || !c->q_evt) return;

    // 关键：一旦开始说话，立即触发 abort，让 recv/play 能立刻被打断
    if (st == APP_SPEAK_STATE_SPEAKING) {
        // 播放期/播放中：忽略 SPEAK_ON（否则扬声器回灌会立刻再次唤醒）
        if (is_playback_active(c)) {
            return;
        }
        c->abort_token++;
        flush_play_rb(c);
    }

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
}

static void task_net(void *arg)
{
    chat_ctx_t *c = (chat_ctx_t *)arg;
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    app_rb3_cfg_t rb3 = app_rb3_cfg_default(c->cfg.base_url);
    // 关键：下行音频格式要和本机播放采样率一致；全链路改成 24k。
    rb3.af = "pcm_24k_16bit";
    rb3.mode = "stream";
    rb3.chunk_bytes = 500;

    // 等待期默认保持 WS 连接（长连接）
    c->phase = CHAT_PHASE_WAITING;
    c->last_activity_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "状态切换: 启动 -> 等待期（保持WS连接，不上传；持续循环存音频）");

    if (app_rb3_ws_open(&rb3, &c->ws) != ESP_OK) {
        ESP_LOGW(TAG, "ws open failed, will retry on next wake");
        c->ws = NULL;
    }

    const uint32_t idle_to_silent_ms = 60000;
    const size_t max_backlog = c->bytes_per_sec * 3;  // 最多允许落后 3s
    const size_t keep_backlog = c->bytes_per_sec * 1; // 追帧后保留 1s
    const int send_chunk = 4096;                      // SRAM bounce buffer
    uint8_t *txbuf = (uint8_t *)malloc(send_chunk);
    if (!txbuf) {
        ESP_LOGE(TAG, "alloc txbuf failed");
        vTaskDelete(NULL);
        return;
    }

    // 用于 WS 打断：token 变化即 abort
    uint32_t last_abort_seen = c->abort_token;
    abort_ctx_t ab = {
        .c = c,
        .last_abort_seen = &last_abort_seen,
    };

    bool round_active = false;

    while (1) {
        // 播放期：等下行音频播完再回到等待期
        if (c->phase == CHAT_PHASE_PLAYBACK) {
            if (!is_playback_active(c)) {
                ESP_LOGI(TAG, "状态切换: 播放期 -> 等待期（下行播完）");
                c->phase = CHAT_PHASE_WAITING;
                c->last_activity_tick = xTaskGetTickCount();
            } else {
                // 播放中也算“活跃”，避免被 idle->silent 误关 WS
                c->last_activity_tick = xTaskGetTickCount();
            }
        }

        // 处理状态事件（非阻塞）
        chat_evt_t ev = {0};
        while (xQueueReceive(c->q_evt, &ev, 0) == pdTRUE) {
            uint32_t tnow = xTaskGetTickCount();
            c->last_activity_tick = tnow;

            if (ev.type == CHAT_EVT_SPEAK_ON) {
                // 播放期：不允许再次唤醒（抑制回声触发唤醒）
                if (c->phase == CHAT_PHASE_PLAYBACK) {
                    continue;
                }
                if (c->phase == CHAT_PHASE_WAITING) {
                    ESP_LOGI(TAG, "状态切换: 等待期 -> 唤醒期");
                } else if (c->phase == CHAT_PHASE_SILENT) {
                    ESP_LOGI(TAG, "状态切换: 静默期 -> 唤醒期");
                }
                c->phase = CHAT_PHASE_WAKE;
                round_active = true;
                last_abort_seen = c->abort_token;

                // 确保 WS 已连接
                if (!c->ws || !app_rb3_ws_is_connected(c->ws)) {
                    if (c->ws) app_rb3_ws_close(c->ws);
                    c->ws = NULL;
                    if (app_rb3_ws_open(&rb3, &c->ws) != ESP_OK) {
                        ESP_LOGE(TAG, "ws open failed");
                        c->phase = CHAT_PHASE_WAITING;
                        round_active = false;
                        break;
                    }
                }

                // start
                if (app_rb3_ws_send_start(c->ws, "r_chat", rb3.af) != ESP_OK) {
                    ESP_LOGE(TAG, "ws send start failed");
                    app_rb3_ws_close(c->ws);
                    c->ws = NULL;
                    c->phase = CHAT_PHASE_WAITING;
                    round_active = false;
                    break;
                }

                // 设置发送指针：从“当前时刻前 1.5s”开始，然后追到实时
                uint64_t seq_w = __atomic_load_n(&c->pre_seq_w, __ATOMIC_ACQUIRE);
                uint64_t min_seq = (seq_w > c->pre_cap) ? (seq_w - c->pre_cap) : 0;
                uint64_t target = (seq_w > c->pre_preroll_bytes) ? (seq_w - c->pre_preroll_bytes) : 0;
                if (target < min_seq) {
                    uint64_t lost = min_seq - target;
                    target = min_seq;
                    ESP_LOGW(TAG, "preroll 不足：被覆盖 %" PRIu64 " bytes，改为发送可用窗口", lost);
                }
                c->send_seq_r = target;
                ESP_LOGI(TAG, "上传: start -> preroll -> realtime, preroll_bytes=%" PRIu64,
                         (seq_w >= target) ? (seq_w - target) : 0);
            } else if (ev.type == CHAT_EVT_SPEAK_OFF) {
                if (c->phase == CHAT_PHASE_WAKE) {
                    // 注意：这里不立刻切回等待期。
                    // 若服务端有下行音频，则进入“播放期”，等播完再切回等待期（避免回声再次唤醒）。
                    if (c->ws && app_rb3_ws_is_connected(c->ws)) {
                        (void)app_rb3_ws_send_end(c->ws);
                        ESP_LOGI(TAG, "上传: end（保持WS连接）");

                        app_rb3_meta_t meta = {0};
                        bool got_audio = false;
                        dl_audio_ctx_t dl = {
                            .c = c,
                            .got_audio = &got_audio,
                        };
                        esp_err_t rxret = app_rb3_ws_recv_until_last(c->ws, &meta, on_audio_push_rb_track, &dl,
                                                                     should_abort_ws, &ab);
                        if (rxret == ESP_ERR_INVALID_STATE) {
                            ESP_LOGI(TAG, "ws recv cancelled");
                        } else if (rxret != ESP_OK) {
                            ESP_LOGE(TAG, "ws recv failed: %s", esp_err_to_name(rxret));
                            if (c->ws) {
                                app_rb3_ws_close(c->ws);
                                c->ws = NULL;
                            }
                        } else {
                            ESP_LOGI(TAG, "resp text=%s anim=%s motion=%s af=%s", meta.text, meta.anim, meta.motion,
                                     meta.af[0] ? meta.af : "(none)");
                        }

                        // 根据是否有下行音频，决定进入播放期还是直接回等待期
                        if (got_audio || is_playback_active(c)) {
                            ESP_LOGI(TAG, "状态切换: 唤醒期 -> 播放期（等待下行播完再回等待期）");
                            c->phase = CHAT_PHASE_PLAYBACK;
                        } else {
                            ESP_LOGI(TAG, "状态切换: 唤醒期 -> 等待期（无下行音频）");
                            c->phase = CHAT_PHASE_WAITING;
                        }
                    } else {
                        ESP_LOGI(TAG, "状态切换: 唤醒期 -> 等待期（WS 未连接）");
                        c->phase = CHAT_PHASE_WAITING;
                    }

                    round_active = false;
                }
            }
        }

        // 唤醒期：从 PSRAM 环形缓冲按 r_send 发送到 WS（带追帧/丢帧）
        if (c->phase == CHAT_PHASE_WAKE && round_active && c->ws && app_rb3_ws_is_connected(c->ws)) {
            if (should_abort_ws(&ab)) {
                round_active = false;
                continue;
            }

            uint64_t seq_w = __atomic_load_n(&c->pre_seq_w, __ATOMIC_ACQUIRE);
            uint64_t min_seq = (seq_w > c->pre_cap) ? (seq_w - c->pre_cap) : 0;
            if (c->send_seq_r < min_seq) {
                uint64_t drop = min_seq - c->send_seq_r;
                c->send_seq_r = min_seq;
                ESP_LOGW(TAG, "丢帧: 超出缓存窗口，跳过 %" PRIu64 " bytes", drop);
            }

            uint64_t backlog64 = (seq_w >= c->send_seq_r) ? (seq_w - c->send_seq_r) : 0;
            size_t backlog = (backlog64 > (uint64_t)SIZE_MAX) ? SIZE_MAX : (size_t)backlog64;

            if (backlog > max_backlog) {
                size_t drop = backlog - keep_backlog;
                c->send_seq_r += drop;
                uint32_t now = xTaskGetTickCount();
                if (now - c->last_catchup_log_tick > pdMS_TO_TICKS(1000)) {
                    c->last_catchup_log_tick = now;
                    ESP_LOGW(TAG, "追帧: backlog=%u bytes，快进丢弃=%u bytes（保留约1s）",
                             (unsigned)backlog, (unsigned)drop);
                }
                backlog = keep_backlog;
            }

            // 轻度节流：每轮最多发送 1~3 个 chunk
            int chunks = 1;
            if (backlog > c->bytes_per_sec) chunks = 3;
            else if (backlog > (c->bytes_per_sec / 2)) chunks = 2;

            for (int i = 0; i < chunks; ++i) {
                seq_w = __atomic_load_n(&c->pre_seq_w, __ATOMIC_ACQUIRE);
                if (c->send_seq_r >= seq_w) break;
                size_t n = (size_t)(seq_w - c->send_seq_r);
                if (n > (size_t)send_chunk) n = (size_t)send_chunk;
                prebuf_copy(c, c->send_seq_r, txbuf, n);
                esp_err_t sret = app_rb3_ws_send_bin(c->ws, txbuf, n, 2000);
                if (sret != ESP_OK) {
                    ESP_LOGW(TAG, "send failed: %s, back to WAITING and reconnect later", esp_err_to_name(sret));
                    app_rb3_ws_close(c->ws);
                    c->ws = NULL;
                    c->phase = CHAT_PHASE_WAITING;
                    round_active = false;
                    break;
                }
                c->send_seq_r += n;
                vTaskDelay(1);
            }
        } else {
            // 非唤醒态：检查等待期是否进入静默
            if (c->phase == CHAT_PHASE_WAITING) {
                TickType_t now = xTaskGetTickCount();
                uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(now - c->last_activity_tick);
                if (elapsed_ms >= idle_to_silent_ms) {
                    c->phase = CHAT_PHASE_SILENT;
                    ESP_LOGI(TAG, "状态切换: 等待期 -> 静默期（空闲>=60s，关闭WS）");
                    if (c->ws) {
                        app_rb3_ws_close(c->ws);
                        c->ws = NULL;
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
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
    // 之前 64KB 容易满（服务端下行音频一段会超过这个量），先增大到 256KB
    // 进一步增大到 512KB：降低下行灌入/播放争抢导致的 underrun
    c->rb_play = xRingbufferCreate(512 * 1024, RINGBUF_TYPE_BYTEBUF);
    ESP_RETURN_ON_FALSE(c->rb_play, ESP_ERR_NO_MEM, TAG, "create rb_play failed");

    // 统一：PSRAM 环形缓冲始终循环存麦克风 PCM
    const int sr = (c->audio_cfg.sample_rate > 0) ? c->audio_cfg.sample_rate : 16000;
    const int ch = (c->audio_cfg.channels > 0) ? c->audio_cfg.channels : 1;
    const int bps = (c->audio_cfg.bits_per_sample > 0) ? c->audio_cfg.bits_per_sample : 16;
    const int bytes_per_sample = bps / 8;
    const size_t bytes_per_sec = (size_t)sr * (size_t)ch * (size_t)bytes_per_sample;
    c->bytes_per_sec = bytes_per_sec;
    c->pre_cap = bytes_per_sec * 5; // 5s 历史缓存
    c->pre_preroll_bytes = (bytes_per_sec * 1500) / 1000; // 1.5s
    c->pre_rb = (uint8_t *)heap_caps_malloc(c->pre_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c->pre_rb) {
        ESP_LOGW(TAG, "PSRAM alloc prebuf failed, fallback to internal heap (%u bytes)", (unsigned)c->pre_cap);
        c->pre_rb = (uint8_t *)malloc(c->pre_cap);
    }
    ESP_RETURN_ON_FALSE(c->pre_rb, ESP_ERR_NO_MEM, TAG, "alloc prebuf failed");
    c->phase = CHAT_PHASE_WAITING;
    c->pre_seq_w = 0;
    c->send_seq_r = 0;
    c->last_catchup_log_tick = 0;

    // 播放预缓冲：默认至少 0.5s 才开始播（降低首句延迟）
    c->play_prefill_bytes = (uint32_t)(bytes_per_sec / 2);
    c->play_bytes_in = 0;
    // 播放背压水位：放宽一点，减少“灌入被频繁暂停”造成的断续
    // 高水位 8s、低水位 4s（16k/16bit/mono 下约 256KB / 128KB）
    c->play_high_wm_bytes = (uint32_t)(bytes_per_sec * 8);
    c->play_low_wm_bytes = (uint32_t)(bytes_per_sec * 4);

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

