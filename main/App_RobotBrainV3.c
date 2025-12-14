#include "App_RobotBrainV3.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
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
        .af = "pcm_16k_16bit",
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
    int ulen = snprintf(url, sizeof(url), "%s%s", cfg->base_url, "/v1/robot/voice");
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

