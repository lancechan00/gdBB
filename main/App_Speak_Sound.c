#include "App_Speak_Sound.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "bsp/esp-bsp.h"          // Waveshare BSP entry
#include "esp_codec_dev.h"        // device handle + read/write

static const char *TAG = "App_Speak_Sound";

static esp_codec_dev_handle_t s_spk = NULL;
static esp_codec_dev_handle_t s_mic = NULL;
static app_speak_sound_cfg_t s_cfg = {
    .sample_rate = 16000,
    .channels = 1,
    .bits_per_sample = 16,
    .volume = 80,
    // 经验值：不少板子默认增益偏小，先给一个更“容易听到”的值；可后续放到 menuconfig 调
    .mic_gain_db = 36,
};

static esp_codec_dev_sample_info_t to_sample_info(const app_speak_sound_cfg_t *cfg)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = cfg->sample_rate,
        .channel = cfg->channels,
        .bits_per_sample = cfg->bits_per_sample,
        .channel_mask = 0,
        .mclk_multiple = 256,
    };
    return fs;
}

void app_speak_sound_get_cfg(app_speak_sound_cfg_t *out_cfg)
{
    if (out_cfg) {
        *out_cfg = s_cfg;
    }
}

esp_err_t app_speak_sound_init(const app_speak_sound_cfg_t *cfg)
{
    if (cfg) {
        s_cfg = *cfg;
    }

    // 关键：Waveshare BSP 的 bsp_audio_codec_*_init() 只有在 i2s_data_if==NULL 时才会调用 bsp_i2c_init()
    // 我们这里先调用 bsp_audio_init() 会导致 i2s_data_if 非空，从而跳过 I2C 初始化，最终 speaker_init 里 i2c_handle 为 NULL 并触发 assert。
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init failed");

    // I2S config: 使用 BSP pins，参数尽量与录放一致
    i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)s_cfg.sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            (s_cfg.bits_per_sample == 16) ? I2S_DATA_BIT_WIDTH_16BIT : I2S_DATA_BIT_WIDTH_16BIT,
            (s_cfg.channels == 2) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    // 常见 ES8311 MCLK=Fs*256
    i2s_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(bsp_audio_init(&i2s_cfg), TAG, "bsp_audio_init failed");

    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(s_spk && s_mic, ESP_FAIL, TAG, "bsp audio codec init failed");

    esp_codec_dev_sample_info_t fs = to_sample_info(&s_cfg);

    // speaker
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_spk, s_cfg.volume), TAG, "set spk vol failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_mute(s_spk, false), TAG, "set spk mute failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_spk, &fs), TAG, "open spk failed");

    // mic
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_mic, &fs), TAG, "open mic failed");
    // 说明：不同 codec 对 gain 的单位/范围不完全一致，这里 best-effort。
    (void)esp_codec_dev_set_in_gain(s_mic, s_cfg.mic_gain_db);

    ESP_LOGI(TAG, "audio inited: %d Hz, ch=%d, bits=%d", s_cfg.sample_rate, s_cfg.channels, s_cfg.bits_per_sample);
    return ESP_OK;
}

esp_err_t app_speak_sound_play_tone(int freq_hz, int duration_ms)
{
    ESP_RETURN_ON_FALSE(s_spk, ESP_ERR_INVALID_STATE, TAG, "speaker not init");
    ESP_RETURN_ON_FALSE(freq_hz > 0 && duration_ms > 0, ESP_ERR_INVALID_ARG, TAG, "bad args");

    const int sr = s_cfg.sample_rate;
    const int ch = s_cfg.channels;
    const int16_t amp = 12000;

    const int samples_total = (sr * duration_ms) / 1000;
    const int chunk_samples = 512; // 每次写 512 帧

    int16_t *buf = (int16_t *)heap_caps_malloc((size_t)chunk_samples * (size_t)ch * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "tone buf alloc failed");

    int sent = 0;
    while (sent < samples_total) {
        int n = chunk_samples;
        if (sent + n > samples_total) n = samples_total - sent;

        for (int i = 0; i < n; ++i) {
            float t = (float)(sent + i) / (float)sr;
            float s = sinf(2.0f * (float)M_PI * (float)freq_hz * t);
            int16_t v = (int16_t)((float)amp * s);
            if (ch == 2) {
                buf[i * 2] = v;
                buf[i * 2 + 1] = v;
            } else {
                buf[i] = v;
            }
        }

        size_t bytes = (size_t)n * (size_t)ch * sizeof(int16_t);
        esp_err_t err = esp_codec_dev_write(s_spk, (const uint8_t *)buf, bytes);
        if (err != ESP_OK) {
            heap_caps_free(buf);
            return err;
        }
        sent += n;
    }

    heap_caps_free(buf);
    return ESP_OK;
}

esp_err_t app_speak_sound_record(void *buf, size_t buf_bytes, size_t *out_bytes, int duration_ms)
{
    ESP_RETURN_ON_FALSE(s_mic, ESP_ERR_INVALID_STATE, TAG, "mic not init");
    ESP_RETURN_ON_FALSE(buf && buf_bytes > 0 && duration_ms > 0, ESP_ERR_INVALID_ARG, TAG, "bad args");

    const uint32_t start = esp_log_timestamp();
    size_t got = 0;

    while (got < buf_bytes) {
        uint32_t now = esp_log_timestamp();
        if ((int)(now - start) >= duration_ms) break;

        size_t want = 1024;
        if (want > (buf_bytes - got)) want = buf_bytes - got;

        esp_err_t err = esp_codec_dev_read(s_mic, (uint8_t *)buf + got, want);
        if (err != ESP_OK) {
            return err;
        }
        got += want;

        // 防止占用过高
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (out_bytes) *out_bytes = got;
    return ESP_OK;
}

esp_err_t app_speak_sound_play_pcm(const void *buf, size_t bytes)
{
    ESP_RETURN_ON_FALSE(s_spk, ESP_ERR_INVALID_STATE, TAG, "speaker not init");
    ESP_RETURN_ON_FALSE(buf && bytes > 0, ESP_ERR_INVALID_ARG, TAG, "bad args");

    const uint8_t *p = (const uint8_t *)buf;
    size_t left = bytes;
    while (left > 0) {
        size_t chunk = left > 2048 ? 2048 : left;
        esp_err_t err = esp_codec_dev_write(s_spk, p, chunk);
        if (err != ESP_OK) return err;
        p += chunk;
        left -= chunk;
    }
    return ESP_OK;
}
