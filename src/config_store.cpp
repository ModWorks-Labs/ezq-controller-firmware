#include "config_store.h"

#include "debug_console.h"

namespace config_store {
namespace {

constexpr char kTag[] = "config_store";

Settings g_settings = {
    15000,
    30000,
    90.0f,
    8.5f,
    {'d', 'e', 'f', 'a', 'u', 'l', 't', '\0'},
};

IgnitionProfile g_profile = {
    true,
    3,
    4,
    375000,
    {{
        {true, 5000, 0.0f},
        {false, 150000, 100.0f},
        {true, 2500, 0.0f},
    }},
    {{
        {false, 60000, 0.0f},
        {false, 90000, 5.0f},
        {true, 15000, 0.0f},
        {false, 210000, 100.0f},
    }},
};

bool g_initialized = false;

}  // namespace

bool init() {
  if (g_initialized) {
    return true;
  }

  // First-phase rebuild keeps control settings/profile in firmware memory.
  // Wi-Fi still uses NVS through the ESP-IDF stack; control config persistence is deferred.
  g_initialized = true;
  DEV_LOGI(kTag,
           "Using built-in control defaults: idle_sleep_ms=%lu abort_cooldown_ms=%lu profile=%s",
           static_cast<unsigned long>(g_settings.idle_sleep_timeout_ms),
           static_cast<unsigned long>(g_settings.abort_cooldown_ms),
           g_settings.active_profile_name.data());
  return true;
}

const Settings &settings() {
  return g_settings;
}

const IgnitionProfile &active_profile() {
  return g_profile;
}

}  // namespace config_store
