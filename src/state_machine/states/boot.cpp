#include "state_machine/state_handlers.h"

#include "button_input.h"
#include "config_store.h"
#include "debug_console.h"
#include "dev_config.h"
#include "output_control.h"
#include "sensor_monitor.h"
#include "update_manager.h"
#include "wifi_manager.h"

namespace state_machine {
namespace {

constexpr char kTag[] = "state_boot";

void enter(StateContext &context) {
  context.machine.update_attempted = false;
  context.machine.boot_update_wait_started = false;
  context.machine.boot_update_wait_start_ms = 0;
  output_control::set_indicator(output_control::Indicator::BOOT);
  DEV_LOGI(kTag, "\n--- BOOT ---");
}

void update(StateContext &context) {
  if (!context.machine.boot_attempted) {
    context.machine.boot_attempted = true;

    DEV_LOGI(kTag, "Initializing control subsystems");
    const bool config_ok = config_store::init();
    DEV_LOGI(kTag, "config_store=%s", config_ok ? "ok" : "fail");
    const bool outputs_ok = output_control::init();
    DEV_LOGI(kTag, "output_control=%s", outputs_ok ? "ok" : "fail");
    const bool buttons_ok = button_input::init();
    DEV_LOGI(kTag, "button_input=%s", buttons_ok ? "ok" : "fail");
    const bool sensors_ok = sensor_monitor::init(config_store::settings());
    DEV_LOGI(kTag, "sensor_monitor=%s", sensors_ok ? "ok" : "fail");

    if (!config_ok || !outputs_ok || !buttons_ok) {
      DEV_LOGE(kTag, "Control init failed");
      context.machine.fault_kind = ControlFaultKind::INIT_FAILURE;
      context.machine.request_transition(StateId::FAULT);
      return;
    }

    if (!sensors_ok) {
      const auto sensors = sensor_monitor::snapshot();
      DEV_LOGE(kTag,
               "Sensor init failed: temp_present=%s temp_valid=%s battery_valid=%s temp=%.2f batt=%.2f",
               sensors.temp_sensor_present ? "yes" : "no",
               sensors.temp_valid ? "yes" : "no",
               sensors.battery_valid ? "yes" : "no",
               sensors.temp_c,
               sensors.battery_v);
      context.machine.fault_kind = ControlFaultKind::SENSOR_FAILURE;
      context.machine.request_transition(StateId::FAULT);
      return;
    }
  }

  const auto wifi = wifi_manager::get_status();
  if (!wifi.credentials_stored || wifi.mode == wifi_manager::WifiMode::PROVISION_AP) {
    DEV_LOGI(kTag, "Skipping cloud update check: Wi-Fi is not provisioned.");
    DEV_LOGI(kTag, "BOOT complete; entering READY_IDLE");
    output_control::play_startup_jingle();
    context.machine.request_transition(StateId::READY_IDLE);
    return;
  }

  if (!context.settings.automatic_firmware_updates_enabled) {
    DEV_LOGI(kTag, "Skipping cloud update check: automatic firmware updates are disabled.");
    DEV_LOGI(kTag, "BOOT complete; entering READY_IDLE");
    output_control::play_startup_jingle();
    context.machine.request_transition(StateId::READY_IDLE);
    return;
  }

  if (!context.machine.boot_update_wait_started) {
    context.machine.boot_update_wait_started = true;
    context.machine.boot_update_wait_start_ms = context.now_ms;
    DEV_LOGI(kTag, "Waiting up to %d ms for Wi-Fi before update check.",
             dev_config::kUpdateCheckWifiWaitMs);
  }

  if (!wifi.connected) {
    if ((context.now_ms - context.machine.boot_update_wait_start_ms) <
        static_cast<uint32_t>(dev_config::kUpdateCheckWifiWaitMs)) {
      return;
    }

    DEV_LOGW(kTag, "Skipping cloud update check: Wi-Fi did not connect in time.");
    DEV_LOGI(kTag, "BOOT complete; entering READY_IDLE");
    output_control::play_startup_jingle();
    context.machine.request_transition(StateId::READY_IDLE);
    return;
  }

  if (!context.machine.update_attempted) {
    context.machine.update_attempted = true;
    update_manager::clear_async_state();
    if (!update_manager::start_check_async()) {
      DEV_LOGW(kTag, "Failed to start asynchronous update check.");
      DEV_LOGI(kTag, "BOOT complete; entering READY_IDLE");
      output_control::play_startup_jingle();
      context.machine.request_transition(StateId::READY_IDLE);
      return;
    }
    DEV_LOGI(kTag, "Started asynchronous cloud update check.");
    return;
  }

  if (update_manager::async_state() == update_manager::AsyncState::RUNNING) {
    return;
  }

  const auto decision = update_manager::async_check_decision();
  const auto status = update_manager::get_status();
  update_manager::clear_async_state();

  if (decision == update_manager::CheckDecision::UPDATE_AVAILABLE) {
    DEV_LOGI(kTag,
             "Update available: current=%s target=%s selector=%s",
             status.current_version.c_str(),
             status.available_version.c_str(),
             status.target_selector.c_str());
    context.machine.request_transition(StateId::UPDATE_FW);
    return;
  }

  if (decision == update_manager::CheckDecision::ERROR) {
    DEV_LOGW(kTag, "Update check failed: %s", status.last_message.c_str());
  } else {
    DEV_LOGI(kTag, "No update required: %s", status.last_message.c_str());
  }

  DEV_LOGI(kTag, "BOOT complete; entering READY_IDLE");
  output_control::play_startup_jingle();
  context.machine.request_transition(StateId::READY_IDLE);
}

void exit(StateContext &) {}

}  // namespace

const StateHandler kBootState = {
    "BOOT",
    enter,
    update,
    exit,
};

}  // namespace state_machine
