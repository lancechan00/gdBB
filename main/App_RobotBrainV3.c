#include "App_RobotBrainV3.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"

static const char *TAG = "App_RobotBrainV3";

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} rb3_resp_buf_t;

static esp_err_t resp_buf_append(rb3_resp_buf_t *r, const char *data, int data_len)
{
    if (!r || !data || data_len <= 0) return ESP_OK;
    const size_t add = (size_t)data_len;
    const size_t max = 512 * 1024; // 自检足够，防止异常响应打爆内存
    if (r->cap == 0) {
        r->cap = 8192;
        r->buf = (char *)malloc(r->cap);
        r->len = 0;
        if (!r->buf) return ESP_ERR_NO_MEM;
    }
    if (r->len + add + 1 > r->cap) {
        size_t nc = r->cap;
        while (nc < r->len + add + 1) nc *= 2;
        if (nc > max) return ESP_ERR_NO_MEM;
        char *p = (char *)realloc(r->buf, nc);
        if (!p) return ESP_ERR_NO_MEM;
        r->buf = p;
        r->cap = nc;
    }
    memcpy(r->buf + r->len, data, add);
    r->len += add;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    rb3_resp_buf_t *rb = (rb3_resp_buf_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (evt->data && evt->data_len > 0) {
            return resp_buf_append(rb, (const char *)evt->data, evt->data_len);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void safe_copy(char *dst, size_t dst_sz, const char *src, size_t src_len)
{
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src || src_len == 0) return;
    size_t n = src_len;
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void json_extract_string(const char *json, const char *key, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!json || !key) return;

    const char *p = strstr(json, key);
    if (!p) return;
    p += strlen(key);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    if (*p != '"') return;
    p++;
    const char *start = p;
    while (*p && *p != '"') p++;
    if (*p != '"') return;
    safe_copy(out, out_sz, start, (size_t)(p - start));
}

static bool json_extract_bool_in_obj(const char *obj_start, const char *obj_end, const char *key)
{
    if (!obj_start || !obj_end || !key) return false;
    char *p = strstr(obj_start, key);
    if (!p || p >= obj_end) return false;
    p += strlen(key);
    while (p < obj_end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    if (p >= obj_end) return false;
    return (*p == 't' || *p == 'T'); // true
}

static char *find_json_object_end(char *start, char *limit)
{
    int brace = 0;
    for (char *p = start; p < limit; ++p) {
        if (*p == '{') brace++;
        else if (*p == '}') {
            brace--;
            if (brace == 0) return p;
        }
    }
    return NULL;
}

static esp_err_t parse_and_cb_audio_array(const char *json,
                                         size_t json_len,
                                         int chunk_bytes,
                                         app_rb3_on_audio_cb on_audio,
                                         void *cb_ctx)
{
    if (!json || json_len == 0 || !on_audio) return ESP_ERR_INVALID_ARG;

    // locate "audio":[ ... ]
    const char *pa = strstr(json, "\"audio\"");
    ESP_RETURN_ON_FALSE(pa, ESP_FAIL, TAG, "no audio field");
    pa += strlen("\"audio\"");
    while (*pa && (*pa == ' ' || *pa == '\t' || *pa == '\r' || *pa == '\n' || *pa == ':')) pa++;
    ESP_RETURN_ON_FALSE(*pa == '[', ESP_FAIL, TAG, "audio not array");

    char *buf = (char *)json; // reuse helpers require char*
    char *p = (char *)(pa + 1);
    char *limit = (char *)json + json_len;

    uint8_t *tmp = (uint8_t *)malloc((size_t)chunk_bytes + 1024); // base64 解码输出上限略大一点
    ESP_RETURN_ON_FALSE(tmp, ESP_ERR_NO_MEM, TAG, "tmp alloc failed");

    esp_err_t cb_ret = ESP_OK;
    while (p < limit) {
        while (p < limit && *p != '{' && *p != ']') p++;
        if (p >= limit || *p == ']') break;

        char *obj_start = p;
        char *obj_end = find_json_object_end(obj_start, limit);
        if (!obj_end) break;

        // type == "audio" ?
        bool is_audio = false;
        char *ptype = strstr(obj_start, "\"type\"");
        if (ptype && ptype < obj_end) {
            ptype += strlen("\"type\"");
            while (ptype < obj_end && (*ptype == ' ' || *ptype == '\t' || *ptype == '\r' || *ptype == '\n' || *ptype == ':')) ptype++;
            if (ptype < obj_end && *ptype == '"') {
                ptype++;
                if ((obj_end - ptype) >= 5 && strncmp(ptype, "audio", 5) == 0) {
                    is_audio = true;
                }
            }
        }

        if (is_audio) {
            bool is_last = json_extract_bool_in_obj(obj_start, obj_end, "\"is_last\"");

            // chunk base64
            char *pc = strstr(obj_start, "\"chunk\"");
            if (pc && pc < obj_end) {
                pc += strlen("\"chunk\"");
                while (pc < obj_end && (*pc == ' ' || *pc == '\t' || *pc == '\r' || *pc == '\n' || *pc == ':')) pc++;
                if (pc < obj_end && *pc == '"') {
                    pc++;
                    char *b64_start = pc;
                    while (pc < obj_end && *pc && *pc != '"') pc++;
                    if (pc < obj_end && *pc == '"') {
                        size_t b64_len = (size_t)(pc - b64_start);
                        size_t out_len = 0;
                        int mret = mbedtls_base64_decode(tmp, (size_t)chunk_bytes + 1024, &out_len,
                                                         (const unsigned char *)b64_start, b64_len);
                        if (mret == 0 && out_len > 0) {
                            cb_ret = on_audio(tmp, out_len, is_last, cb_ctx);
                            if (cb_ret != ESP_OK) {
                                break;
                            }
                        }
                    }
                }
            }
        }

        p = obj_end + 1;
        while (p < limit && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) p++;
    }

    free(tmp);
    (void)buf;
    return cb_ret;
}

app_rb3_cfg_t app_rb3_cfg_default(const char *base_url)
{
    app_rb3_cfg_t cfg = {
        .base_url = base_url,
        .event_path = "/v1/robot/event",
        // 自检建议用 pcm，避免端上引入 MP3 解码
        .af = "pcm16",
        .voice = "alloy",
        .model = "gpt-realtime-mini",
        .mode = "stream",
        .chunk_bytes = 500,
        .timeout_ms = 20000,
    };
    return cfg;
}

esp_err_t app_rb3_http_event_stream(const app_rb3_cfg_t *cfg,
                                   const char *event_name,
                                   const char *req_id,
                                   const char *user_id,
                                   app_rb3_meta_t *out_meta,
                                   app_rb3_on_audio_cb on_audio,
                                   void *cb_ctx)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->base_url && cfg->event_path, ESP_ERR_INVALID_ARG, TAG, "cfg invalid");
    ESP_RETURN_ON_FALSE(event_name && on_audio, ESP_ERR_INVALID_ARG, TAG, "arg invalid");

    // build url
    char url[256];
    int ulen = snprintf(url, sizeof(url), "%s%s", cfg->base_url, cfg->event_path);
    ESP_RETURN_ON_FALSE(ulen > 0 && ulen < (int)sizeof(url), ESP_ERR_INVALID_ARG, TAG, "url too long");

    // build json body
    char body[384];
    const char *rid = req_id ? req_id : "r001";
    const char *uid = user_id ? user_id : "demo";
    const char *af = cfg->af ? cfg->af : "pcm_16k_16bit";
    const char *mode = cfg->mode ? cfg->mode : "stream";
    const int chunk_bytes = cfg->chunk_bytes > 0 ? cfg->chunk_bytes : 500;

    int blen = snprintf(body, sizeof(body),
                        "{\"type\":\"event\",\"event\":\"%s\",\"req\":\"%s\",\"user_id\":\"%s\","
                        "\"chunk_bytes\":%d,\"mode\":\"%s\",\"af\":\"%s\"}",
                        event_name, rid, uid, chunk_bytes, mode, af);
    ESP_RETURN_ON_FALSE(blen > 0 && blen < (int)sizeof(body), ESP_ERR_INVALID_ARG, TAG, "body too long");

    rb3_resp_buf_t rb = {0};
    esp_http_client_config_t c = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 20000,
        .event_handler = http_evt,
        .user_data = &rb,
        .disable_auto_redirect = true,
        .transport_type = (strncmp(url, "https://", 8) == 0) ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    ESP_RETURN_ON_FALSE(h, ESP_FAIL, TAG, "http init failed");

    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_post_field(h, body, blen);

    esp_err_t ret = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);

    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "http perform failed");
    ESP_RETURN_ON_FALSE(status >= 200 && status < 300, ESP_FAIL, TAG, "http status=%d", status);
    ESP_RETURN_ON_FALSE(rb.buf && rb.len > 0, ESP_FAIL, TAG, "empty body");

    // fill meta (best-effort)
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
        json_extract_string(rb.buf, "\"req\"", out_meta->req, sizeof(out_meta->req));
        json_extract_string(rb.buf, "\"rid\"", out_meta->rid, sizeof(out_meta->rid));
        json_extract_string(rb.buf, "\"text\"", out_meta->text, sizeof(out_meta->text));
        // meta 子对象里可能有 anim/motion/af（这里简单全局找，够用）
        json_extract_string(rb.buf, "\"anim\"", out_meta->anim, sizeof(out_meta->anim));
        json_extract_string(rb.buf, "\"motion\"", out_meta->motion, sizeof(out_meta->motion));
        json_extract_string(rb.buf, "\"af\"", out_meta->af, sizeof(out_meta->af));
    }

    esp_err_t cb_ret = parse_and_cb_audio_array(rb.buf, rb.len, chunk_bytes, on_audio, cb_ctx);
    free(rb.buf);
    return cb_ret;
}

