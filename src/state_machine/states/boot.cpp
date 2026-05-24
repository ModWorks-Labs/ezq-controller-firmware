#include "state_machine/state_handlers.h"

#include "button_input.h"
#include "config_store.h"
#include "debug_console.h"
#include "output_control.h"
#include "sensor_monitor.h"

namespace state_machine {
namespace {

constexpr char kTag[] = "state_boot";

void enter(StateContext &) {
  output_control::set_indicator(output_control::Indicator::BOOT);
  DEV_LOGI(kTag, "\n--- BOOT ---");
}

void update(StateContext &context) {
  if (context.machine.boot_attempted) {
    return;
  }
  context.machine.boot_attempted = true;

  DEV_LOGI(kTag, "Initializing control subsystems");
  const bool config_ok = config_store::init();
  DEV_LOGI(kTag, "config_store=%s", config_ok ? "ok" : "fail");
  const bool outputs_ok = output_control::init();
  DEV_LOGI(kTag, "output_control=%s", outputs_ok ? "ok" : "fail");
  const bool buttons_ok = button_input::init();
  DEV_LOGI(kTag, "button_input=%s", buttons_ok ? "ok" : "fail");
  const bool sensors_ok = sensor_monitor::init();
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
