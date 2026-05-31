#include "settings_defaults.h"

namespace settings_defaults {
namespace {

const config_store::Settings kDefaults = {
    2,
    true,
    true,
    true,
    false,
    true,
    240000,
    100,
    true,
    65,
    10000,
    20000,
    5,
    100,
    20000,
    10,
    true,
    50,
    180000,
    90.0f,
    85.0f,
    10.5f,
    9.6f,
    true,
    true,
    true,
    30000,
    3000,
    1000,
    540000,
    15000,
    450,
};

}  // namespace

const config_store::Settings &settings() {
  return kDefaults;
}

}  // namespace settings_defaults
