#pragma once

#include <cstdint>

namespace button_input {

enum class EventType {
  NONE,
  BUTTON_PRESSED,
  BUTTON_RELEASED,
  BUTTON_SHORT_PRESS,
  BUTTON_HOLD_STARTED,
  BUTTON_LONG_PRESS,
};

struct Event {
  EventType type;
};

bool init();
void reset_after_wake(uint32_t now_ms);
bool is_pressed();
bool poll(uint32_t now_ms, Event &event);

}  // namespace button_input
