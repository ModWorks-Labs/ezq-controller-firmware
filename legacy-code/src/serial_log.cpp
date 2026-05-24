#include <Arduino.h>

#include "serial_log.h"

namespace {

constexpr int kSerialGatePin = 9;
constexpr unsigned long kSerialReleaseDelayMs = 2000;
constexpr size_t kSerialBufferReserve = 4096;

}

DeferredSerialLog DebugSerial;

void DeferredSerialLog::begin(unsigned long baud_rate) {
  pinMode(kSerialGatePin, INPUT_PULLUP);
  buffer_.reserve(kSerialBufferReserve);
  Serial.begin(baud_rate);

  if (digitalRead(kSerialGatePin) == LOW) {
    gate_seen_low_ = true;
    gate_low_at_ms_ = millis();
  }
}

void DeferredSerialLog::update() {
  if (ready_) {
    return;
  }

  if (!gate_seen_low_ && digitalRead(kSerialGatePin) == LOW) {
    gate_seen_low_ = true;
    gate_low_at_ms_ = millis();
  }

  if (!gate_seen_low_) {
    return;
  }

  if (millis() - gate_low_at_ms_ < kSerialReleaseDelayMs) {
    return;
  }

  ready_ = true;
  if (buffer_.length() > 0) {
    Serial.print(buffer_);
    buffer_ = "";
  }
}

bool DeferredSerialLog::ready() const {
  return ready_;
}

size_t DeferredSerialLog::write(uint8_t value) {
  if (ready_) {
    return Serial.write(value);
  }

  if (buffer_.length() < kSerialBufferReserve) {
    buffer_ += static_cast<char>(value);
  }

  return 1;
}
