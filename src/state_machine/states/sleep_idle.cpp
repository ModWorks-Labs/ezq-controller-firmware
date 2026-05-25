#include "state_machine/state_handlers.h"

#include "button_input.h"
#include "debug_console.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "output_control.h"
#include "pinout.h"
#include "wifi_manager.h"

namespace state_machine {
namespace {

constexpr char kTag[] = "state_sleep";

void restore_button_pin_after_wake() {
  const auto button_pin = static_cast<gpio_num_t>(pinout::kButtonInputPin);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  gpio_wakeup_disable(button_pin);
  gpio_sleep_sel_dis(button_pin);
  gpio_reset_pin(button_pin);
  gpio_set_direction(button_pin, GPIO_MODE_INPUT);
  gpio_set_pull_mode(button_pin, GPIO_PULLUP_ONLY);
}

void enter(StateContext &) {
  DEV_LOGI(kTag, "\n--- SLEEP_IDLE ---\nEntering light sleep.");
  output_control::set_indicator(output_control::Indicator::OFF);
  output_control::prepare_for_light_sleep();
  wifi_manager::suspend_for_sleep();

  const auto button_pin = static_cast<gpio_num_t>(pinout::kButtonInputPin);
  gpio_sleep_set_direction(button_pin, GPIO_MODE_INPUT);
  gpio_sleep_set_pull_mode(button_pin, GPIO_PULLUP_ONLY);
  gpio_sleep_sel_en(button_pin);
}

void update(StateContext &context) {
  const auto button_pin = static_cast<gpio_num_t>(pinout::kButtonInputPin);
  gpio_wakeup_enable(button_pin, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_light_sleep_start();
  restore_button_pin_after_wake();
  const uint32_t wake_now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
  DEV_LOGI(kTag, "Wake cause: %d", static_cast<int>(esp_sleep_get_wakeup_cause()));
  wifi_manager::resume_after_sleep();
  output_control::restore_after_light_sleep();
  output_control::play_button_press();
  button_input::reset_after_wake(wake_now_ms);
  DEV_LOGI(kTag, "Wake detected. Returning to READY_IDLE.");
  context.machine.request_transition_at(StateId::READY_IDLE, wake_now_ms);
}

void exit(StateContext &) {}

}  // namespace

const StateHandler kSleepIdleState = {
    "SLEEP_IDLE",
    enter,
    update,
    exit,
};

}  // namespace state_machine
