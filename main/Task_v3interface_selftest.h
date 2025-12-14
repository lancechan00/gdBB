#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief v3 接口自检：连网 -> 发 /v1/robot/event -> 播放返回音频
 */
esp_err_t task_v3interface_selftest_start(void);

#ifdef __cplusplus
}
#endif

