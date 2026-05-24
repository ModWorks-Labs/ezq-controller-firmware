#include "button_input.h"

#include <array>

#include "dev_config.h"
#include "driver/gpio.h"
#include "pinout.h"

namespace button_input {
namespace {

constexpr uint32_t kDebounceMs = 40;
constexpr uint32_t kHoldFeedbackMs = 1500;
constexpr uint32_t kLongPressMs = 3000;

struct State {
  bool initialized = false;
  bool raw_pressed = false;
  bool stable_pressed = false;
  bool hold_started = false;
  bool long_sent = false;
  bool wait_for_release = false;
  uint32_t ignore_until_ms = 0;
  uint32_t raw_change_ms = 0;
  uint32_t press_started_ms = 0;
  std::array<Event, 4> queue = {};
  std::size_t queue_count = 0;
};

State g_state;

bool read_button_level() {
  return gpio_get_level(static_cast<gpio_num_t>(pinout::kButtonInputPin)) == 0;
}

void push_event(EventType type) {
  if (g_state.queue_count < g_state.queue.size()) {
    g_state.queue[g_state.queue_count++] = {type};
  }
}

bool pop_event(Event &event) {
  if (g_state.queue_count == 0) {
    return false;
  }

  event = g_state.queue[0];
  for (std::size_t i = 1; i < g_state.queue_count; ++i) {
    g_state.queue[i - 1] = g_state.queue[i];
  }
  --g_state.queue_count;
  return true;
}

}  // namespace

bool init() {
  if (g_state.initialized) {
    return true;
  }

  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << pinout::kButtonInputPin;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&io) != ESP_OK) {
    return false;
  }

  g_state.raw_pressed = read_button_level();
  g_state.stable_pressed = g_state.raw_pressed;
  g_state.initialized = true;
  return true;
}

void reset_after_wake(uint32_t now_ms) {
  g_state.raw_pressed = read_button_level();
  g_state.stable_pressed = g_state.raw_pressed;
  g_state.hold_started = false;
  g_state.long_sent = false;
  g_state.wait_for_release = true;
  g_state.ignore_until_ms = now_ms + dev_config::kWakeButtonSettleMs;
  g_state.press_started_ms = now_ms;
  g_state.queue_count = 0;
}

bool is_pressed() {
  return g_state.stable_pressed;
}

bool poll(uint32_t now_ms, Event &event) {
  if (pop_event(event)) {
    return true;
  }

  const bool pressed = read_button_level();
  if (pressed != g_state.raw_pressed) {
    g_state.raw_pressed = pressed;
    g_state.raw_change_ms = now_ms;
  }

  if (g_state.wait_for_release) {
    if (!pressed) {
      g_state.wait_for_release = false;
      g_state.raw_pressed = false;
      g_state.stable_pressed = false;
      g_state.raw_change_ms = now_ms;
      g_state.press_started_ms = now_ms;
    }
    return false;
  }

  if (now_ms < g_state.ignore_until_ms) {
    return false;
  }

  if (g_state.raw_pressed != g_state.stable_pressed &&
      (now_ms - g_state.raw_change_ms) >= kDebounceMs) {
    g_state.stable_pressed = g_state.raw_pressed;
    if (g_state.stable_pressed) {
      g_state.press_started_ms = now_ms;
      g_state.hold_started = false;
      g_state.long_sent = false;
      push_event(EventType::BUTTON_PRESSED);
    } else {
      push_event(EventType::BUTTON_RELEASED);
      if (!g_state.hold_started) {
        push_event(EventType::BUTTON_SHORT_PRESS);
      }
      g_state.hold_started = false;
      g_state.long_sent = false;
    }
  }

  if (g_state.stable_pressed) {
    const uint32_t held_ms = now_ms - g_state.press_started_ms;
    if (!g_state.hold_started && held_ms >= kHoldFeedbackMs) {
      g_state.hold_started = true;
      push_event(EventType::BUTTON_HOLD_STARTED);
    }
    if (!g_state.long_sent && held_ms >= kLongPressMs) {
      g_state.long_sent = true;
      push_event(EventType::BUTTON_LONG_PRESS);
    }
  }

  return pop_event(event);
}

}  // namespace button_input
