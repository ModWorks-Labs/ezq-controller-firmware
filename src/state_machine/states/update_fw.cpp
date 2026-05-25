#include "state_machine/state_handlers.h"

#include "debug_console.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "output_control.h"
#include "update_manager.h"

namespace state_machine {
namespace {

constexpr char kTag[] = "state_update_fw";

void enter(StateContext &context) {
  context.machine.update_attempted = false;
  output_control::set_indicator(output_control::Indicator::BOOT);
  update_manager::clear_async_state();
  DEV_LOGI(kTag, "\n--- UPDATE_FW ---");
}

void update(StateContext &context) {
  if (context.machine.update_attempted) {
    if (update_manager::async_state() == update_manager::AsyncState::RUNNING) {
      return;
    }

    const bool success = update_manager::async_apply_success();
    const auto status = update_manager::get_status();
    update_manager::clear_async_state();

    if (!success) {
      DEV_LOGW(kTag, "Cloud update failed: %s", status.last_message.c_str());
      context.machine.request_transition(StateId::READY_IDLE);
      return;
    }

    DEV_LOGI(kTag, "Cloud update applied successfully. Rebooting into new slot.");
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return;
  }
  context.machine.update_attempted = true;

  DEV_LOGI(kTag, "Starting asynchronous HTTPS OTA update.");
  if (!update_manager::start_apply_async()) {
    DEV_LOGW(kTag, "Failed to start asynchronous OTA worker.");
    context.machine.request_transition(StateId::READY_IDLE);
  }
}

void exit(StateContext &) {}

}  // namespace

const StateHandler kUpdateFwState = {
    "UPDATE_FW",
    enter,
    update,
    exit,
};

}  // namespace state_machine
