#include "settings_defaults.h"

namespace settings_defaults {
namespace {

const config_store::Settings kDefaults = {
    240000,
    15000,
    90.0f,
    85.0f,
    10.5f,
    9.6f,
    2,
    true,
    {'d', 'e', 'f', 'a', 'u', 'l', 't', '\0'},
};

}  // namespace

const config_store::Settings &settings() {
  return kDefaults;
}

}  // namespace settings_defaults
