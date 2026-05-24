#ifndef SERIAL_LOG_H
#define SERIAL_LOG_H

#include <Arduino.h>

class DeferredSerialLog : public Print {
 public:
  void begin(unsigned long baud_rate);
  void update();
  bool ready() const;

  size_t write(uint8_t value) override;
  using Print::write;

 private:
  bool gate_seen_low_ = false;
  bool ready_ = false;
  unsigned long gate_low_at_ms_ = 0;
  String buffer_;
};

extern DeferredSerialLog DebugSerial;

#endif