static bool starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static esp_err_t build_ws_url(const char *base_url, char *out, size_t out_sz)
{
    if (!base_url || !out || out_sz == 0) return ESP_ERR_INVALID_ARG;
    const char *path = "/v1/robot/voice_rt";
    if (starts_with(base_url, "http://")) {
        const char *host = base_url + 7;
        // 若未显式提供端口，默认用 8443（避免误连到 8000/80）
        const char *slash = strchr(host, '/');
        const char *port = strchr(host, ':');
        if (!port || (slash && port > slash)) {
            char host_only[192];
            size_t ncopy = slash ? (size_t)(slash - host) : strlen(host);
            if (ncopy >= sizeof(host_only)) ncopy = sizeof(host_only) - 1;
            memcpy(host_only, host, ncopy);
            host_only[ncopy] = '\0';
            int n = snprintf(out, out_sz, "ws://%s:8443%s", host_only, path);
            return (n > 0 && (size_t)n < out_sz) ? ESP_OK : ESP_ERR_INVALID_SIZE;
        }
        int n = snprintf(out, out_sz, "ws://%s%s", host, path);
        return (n > 0 && (size_t)n < out_sz) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    if (starts_with(base_url, "https://")) {
        const char *host = base_url + 8;
        const char *slash = strchr(host, '/');
        const char *port = strchr(host, ':');
        if (!port || (slash && port > slash)) {
            char host_only[192];
            size_t ncopy = slash ? (size_t)(slash - host) : strlen(host);
            if (ncopy >= sizeof(host_only)) ncopy = sizeof(host_only) - 1;
            memcpy(host_only, host, ncopy);
            host_only[ncopy] = '\0';
            int n = snprintf(out, out_sz, "wss://%s:8443%s", host_only, path);
            return (n > 0 && (size_t)n < out_sz) ? ESP_OK : ESP_ERR_INVALID_SIZE;
        }
        int n = snprintf(out, out_sz, "wss://%s%s", host, path);
        return (n > 0 && (size_t)n < out_sz) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    // 兼容用户直接传 ws/wss：若未显式提供端口，则默认 8443
    if (starts_with(base_url, "ws://") || starts_with(base_url, "wss://")) {
        const char *host = starts_with(base_url, "ws://") ? (base_url + 5) : (base_url + 6);
        const char *slash = strchr(host, '/');
        const char *port = strchr(host, ':');
        if (!port || (slash && port > slash)) {
            char host_only[192];
            size_t ncopy = slash ? (size_t)(slash - host) : strlen(host);
            if (ncopy >= sizeof(host_only)) ncopy = sizeof(host_only) - 1;
            memcpy(host_only, host, ncopy);
            host_only[ncopy] = '\0';
            if (starts_with(base_url, "ws://")) {
                int n = snprintf(out, out_sz, "ws://%s:8443%s", host_only, path);
                return (n > 0 && (size_t)n < out_sz) ? ESP_OK : ESP_ERR_INVALID_SIZE;
            }
            int n = snprintf(out, out_sz, "wss://%s:8443%s", host_only, path);
            return (n > 0 && (size_t)n < out_sz) ? ESP_OK : ESP_ERR_INVALID_SIZE;
        }
        int n = snprintf(out, out_sz, "%s%s", base_url, path);
        return (n > 0 && (size_t)n < out_sz) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    return ESP_ERR_INVALID_ARG;
}

static void json_extract_string_inplace(const char *json, const char *key, char *out, size_t out_sz)
{
    // 复用旧实现（key 需包含引号，例如 "\"type\""）
    json_extract_string(json, key, out, out_sz);
}

static bool json_extract_bool(const char *json, const char *key)
{
    // 简化版：全局查找 key，然后读 true/false
    if (!json || !key) return false;
    const char *p = strstr(json, key);
    if (!p) return false;
    p += strlen(key);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    return (*p == 't' || *p == 'T');
}

static esp_err_t json_extract_b64_chunk(const char *json, const char *key, const char **out_b64, size_t *out_len)
{
    if (!json || !key || !out_b64 || !out_len) return ESP_ERR_INVALID_ARG;
    *out_b64 = NULL;
    *out_len = 0;
    const char *p = strstr(json, key);
    if (!p) return ESP_FAIL;
    p += strlen(key);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    if (*p != '"') return ESP_FAIL;
    p++;
    const char *start = p;
    while (*p && *p != '"') p++;
    if (*p != '"') return ESP_FAIL;
    *out_b64 = start;
    *out_len = (size_t)(p - start);
    return ESP_OK;
}

typedef struct {
    QueueHandle_t q;      // item: char* (heap allocated, null-terminated)
    char *assem;          // assembling buffer
    int assem_len;        // expected total length
} ws_rx_ctx_t;

static void ws_rx_ctx_reset(ws_rx_ctx_t *r)
{
    if (!r) return;
    if (r->assem) {
        free(r->assem);
        r->assem = NULL;
    }
    r->assem_len = 0;
}

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    ws_rx_ctx_t *r = (ws_rx_ctx_t *)handler_args;
    if (!r) return;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "ws connected");
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "ws disconnected");
        ws_rx_ctx_reset(r);
        if (r->q) {
            char *nil = NULL;
            (void)xQueueSend(r->q, &nil, 0);
        }
        return;
    }
    if (event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGE(TAG, "ws error");
        ws_rx_ctx_reset(r);
        if (r->q) {
            char *nil = NULL;
            (void)xQueueSend(r->q, &nil, 0);
        }
        return;
    }

    if (event_id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
        if (!d || !d->data_ptr || d->data_len <= 0) return;

        // 组装完整 payload（esp_websocket_client 可能分片回调）
        int total = (d->payload_len > 0) ? d->payload_len : d->data_len;
        int offset = (d->payload_offset >= 0) ? d->payload_offset : 0;

        // 只处理文本消息（服务端音频也是 JSON + base64）
        // 若未来出现二进制下行，这里需要按 op_code 分支处理。
        if (offset == 0) {
            ws_rx_ctx_reset(r);
            r->assem = (char *)malloc((size_t)total + 1);
            if (!r->assem) return;
            r->assem_len = total;
        }
        if (!r->assem || r->assem_len <= 0) return;
        if (offset + d->data_len > r->assem_len) {
            // 异常分片，丢弃
            ws_rx_ctx_reset(r);
            return;
        }
        memcpy(r->assem + offset, d->data_ptr, (size_t)d->data_len);

        if (offset + d->data_len >= r->assem_len) {
            r->assem[r->assem_len] = '\0';
            char *msg = r->assem;
            r->assem = NULL;
            r->assem_len = 0;
            if (r->q) {
                if (xQueueSend(r->q, &msg, 0) != pdTRUE) {
                    free(msg);
                }
            } else {
                free(msg);
            }
        }
    }
}

