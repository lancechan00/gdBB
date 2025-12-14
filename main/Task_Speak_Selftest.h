#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 录音 5 秒并回放（用于验证 MIC->I2S RX + SPK->I2S TX）
 */
esp_err_t task_speak_selftest_start(void);

#ifdef __cplusplus
}
#endif

