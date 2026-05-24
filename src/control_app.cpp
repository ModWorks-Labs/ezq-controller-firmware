#include "control_runtime.h"

#include "button_input.h"
#include "config_store.h"
#include "debug_console.h"
#include "output_control.h"
#include "sensor_monitor.h"
#include "state_machine/state_machine_types.h"

#include "dev_config.h"
#include "esp_timer.h"

namespace control_app {
namespace {

constexpr char kTag[] = "control_app";
constexpr uint64_t kTickPeriodUs = 10000;

state_machine::Machine g_machine;
bool g_initialized = false;
bool g_fault_monitor_armed = false;
uint64_t g_last_tick_us = 0;

void post_button_events(uint32_t now_ms) {
  button_input::Event event = {};
  while (button_input::poll(now_ms, event)) {
    switch (event.type) {
      case button_input::EventType::BUTTON_PRESSED:
        g_machine.post_event(state_machine::EventType::BUTTON_PRESSED);
        break;
      case button_input::EventType::BUTTON_RELEASED:
        g_machine.post_event(state_machine::EventType::BUTTON_RELEASED);
        break;
      case button_input::EventType::BUTTON_SHORT_PRESS:
        g_machine.post_event(state_machine::EventType::BUTTON_SHORT_PRESS);
        break;
      case button_input::EventType::BUTTON_HOLD_STARTED:
        g_machine.post_event(state_machine::EventType::BUTTON_HOLD_STARTED);
        break;
      case button_input::EventType::BUTTON_LONG_PRESS:
        g_machine.post_event(state_machine::EventType::BUTTON_LONG_PRESS);
        break;
      case button_input::EventType::NONE:
      default:
        break;
    }
  }
}

}  // namespace

void init() {
  if (g_initialized) {
    return;
  }

  g_machine.init();
  g_last_tick_us = 0;
  g_fault_monitor_armed = false;
  g_initialized = true;
}

void tick() {
  if (!g_initialized) {
    return;
  }

  const uint64_t now_us = esp_timer_get_time();
  const uint32_t now_ms = static_cast<uint32_t>(now_us / 1000ULL);
  output_control::tick(now_ms);
  post_button_events(now_ms);

  if (g_last_tick_us != 0 && (now_us - g_last_tick_us) < kTickPeriodUs) {
    return;
  }
  g_last_tick_us = now_us;

  sensor_monitor::update(now_ms);

  if (!config_store::init()) {
    g_machine.raise_fault(ControlFaultKind::INIT_FAILURE);
    const auto snapshot = sensor_monitor::snapshot();
    g_machine.tick(now_ms, config_store::settings(), config_store::active_profile(), snapshot);
    return;
  }

  g_machine.tick(now_ms,
                 config_store::settings(),
                 config_store::active_profile(),
                 sensor_monitor::snapshot());

  if (!g_fault_monitor_armed &&
      g_machine.current_state == state_machine::StateId::READY_IDLE &&
      (now_ms - g_machine.state_entry_ms) >= dev_config::kFaultMonitorArmDelayMs) {
    g_fault_monitor_armed = true;
    DEV_LOGI(kTag, "Fault monitoring armed after READY_IDLE stabilization");
  }

  if (g_fault_monitor_armed && sensor_monitor::initialized()) {
    const auto fault = sensor_monitor::evaluate_fault(config_store::settings());
    if (fault != ControlFaultKind::NONE) {
      g_machine.raise_fault(fault);
    }
  }
}

bool initialized() {
  return g_initialized;
}

ControlRuntimeStatus get_status() {
  if (!g_initialized) {
    return {
        .state = ControlStateId::BOOT,
        .fault_kind = ControlFaultKind::NONE,
        .state_name = "DEV_HOLD",
        .detail_name = "",
        .elapsed_ms = 0,
        .total_ms = 0,
        .has_total_ms = false,
        .can_start_cycle = false,
        .can_start_blower = false,
        .can_abort_cycle = false,
        .can_sleep = false,
        .glow_plug_active = false,
        .fan_active = false,
        .fault_latched = false,
        .temp_valid = false,
        .battery_valid = false,
        .temp_c = 0.0f,
        .battery_v = 0.0f,
    };
  }

  auto status = g_machine.runtime_status();
  const auto sensors = sensor_monitor::snapshot();
  status.temp_valid = sensors.temp_valid;
  status.battery_valid = sensors.battery_valid;
  status.temp_c = sensors.temp_c;
  status.battery_v = sensors.battery_v;
  status.elapsed_ms =
      static_cast<uint32_t>(esp_timer_get_time() / 1000ULL) - g_machine.state_entry_ms;
  switch (g_machine.ignition_phase) {
    case state_machine::IgnitionPhase::COUNTDOWN:
      status.detail_name = "countdown";
      break;
    case state_machine::IgnitionPhase::ACTIVE:
      status.detail_name = "active";
      break;
    case state_machine::IgnitionPhase::POST_CYCLE_HOLD:
      status.detail_name = "post_cycle_hold";
      break;
  }
  if (status.state != ControlStateId::IGNITION_CYCLE) {
    status.detail_name = "";
  }
  return status;
}

}  // namespace control_app