typedef struct app_rb3_ws_sess_t {
    esp_websocket_client_handle_t client;
    ws_rx_ctx_t rx;
    uint8_t *tmp;
    size_t tmp_cap;
    app_rb3_cfg_t cfg; // 保存一份 cfg（指针字段由调用方保证生命周期）
} app_rb3_ws_sess_t;

static esp_err_t ws_wait_connected(esp_websocket_client_handle_t client,
                                   app_rb3_should_abort_cb should_abort,
                                   void *abort_ctx,
                                   int timeout_ms)
{
    uint32_t t0 = esp_log_timestamp();
    while (!esp_websocket_client_is_connected(client)) {
        if (should_abort && should_abort(abort_ctx)) return ESP_ERR_INVALID_STATE;
        if ((int)(esp_log_timestamp() - t0) > timeout_ms) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

esp_err_t app_rb3_ws_open(const app_rb3_cfg_t *cfg, app_rb3_ws_sess_t **out_sess)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->base_url && out_sess, ESP_ERR_INVALID_ARG, TAG, "arg invalid");
    *out_sess = NULL;

    char ws_url[256];
    ESP_RETURN_ON_ERROR(build_ws_url(cfg->base_url, ws_url, sizeof(ws_url)), TAG, "build ws url failed");

    esp_websocket_client_config_t wcfg = {
        .uri = ws_url,
        .buffer_size = 8192,
        .task_stack = 4096,
        .task_prio = 5,
        .reconnect_timeout_ms = 0,
        .network_timeout_ms = 10000,
        .disable_auto_reconnect = true,
    };

    app_rb3_ws_sess_t *s = (app_rb3_ws_sess_t *)calloc(1, sizeof(*s));
    ESP_RETURN_ON_FALSE(s, ESP_ERR_NO_MEM, TAG, "alloc sess failed");
    s->cfg = *cfg;

    s->client = esp_websocket_client_init(&wcfg);
    if (!s->client) {
        free(s);
        return ESP_FAIL;
    }

    s->rx.q = xQueueCreate(16, sizeof(char *));
    if (!s->rx.q) {
        esp_websocket_client_destroy(s->client);
        free(s);
        return ESP_ERR_NO_MEM;
    }
    s->rx.assem = NULL;
    s->rx.assem_len = 0;

    ESP_ERROR_CHECK(esp_websocket_register_events(s->client, WEBSOCKET_EVENT_ANY, ws_event_handler, &s->rx));
    esp_err_t ret = esp_websocket_client_start(s->client);
    if (ret != ESP_OK) {
        ws_rx_ctx_reset(&s->rx);
        vQueueDelete(s->rx.q);
        esp_websocket_client_destroy(s->client);
        free(s);
        return ret;
    }

    // 等待连接建立
    ret = ws_wait_connected(s->client, NULL, NULL, 5000);
    if (ret != ESP_OK) {
        app_rb3_ws_close(s);
        return ret;
    }

    *out_sess = s;
    return ESP_OK;
}

