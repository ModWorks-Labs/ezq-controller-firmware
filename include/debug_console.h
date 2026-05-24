#pragma once

#include <cstdarg>

#include "esp_log.h"

namespace debug_console {

void init();
void log_message(esp_log_level_t level, const char *tag, const char *format, ...);
void log_message_v(esp_log_level_t level, const char *tag, const char *format, va_list args);

}  // namespace debug_console

#define DEV_LOGE(tag, format, ...) \
  ::debug_console::log_message(ESP_LOG_ERROR, tag, format, ##__VA_ARGS__)
#define DEV_LOGW(tag, format, ...) \
  ::debug_console::log_message(ESP_LOG_WARN, tag, format, ##__VA_ARGS__)
#define DEV_LOGI(tag, format, ...) \
  ::debug_console::log_message(ESP_LOG_INFO, tag, format, ##__VA_ARGS__)
#define DEV_LOGD(tag, format, ...) \
  ::debug_console::log_message(ESP_LOG_DEBUG, tag, format, ##__VA_ARGS__)

#define DEV_PRINTF(format, ...) \
  ::debug_console::log_message(ESP_LOG_INFO, "PRINTF", format, ##__VA_ARGS__)
