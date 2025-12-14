#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 声卡/扬声器自检：播放一段测试音
 */
esp_err_t task_sound_selftest_start(void);

#ifdef __cplusplus
}
#endif