bool app_rb3_ws_is_connected(app_rb3_ws_sess_t *sess)
{
    if (!sess || !sess->client) return false;
    return esp_websocket_client_is_connected(sess->client);
}

void app_rb3_ws_close(app_rb3_ws_sess_t *sess)
{
    if (!sess) return;

    if (sess->client) {
        esp_websocket_client_stop(sess->client);
        esp_websocket_client_destroy(sess->client);
        sess->client = NULL;
    }

    // 清空队列中的残留消息
    if (sess->rx.q) {
        char *rx = NULL;
        while (xQueueReceive(sess->rx.q, &rx, 0) == pdTRUE) {
            if (rx) free(rx);
        }
        vQueueDelete(sess->rx.q);
        sess->rx.q = NULL;
    }
    ws_rx_ctx_reset(&sess->rx);

    if (sess->tmp) free(sess->tmp);
    sess->tmp = NULL;
    sess->tmp_cap = 0;

    free(sess);
}

esp_err_t app_rb3_ws_send_start(app_rb3_ws_sess_t *sess, const char *req_id, const char *audio_format)
{
    ESP_RETURN_ON_FALSE(sess && sess->client, ESP_ERR_INVALID_ARG, TAG, "sess invalid");
    ESP_RETURN_ON_FALSE(esp_websocket_client_is_connected(sess->client), ESP_ERR_INVALID_STATE, TAG, "ws not connected");

    const char *req = req_id; // 可为 NULL
    const char *af_out = sess->cfg.af ? sess->cfg.af : (audio_format ? audio_format : "pcm16");
    const char *voice = sess->cfg.voice ? sess->cfg.voice : "alloy";
    const char *model = sess->cfg.model ? sess->cfg.model : "gpt-realtime-mini";

    char start_msg[256];
    int slen = 0;
    if (req) {
        slen = snprintf(start_msg, sizeof(start_msg),
                        "{\"type\":\"start\",\"req\":\"%s\",\"af\":\"%s\",\"voice\":\"%s\",\"model\":\"%s\"}",
                        req, af_out, voice, model);
    } else {
        slen = snprintf(start_msg, sizeof(start_msg),
                        "{\"type\":\"start\",\"af\":\"%s\",\"voice\":\"%s\",\"model\":\"%s\"}",
                        af_out, voice, model);
    }
    ESP_RETURN_ON_FALSE(slen > 0 && slen < (int)sizeof(start_msg), ESP_ERR_INVALID_SIZE, TAG, "start msg too long");

    int wr = esp_websocket_client_send_text(sess->client, start_msg, slen, pdMS_TO_TICKS(2000));
    return (wr > 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t app_rb3_ws_send_bin(app_rb3_ws_sess_t *sess, const uint8_t *data, size_t len, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(sess && sess->client && data && len > 0, ESP_ERR_INVALID_ARG, TAG, "arg invalid");
    ESP_RETURN_ON_FALSE(esp_websocket_client_is_connected(sess->client), ESP_ERR_INVALID_STATE, TAG, "ws not connected");
    if (timeout_ms <= 0) timeout_ms = 2000;
    int wr = esp_websocket_client_send_bin(sess->client, (const char *)data, (int)len, pdMS_TO_TICKS(timeout_ms));
    return (wr > 0 && wr == (int)len) ? ESP_OK : ESP_FAIL;
}

esp_err_t app_rb3_ws_send_end(app_rb3_ws_sess_t *sess)
{
    ESP_RETURN_ON_FALSE(sess && sess->client, ESP_ERR_INVALID_ARG, TAG, "sess invalid");
    ESP_RETURN_ON_FALSE(esp_websocket_client_is_connected(sess->client), ESP_ERR_INVALID_STATE, TAG, "ws not connected");
    const char *end_msg = "{\"type\":\"end\"}";
    int wr = esp_websocket_client_send_text(sess->client, end_msg, (int)strlen(end_msg), pdMS_TO_TICKS(2000));
    return (wr > 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t app_rb3_ws_recv_until_last(app_rb3_ws_sess_t *sess,
                                     app_rb3_meta_t *out_meta,
                                     app_rb3_on_audio_cb on_audio,
                                     void *cb_ctx,
                                     app_rb3_should_abort_cb should_abort,
                                     void *abort_ctx)
{
    ESP_RETURN_ON_FALSE(sess && sess->client && sess->rx.q && on_audio, ESP_ERR_INVALID_ARG, TAG, "arg invalid");
    ESP_RETURN_ON_FALSE(esp_websocket_client_is_connected(sess->client), ESP_ERR_INVALID_STATE, TAG, "ws not connected");

    if (out_meta) memset(out_meta, 0, sizeof(*out_meta));
    size_t text_len = 0;
    bool got_last = false;

    while (!got_last) {
        if (should_abort && should_abort(abort_ctx)) return ESP_ERR_INVALID_STATE;

        char *rx = NULL;
        if (xQueueReceive(sess->rx.q, &rx, pdMS_TO_TICKS(3000)) != pdTRUE) {
            if (!esp_websocket_client_is_connected(sess->client)) return ESP_FAIL;
            continue;
        }
        if (rx == NULL) {
            return ESP_FAIL;
        }

        char type[16] = {0};
        json_extract_string_inplace(rx, "\"type\"", type, sizeof(type));
        if (strcmp(type, "meta") == 0) {
            if (out_meta) {
                json_extract_string_inplace(rx, "\"req\"", out_meta->req, sizeof(out_meta->req));
                json_extract_string_inplace(rx, "\"rid\"", out_meta->rid, sizeof(out_meta->rid));
                json_extract_string_inplace(rx, "\"anim\"", out_meta->anim, sizeof(out_meta->anim));
                json_extract_string_inplace(rx, "\"motion\"", out_meta->motion, sizeof(out_meta->motion));
                json_extract_string_inplace(rx, "\"af\"", out_meta->af, sizeof(out_meta->af));
            }
        } else if (strcmp(type, "asr_text") == 0) {
            if (out_meta) {
                json_extract_string_inplace(rx, "\"text\"", out_meta->text, sizeof(out_meta->text));
                text_len = strlen(out_meta->text);
            }
        } else if (strcmp(type, "text_delta") == 0) {
            if (out_meta) {
                char delta[128] = {0};
                json_extract_string_inplace(rx, "\"text\"", delta, sizeof(delta));
                if (delta[0]) {
                    size_t dlen = strlen(delta);
                    size_t cap = sizeof(out_meta->text);
                    if (text_len < cap - 1) {
                        size_t can = cap - 1 - text_len;
                        if (dlen > can) dlen = can;
                        memcpy(out_meta->text + text_len, delta, dlen);
                        text_len += dlen;
                        out_meta->text[text_len] = '\0';
                    }
                }
            }
        } else if (strcmp(type, "text") == 0) {
            if (out_meta) {
                json_extract_string_inplace(rx, "\"text\"", out_meta->text, sizeof(out_meta->text));
                text_len = strlen(out_meta->text);
            }
        } else if (strcmp(type, "audio") == 0) {
            bool is_last = json_extract_bool(rx, "\"is_last\"");
            const char *b64 = NULL;
            size_t b64_len = 0;
            if (json_extract_b64_chunk(rx, "\"chunk\"", &b64, &b64_len) == ESP_OK && b64 && b64_len > 0) {
                size_t need = (b64_len / 4) * 3 + 4;
                if (need > sess->tmp_cap) {
                    uint8_t *p = (uint8_t *)realloc(sess->tmp, need);
                    if (!p) {
                        free(rx);
                        return ESP_ERR_NO_MEM;
                    }
                    sess->tmp = p;
                    sess->tmp_cap = need;
                }
                size_t out_len = 0;
                int mret = mbedtls_base64_decode(sess->tmp, sess->tmp_cap, &out_len,
                                                 (const unsigned char *)b64, b64_len);
                if (mret == 0 && out_len > 0) {
                    esp_err_t cbret = on_audio(sess->tmp, out_len, is_last, cb_ctx);
                    if (cbret != ESP_OK) {
                        free(rx);
                        return cbret;
                    }
                }
            }
            if (is_last) got_last = true;
        }

        free(rx);
    }

    return ESP_OK;
}

esp_err_t app_rb3_http_voice_stream(const app_rb3_cfg_t *cfg,
                                   const uint8_t *pcm,
                                   size_t pcm_len,
                                   const char *audio_format,
                                   const char *language,
                                   const char *req_id,
                                   const char *user_id,
                                   app_rb3_meta_t *out_meta,
                                   app_rb3_on_audio_cb on_audio,
                                   void *cb_ctx)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->base_url, ESP_ERR_INVALID_ARG, TAG, "cfg invalid");
    ESP_RETURN_ON_FALSE(pcm && pcm_len > 0 && on_audio, ESP_ERR_INVALID_ARG, TAG, "arg invalid");

    // build url
    char url[256];
    int ulen = snprintf(url, sizeof(url), "%s%s", cfg->base_url, "/v1/robot/voice_rt");
    ESP_RETURN_ON_FALSE(ulen > 0 && ulen < (int)sizeof(url), ESP_ERR_INVALID_ARG, TAG, "url too long");

    const char *rid = req_id ? req_id : "r_voice";
    const char *uid = user_id ? user_id : "demo";
    const char *af_out = cfg->af ? cfg->af : "pcm_16k_16bit";
    const char *mode = cfg->mode ? cfg->mode : "stream";
    const int chunk_bytes = cfg->chunk_bytes > 0 ? cfg->chunk_bytes : 500;
    const char *af_in = audio_format ? audio_format : "pcm_16k_16bit";
    const char *lang = language ? language : "zh-CN";

    // base64 encode pcm
    size_t b64_need = 0;
    int b64ret = mbedtls_base64_encode(NULL, 0, &b64_need, pcm, pcm_len);
    // 预期会返回缓冲不足，但 b64_need 会给出所需长度
    (void)b64ret;
    ESP_RETURN_ON_FALSE(b64_need > 0 && b64_need < (2 * 1024 * 1024), ESP_FAIL, TAG, "b64 size invalid");

    char *b64 = (char *)malloc(b64_need + 1);
    ESP_RETURN_ON_FALSE(b64, ESP_ERR_NO_MEM, TAG, "alloc b64 failed");
    size_t b64_out = 0;
    b64ret = mbedtls_base64_encode((unsigned char *)b64, b64_need, &b64_out, pcm, pcm_len);
    if (b64ret != 0 || b64_out == 0) {
        free(b64);
        ESP_LOGE(TAG, "base64 encode failed: %d", b64ret);
        return ESP_FAIL;
    }
    b64[b64_out] = '\0';

    // build json body (dynamic)
    // {"type":"voice","audio_data":"...","audio_format":"...","language":"...","req":"...","user_id":"...","chunk_bytes":500,"mode":"stream","af":"pcm_16k_16bit"}
    const size_t body_cap = b64_out + 512;
    char *body = (char *)malloc(body_cap);
    if (!body) {
        free(b64);
        return ESP_ERR_NO_MEM;
    }
    int blen = snprintf(body, body_cap,
                        "{\"type\":\"voice\",\"audio_data\":\"%s\",\"audio_format\":\"%s\",\"language\":\"%s\","
                        "\"req\":\"%s\",\"user_id\":\"%s\",\"chunk_bytes\":%d,\"mode\":\"%s\",\"af\":\"%s\"}",
                        b64, af_in, lang, rid, uid, chunk_bytes, mode, af_out);
    free(b64);
    ESP_RETURN_ON_FALSE(blen > 0 && (size_t)blen < body_cap, ESP_ERR_INVALID_ARG, TAG, "body too long");

    rb3_resp_buf_t rb = {0};
    esp_http_client_config_t c = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 20000,
        .event_handler = http_evt,
        .user_data = &rb,
        .disable_auto_redirect = true,
        .transport_type = (strncmp(url, "https://", 8) == 0) ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) {
        free(body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_post_field(h, body, blen);

    esp_err_t ret = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    free(body);

    ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, "http perform failed");
    ESP_RETURN_ON_FALSE(status >= 200 && status < 300, ESP_FAIL, TAG, "http status=%d", status);
    ESP_RETURN_ON_FALSE(rb.buf && rb.len > 0, ESP_FAIL, TAG, "empty body");

    // fill meta (best-effort)
    if (out_meta) {
        memset(out_meta, 0, sizeof(*out_meta));
        json_extract_string(rb.buf, "\"req\"", out_meta->req, sizeof(out_meta->req));
        json_extract_string(rb.buf, "\"rid\"", out_meta->rid, sizeof(out_meta->rid));
        json_extract_string(rb.buf, "\"text\"", out_meta->text, sizeof(out_meta->text));
        json_extract_string(rb.buf, "\"anim\"", out_meta->anim, sizeof(out_meta->anim));
        json_extract_string(rb.buf, "\"motion\"", out_meta->motion, sizeof(out_meta->motion));
        json_extract_string(rb.buf, "\"af\"", out_meta->af, sizeof(out_meta->af));
    }

    esp_err_t cb_ret = parse_and_cb_audio_array(rb.buf, rb.len, chunk_bytes, on_audio, cb_ctx);
    free(rb.buf);
    return cb_ret;
}

esp_err_t app_rb3_ws_voice_stream(const app_rb3_cfg_t *cfg,
                                 const uint8_t *pcm,
                                 size_t pcm_len,
                                 int send_chunk_bytes,
                                 const char *audio_format,
                                 const char *language,
                                 const char *req_id,
                                 const char *user_id,
                                 app_rb3_meta_t *out_meta,
                                 app_rb3_on_audio_cb on_audio,
                                 void *cb_ctx,
                                 app_rb3_should_abort_cb should_abort,
                                 void *abort_ctx)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->base_url, ESP_ERR_INVALID_ARG, TAG, "cfg invalid");
    ESP_RETURN_ON_FALSE(pcm && pcm_len > 0 && on_audio, ESP_ERR_INVALID_ARG, TAG, "arg invalid");

    char ws_url[256];
    ESP_RETURN_ON_ERROR(build_ws_url(cfg->base_url, ws_url, sizeof(ws_url)), TAG, "build ws url failed");

    (void)language;
    (void)user_id;

    // voice_rt: start 参数（req/rid 可选；af/voice/model 可选）
    const char *req = req_id; // 可为 NULL，让服务端生成
    const char *rid = NULL;   // 目前端侧不生成 rid；如需可扩展参数

    // 输出音频格式（服务端当前实现以 pcm16 输出）
    const char *af_out = cfg->af ? cfg->af : (audio_format ? audio_format : "pcm16");
    const char *voice = cfg->voice ? cfg->voice : "alloy";
    const char *model = cfg->model ? cfg->model : "gpt-realtime-mini";

    const int snd_chunk = (send_chunk_bytes > 0) ? send_chunk_bytes : 4096;

    // start json（字段按你最新协议）
    char start_msg[256];
    int slen = 0;
    if (req) {
        slen = snprintf(start_msg, sizeof(start_msg),
                        "{\"type\":\"start\",\"req\":\"%s\",\"af\":\"%s\",\"voice\":\"%s\",\"model\":\"%s\"}",
                        req, af_out, voice, model);
    } else {
        slen = snprintf(start_msg, sizeof(start_msg),
                        "{\"type\":\"start\",\"af\":\"%s\",\"voice\":\"%s\",\"model\":\"%s\"}",
                        af_out, voice, model);
    }
    // rid 预留：若未来需要端侧指定 rid，可在这里补上
    (void)rid;
    ESP_RETURN_ON_FALSE(slen > 0 && slen < (int)sizeof(start_msg), ESP_ERR_INVALID_SIZE, TAG, "start msg too long");

    esp_websocket_client_config_t wcfg = {
        .uri = ws_url,
        .buffer_size = 8192,
        .task_stack = 4096,
        .task_prio = 5,
        .reconnect_timeout_ms = 0, // 我们自己控制生命周期
        .network_timeout_ms = 10000,
        .disable_auto_reconnect = true,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&wcfg);
    ESP_RETURN_ON_FALSE(client, ESP_FAIL, TAG, "ws init failed");

    ws_rx_ctx_t rxctx = {
        .q = xQueueCreate(8, sizeof(char *)),
        .assem = NULL,
        .assem_len = 0,
    };
    if (!rxctx.q) {
        esp_websocket_client_destroy(client);
        return ESP_ERR_NO_MEM;
    }
    // 收消息走事件回调，避免使用不存在的同步 recv API
    ESP_ERROR_CHECK(esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler, &rxctx));

    esp_err_t ret = esp_websocket_client_start(client);
    if (ret != ESP_OK) {
        vQueueDelete(rxctx.q);
        esp_websocket_client_destroy(client);
        return ret;
    }

    // 等待连接建立（简单轮询）
    uint32_t t0 = esp_log_timestamp();
    while (!esp_websocket_client_is_connected(client)) {
        if (should_abort && should_abort(abort_ctx)) {
            esp_websocket_client_stop(client);
            esp_websocket_client_destroy(client);
            return ESP_ERR_INVALID_STATE;
        }
        if ((int)(esp_log_timestamp() - t0) > 5000) {
            esp_websocket_client_stop(client);
            esp_websocket_client_destroy(client);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "ws send start: %s", start_msg);
    // send start (text)
    int wst = esp_websocket_client_send_text(client, start_msg, slen, pdMS_TO_TICKS(2000));
    if (wst <= 0) {
        ESP_LOGE(TAG, "ws send start failed, ret=%d", wst);
        ws_rx_ctx_reset(&rxctx);
        vQueueDelete(rxctx.q);
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        return ESP_FAIL;
    }

    // send audio (binary chunks)
    size_t off = 0;
    while (off < pcm_len) {
        if (should_abort && should_abort(abort_ctx)) {
            esp_websocket_client_stop(client);
            esp_websocket_client_destroy(client);
            return ESP_ERR_INVALID_STATE;
        }
        size_t n = pcm_len - off;
        if (n > (size_t)snd_chunk) n = (size_t)snd_chunk;
        int wr = esp_websocket_client_send_bin(client, (const char *)pcm + off, (int)n, pdMS_TO_TICKS(2000));
        if (wr <= 0 || wr != (int)n) {
            ESP_LOGE(TAG, "ws send bin failed, want=%d ret=%d off=%u/%u",
                     (int)n, wr, (unsigned)off, (unsigned)pcm_len);
            ws_rx_ctx_reset(&rxctx);
            vQueueDelete(rxctx.q);
            esp_websocket_client_stop(client);
            esp_websocket_client_destroy(client);
            return ESP_FAIL;
        }
        off += n;
        // 给网络栈一点调度机会，避免长时间占用导致写失败
        vTaskDelay(1);
    }

    // send end (text)
    const char *end_msg = "{\"type\":\"end\"}";
    int wse = esp_websocket_client_send_text(client, end_msg, (int)strlen(end_msg), pdMS_TO_TICKS(2000));
    if (wse <= 0) {
        ESP_LOGW(TAG, "ws send end failed, ret=%d", wse);
    }

    // recv loop：直到 audio is_last=true 或 abort
    if (out_meta) memset(out_meta, 0, sizeof(*out_meta));

    // base64 解码缓冲：按收到的 b64 长度动态扩容，避免依赖 chunk_bytes 假设
    uint8_t *tmp = NULL;
    size_t tmp_cap = 0;

    bool got_last = false;
    // 文本聚合：支持 text_delta（增量）和 text（整句）
    size_t text_len = 0;

    while (!got_last) {
        if (should_abort && should_abort(abort_ctx)) {
            free(tmp);
            ws_rx_ctx_reset(&rxctx);
            vQueueDelete(rxctx.q);
            esp_websocket_client_stop(client);
            esp_websocket_client_destroy(client);
            return ESP_ERR_INVALID_STATE;
        }

        char *rx = NULL;
        if (xQueueReceive(rxctx.q, &rx, pdMS_TO_TICKS(3000)) != pdTRUE) {
            if (!esp_websocket_client_is_connected(client)) break;
            continue;
        }
        if (rx == NULL) {
            // disconnected/error signal
            break;
        }

        // 仅处理 JSON 文本帧（服务端 audio 是 base64 字段）
        char type[16] = {0};
        json_extract_string_inplace(rx, "\"type\"", type, sizeof(type));
        if (strcmp(type, "meta") == 0) {
            if (out_meta) {
                json_extract_string_inplace(rx, "\"req\"", out_meta->req, sizeof(out_meta->req));
                json_extract_string_inplace(rx, "\"rid\"", out_meta->rid, sizeof(out_meta->rid));
                json_extract_string_inplace(rx, "\"anim\"", out_meta->anim, sizeof(out_meta->anim));
                json_extract_string_inplace(rx, "\"motion\"", out_meta->motion, sizeof(out_meta->motion));
                json_extract_string_inplace(rx, "\"af\"", out_meta->af, sizeof(out_meta->af));
            }
        } else if (strcmp(type, "asr_text") == 0) {
            if (out_meta) {
                json_extract_string_inplace(rx, "\"text\"", out_meta->text, sizeof(out_meta->text));
            }
        } else if (strcmp(type, "text_delta") == 0) {
            // 增量拼接到 out_meta->text
            if (out_meta) {
                char delta[128] = {0};
                json_extract_string_inplace(rx, "\"text\"", delta, sizeof(delta));
                if (delta[0]) {
                    size_t dlen = strlen(delta);
                    size_t cap = sizeof(out_meta->text);
                    if (text_len < cap - 1) {
                        size_t can = cap - 1 - text_len;
                        if (dlen > can) dlen = can;
                        memcpy(out_meta->text + text_len, delta, dlen);
                        text_len += dlen;
                        out_meta->text[text_len] = '\0';
                    }
                }
            }
        } else if (strcmp(type, "text") == 0) {
            // 完整文本
            if (out_meta) {
                json_extract_string_inplace(rx, "\"text\"", out_meta->text, sizeof(out_meta->text));
                text_len = strlen(out_meta->text);
            }
        } else if (strcmp(type, "audio") == 0) {
            bool is_last = json_extract_bool(rx, "\"is_last\"");
            const char *b64 = NULL;
            size_t b64_len = 0;
            if (json_extract_b64_chunk(rx, "\"chunk\"", &b64, &b64_len) == ESP_OK && b64 && b64_len > 0) {
                // base64 输出上限约为 b64_len*3/4
                size_t need = (b64_len / 4) * 3 + 4;
                if (need > tmp_cap) {
                    uint8_t *p = (uint8_t *)realloc(tmp, need);
                    if (!p) {
                        free(rx);
                        break;
                    }
                    tmp = p;
                    tmp_cap = need;
                }
                size_t out_len = 0;
                int mret = mbedtls_base64_decode(tmp, tmp_cap, &out_len,
                                                 (const unsigned char *)b64, b64_len);
                if (mret == 0 && out_len > 0) {
                    esp_err_t cbret = on_audio(tmp, out_len, is_last, cb_ctx);
                    if (cbret != ESP_OK) {
                        break;
                    }
                }
            }
            if (is_last) got_last = true;
        }
        free(rx);
    }

    free(tmp);
    ws_rx_ctx_reset(&rxctx);
    vQueueDelete(rxctx.q);
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    return got_last ? ESP_OK : ESP_FAIL;
}

