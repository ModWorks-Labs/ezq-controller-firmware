#include "control_runtime.h"

#include "button_input.h"
#include "config_store.h"
#include "debug_console.h"
#include "output_control.h"
#include "ota_service.h"
#include "sensor_monitor.h"
#include "state_machine/state_machine_types.h"
#include "update_manager.h"

#include "dev_config.h"
#include "esp_timer.h"
#include "freertos/portmacro.h"

namespace control_app {
namespace {

constexpr char kTag[] = "control_app";
constexpr uint64_t kTickPeriodUs = 10000;

state_machine::Machine g_machine;
bool g_initialized = false;
bool g_fault_monitor_armed = false;
uint64_t g_last_tick_us = 0;
portMUX_TYPE g_command_lock = portMUX_INITIALIZER_UNLOCKED;

enum class PendingCommand : uint32_t {
  NONE = 0,
  START_CYCLE = 1U << 0,
  TOGGLE_BLOWER = 1U << 1,
  ABORT = 1U << 2,
};

uint32_t g_pending_commands = 0;

uint32_t take_pending_commands() {
  portENTER_CRITICAL(&g_command_lock);
  const uint32_t commands = g_pending_commands;
  g_pending_commands = 0;
  portEXIT_CRITICAL(&g_command_lock);
  return commands;
}

void queue_pending_command(PendingCommand command) {
  portENTER_CRITICAL(&g_command_lock);
  g_pending_commands |= static_cast<uint32_t>(command);
  portEXIT_CRITICAL(&g_command_lock);
}

void post_remote_commands() {
  const uint32_t commands = take_pending_commands();
  if (commands == 0) {
    return;
  }

  if ((commands & static_cast<uint32_t>(PendingCommand::START_CYCLE)) != 0U) {
    g_machine.post_event(state_machine::EventType::BUTTON_LONG_PRESS);
  }
  if ((commands & static_cast<uint32_t>(PendingCommand::TOGGLE_BLOWER)) != 0U) {
    g_machine.post_event(state_machine::EventType::BUTTON_SHORT_PRESS);
  }
  if ((commands & static_cast<uint32_t>(PendingCommand::ABORT)) != 0U) {
    g_machine.post_event(state_machine::EventType::BUTTON_PRESSED);
  }
}

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
  post_remote_commands();

  if (g_last_tick_us != 0 && (now_us - g_last_tick_us) < kTickPeriodUs) {
    return;
  }
  g_last_tick_us = now_us;

  if (!config_store::init()) {
    g_machine.raise_fault(ControlFaultKind::INIT_FAILURE);
    const auto snapshot = sensor_monitor::snapshot();
    g_machine.tick(now_ms, config_store::settings(), config_store::active_profile(), snapshot);
    return;
  }

  sensor_monitor::update(now_ms, config_store::settings());

  g_machine.tick(now_ms,
                 config_store::settings(),
                 config_store::active_profile(),
                 sensor_monitor::snapshot());

  const auto runtime = g_machine.runtime_status();
  update_manager::tick_post_boot(runtime.state, runtime.fault_kind, now_ms);

  if (ota_service::maintenance_active()) {
    return;
  }

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

RemoteActionResult request_start_cycle() {
  if (!g_initialized) {
    return RemoteActionResult::NOT_INITIALIZED;
  }

  const auto status = get_status();
  if (status.state != ControlStateId::READY_IDLE || !status.can_start_cycle) {
    return RemoteActionResult::INVALID_STATE;
  }

  queue_pending_command(PendingCommand::START_CYCLE);
  return RemoteActionResult::ACCEPTED;
}

RemoteActionResult request_toggle_blower() {
  if (!g_initialized) {
    return RemoteActionResult::NOT_INITIALIZED;
  }

  const auto status = get_status();
  if (status.state != ControlStateId::READY_IDLE && status.state != ControlStateId::BLOWER_MODE) {
    return RemoteActionResult::INVALID_STATE;
  }

  queue_pending_command(PendingCommand::TOGGLE_BLOWER);
  return RemoteActionResult::ACCEPTED;
}

RemoteActionResult request_abort() {
  if (!g_initialized) {
    return RemoteActionResult::NOT_INITIALIZED;
  }

  const auto status = get_status();
  if (status.state != ControlStateId::IGNITION_CYCLE) {
    return RemoteActionResult::INVALID_STATE;
  }

  queue_pending_command(PendingCommand::ABORT);
  return RemoteActionResult::ACCEPTED;
}

}  // namespace control_app
