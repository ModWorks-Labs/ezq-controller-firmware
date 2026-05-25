#include "output_control.h"

#include <algorithm>
#include <array>

#include "debug_console.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "pinout.h"

namespace output_control {
namespace {

constexpr char kTag[] = "output_control";
constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_bit_t kTimerBits = LEDC_TIMER_10_BIT;
constexpr uint8_t kLedBrightness = 32;
constexpr uint32_t kBuzzerDuty = 256;
constexpr uint32_t kBuzzerFreq = 2730;
constexpr uint32_t kBuzzerNoteLow = 2460;
constexpr uint32_t kBuzzerNoteMid = 2730;
constexpr uint32_t kBuzzerNoteHigh = 2980;
constexpr uint32_t kBuzzerNoteBright = 3220;
constexpr uint32_t kBuzzerNoteFault = 2320;

constexpr ledc_channel_t kBuzzerChannel = LEDC_CHANNEL_0;
constexpr ledc_channel_t kGlowChannel = LEDC_CHANNEL_1;
constexpr ledc_channel_t kFanChannel = LEDC_CHANNEL_2;

constexpr uint32_t kGlowFreq = 20000;
constexpr uint32_t kFanFreq = 20000;
constexpr uint32_t kMaxDuty10Bit = (1U << 10) - 1U;
constexpr uint32_t kFanWakePulseMs = 10;

struct ToneStep {
  uint32_t frequency_hz;
  uint32_t duration_ms;
  uint32_t gap_ms;
};

struct ToneSequence {
  std::array<ToneStep, 8> steps = {};
  std::size_t count = 0;
  std::size_t index = 0;
  bool playing = false;
  bool in_gap = false;
  uint32_t deadline_ms = 0;
  bool reminder_enabled = false;
  bool reminder_playing = false;
  uint32_t reminder_deadline_ms = 0;
};

bool g_initialized = false;
Indicator g_indicator = Indicator::OFF;
float g_fan_percent = 0.0f;
float g_glow_percent = 0.0f;
ToneSequence g_tones;
led_strip_handle_t g_led_strip = nullptr;
bool g_led_available = false;

struct FanThrottleMapPoint {
  float commanded_percent;
  float effective_percent;
};

constexpr FanThrottleMapPoint kFanThrottleMap[] = {
    {0.0f, 0.0f}, {5.0f, 5.0f},  {10.0f, 18.4f}, {20.0f, 26.2f},
    {30.0f, 33.4f}, {40.0f, 40.6f}, {50.0f, 47.7f}, {60.0f, 55.9f},
    {70.0f, 62.4f}, {80.0f, 68.4f}, {90.0f, 74.8f}, {100.0f, 96.0f},
};

float clamp_percent(float value) {
  return std::clamp(value, 0.0f, 100.0f);
}

bool check_esp(const char *step, esp_err_t err) {
  if (err == ESP_OK) {
    return true;
  }

  DEV_LOGE(kTag, "%s failed: %s", step, esp_err_to_name(err));
  return false;
}

void configure_sleep_hold_low(gpio_num_t pin) {
  gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  gpio_set_level(pin, 0);
  gpio_sleep_set_direction(pin, GPIO_MODE_OUTPUT);
  gpio_sleep_set_pull_mode(pin, GPIO_FLOATING);
  gpio_sleep_sel_en(pin);
  gpio_hold_en(pin);
}

void restore_held_pin(gpio_num_t pin) {
  gpio_hold_dis(pin);
  gpio_sleep_sel_dis(pin);
  gpio_reset_pin(pin);
}

bool restore_ledc_channel(gpio_num_t pin, ledc_channel_t channel, ledc_timer_t timer) {
  ledc_channel_config_t channel_config = {};
  channel_config.gpio_num = pin;
  channel_config.speed_mode = kMode;
  channel_config.channel = channel;
  channel_config.intr_type = LEDC_INTR_DISABLE;
  channel_config.timer_sel = timer;
  channel_config.duty = 0;
  channel_config.hpoint = 0;
  return check_esp("ledc_channel_config(restore)", ledc_channel_config(&channel_config));
}

void apply_led_color(uint8_t red, uint8_t green, uint8_t blue) {
  if (!g_led_available) {
    return;
  }

  if (led_strip_set_pixel(g_led_strip, 0, red, green, blue) != ESP_OK) {
    return;
  }
  led_strip_refresh(g_led_strip);
}

void write_buzzer(uint32_t frequency_hz, uint32_t duty) {
  if (!g_initialized) {
    return;
  }
  ledc_set_freq(kMode, LEDC_TIMER_0, frequency_hz);
  ledc_set_duty(kMode, kBuzzerChannel, duty);
  ledc_update_duty(kMode, kBuzzerChannel);
}

void buzzer_off() {
  if (!g_initialized) {
    return;
  }
  ledc_set_duty(kMode, kBuzzerChannel, 0);
  ledc_update_duty(kMode, kBuzzerChannel);
}

float map_fan_percent(float requested_percent) {
  if (requested_percent <= kFanThrottleMap[0].commanded_percent) {
    return kFanThrottleMap[0].effective_percent;
  }

  for (std::size_t i = 1; i < std::size(kFanThrottleMap); ++i) {
    const auto &prev = kFanThrottleMap[i - 1];
    const auto &next = kFanThrottleMap[i];
    if (requested_percent > next.commanded_percent) {
      continue;
    }
    const float span = next.commanded_percent - prev.commanded_percent;
    if (span <= 0.0f) {
      return next.effective_percent;
    }
    const float progress = (requested_percent - prev.commanded_percent) / span;
    return prev.effective_percent + ((next.effective_percent - prev.effective_percent) * progress);
  }

  return kFanThrottleMap[std::size(kFanThrottleMap) - 1].effective_percent;
}

uint32_t duty_from_percent(float requested_percent) {
  const float effective_percent = map_fan_percent(clamp_percent(requested_percent));
  return static_cast<uint32_t>((effective_percent / 100.0f) * kMaxDuty10Bit);
}

void write_fan(float requested_percent) {
  if (!g_initialized) {
    return;
  }
  if (requested_percent <= 0.0f) {
    ledc_stop(kMode, kFanChannel, 0);
    gpio_set_level(static_cast<gpio_num_t>(pinout::kBlowerFanDrivePwmPin), 0);
    return;
  }

  ledc_set_duty(kMode, kFanChannel, duty_from_percent(requested_percent));
  ledc_update_duty(kMode, kFanChannel);
}

void write_glow(float requested_percent) {
  if (!g_initialized) {
    return;
  }
  const uint32_t duty =
      static_cast<uint32_t>((clamp_percent(requested_percent) / 100.0f) * kMaxDuty10Bit);
  ledc_set_duty(kMode, kGlowChannel, duty);
  ledc_update_duty(kMode, kGlowChannel);
}

void queue_sequence(const std::initializer_list<ToneStep> &steps) {
  g_tones.count = std::min<std::size_t>(steps.size(), g_tones.steps.size());
  g_tones.index = 0;
  g_tones.playing = false;
  g_tones.in_gap = false;
  std::size_t i = 0;
  for (const auto &step : steps) {
    if (i >= g_tones.count) {
      break;
    }
    g_tones.steps[i++] = step;
  }
}

void start_tone(const ToneStep &step, uint32_t now_ms) {
  write_buzzer(step.frequency_hz, kBuzzerDuty);
  g_tones.playing = true;
  g_tones.in_gap = false;
  g_tones.deadline_ms = now_ms + step.duration_ms;
}

void start_gap(uint32_t gap_ms, uint32_t now_ms) {
  buzzer_off();
  g_tones.playing = false;
  g_tones.in_gap = true;
  g_tones.deadline_ms = now_ms + gap_ms;
}

void update_sequence(uint32_t now_ms) {
  if (g_tones.count == 0) {
    return;
  }

  if (!g_tones.playing && !g_tones.in_gap) {
    start_tone(g_tones.steps[g_tones.index], now_ms);
    return;
  }

  if (now_ms < g_tones.deadline_ms) {
    return;
  }

  if (g_tones.playing) {
    const uint32_t gap_ms = g_tones.steps[g_tones.index].gap_ms;
    if (gap_ms > 0) {
      start_gap(gap_ms, now_ms);
      return;
    }
    ++g_tones.index;
    g_tones.playing = false;
    if (g_tones.index >= g_tones.count) {
      g_tones.count = 0;
      buzzer_off();
    }
    return;
  }

  ++g_tones.index;
  g_tones.in_gap = false;
  if (g_tones.index >= g_tones.count) {
    g_tones.count = 0;
    buzzer_off();
  }
}

void update_reminder(uint32_t now_ms) {
  if (!g_tones.reminder_enabled || g_tones.count > 0 || g_tones.playing || g_tones.in_gap) {
    if (g_tones.reminder_playing) {
      buzzer_off();
      g_tones.reminder_playing = false;
    }
    return;
  }

  if (!g_tones.reminder_playing) {
    if (now_ms < g_tones.reminder_deadline_ms) {
      return;
    }
    write_buzzer(kBuzzerNoteMid, kBuzzerDuty);
    g_tones.reminder_playing = true;
    g_tones.reminder_deadline_ms = now_ms + 450;
    return;
  }

  if (now_ms < g_tones.reminder_deadline_ms) {
    return;
  }

  buzzer_off();
  g_tones.reminder_playing = false;
  g_tones.reminder_deadline_ms = now_ms + 15000;
}

void render_indicator(uint32_t now_ms) {
  switch (g_indicator) {
    case Indicator::OFF:
      apply_led_color(0, 0, 0);
      break;

    case Indicator::BOOT:
      apply_led_color(0, 0, 0);
      break;

    case Indicator::UPDATE_FW:
      apply_led_color(kLedBrightness, 0, kLedBrightness);
      break;

    case Indicator::READY_IDLE:
      apply_led_color(0, kLedBrightness, 0);
      break;

    case Indicator::BLOWER_MODE: {
      uint32_t blink_period_ms = 900;
      if (g_fan_percent > 0.0f) {
        float throttle_percent = g_fan_percent > 95.0f ? 95.0f : g_fan_percent;
        blink_period_ms -= static_cast<uint32_t>(throttle_percent * 7.0f);
        if (blink_period_ms < 180) {
          blink_period_ms = 180;
        }
      }
      const bool led_on = (now_ms % blink_period_ms) < (blink_period_ms / 2U);
      apply_led_color(0, 0, led_on ? kLedBrightness : 0);
      break;
    }

    case Indicator::ARMING: {
      const bool led_on = ((now_ms / 200U) % 2U) == 0U;
      apply_led_color(led_on ? kLedBrightness : 0, led_on ? kLedBrightness : 0, 0);
      break;
    }

    case Indicator::CYCLE_READY:
      apply_led_color(kLedBrightness, kLedBrightness, 0);
      break;

    case Indicator::IGNITION_ACTIVE:
      apply_led_color(kLedBrightness, 12, 0);
      break;

    case Indicator::POST_CYCLE:
    case Indicator::COOLDOWN:
      apply_led_color(0, 0, kLedBrightness);
      break;

    case Indicator::FAULT:
      apply_led_color(kLedBrightness, 0, 0);
      break;
  }
}

}  // namespace

bool init() {
  if (g_initialized) {
    return true;
  }

  gpio_config_t fan_gpio = {};
  fan_gpio.pin_bit_mask = 1ULL << pinout::kBlowerFanDrivePwmPin;
  fan_gpio.mode = GPIO_MODE_OUTPUT;
  fan_gpio.pull_down_en = GPIO_PULLDOWN_DISABLE;
  fan_gpio.pull_up_en = GPIO_PULLUP_DISABLE;
  fan_gpio.intr_type = GPIO_INTR_DISABLE;
  if (!check_esp("gpio_config(fan)", gpio_config(&fan_gpio))) {
    return false;
  }

  ledc_timer_config_t buzzer_timer = {};
  buzzer_timer.speed_mode = kMode;
  buzzer_timer.timer_num = LEDC_TIMER_0;
  buzzer_timer.freq_hz = static_cast<int>(kBuzzerFreq);
  buzzer_timer.duty_resolution = kTimerBits;
  buzzer_timer.clk_cfg = LEDC_AUTO_CLK;
  if (!check_esp("ledc_timer_config(buzzer)", ledc_timer_config(&buzzer_timer))) {
    return false;
  }

  ledc_timer_config_t glow_timer = buzzer_timer;
  glow_timer.timer_num = LEDC_TIMER_1;
  glow_timer.freq_hz = static_cast<int>(kGlowFreq);
  if (!check_esp("ledc_timer_config(glow)", ledc_timer_config(&glow_timer))) {
    return false;
  }

  ledc_timer_config_t fan_timer = buzzer_timer;
  fan_timer.timer_num = LEDC_TIMER_2;
  fan_timer.freq_hz = static_cast<int>(kFanFreq);
  if (!check_esp("ledc_timer_config(fan)", ledc_timer_config(&fan_timer))) {
    return false;
  }

  ledc_channel_config_t buzzer_channel = {};
  buzzer_channel.gpio_num = pinout::kBuzzerPin;
  buzzer_channel.speed_mode = kMode;
  buzzer_channel.channel = kBuzzerChannel;
  buzzer_channel.intr_type = LEDC_INTR_DISABLE;
  buzzer_channel.timer_sel = LEDC_TIMER_0;
  buzzer_channel.duty = 0;
  buzzer_channel.hpoint = 0;
  if (!check_esp("ledc_channel_config(buzzer)", ledc_channel_config(&buzzer_channel))) {
    return false;
  }

  ledc_channel_config_t glow_channel = {};
  glow_channel.gpio_num = pinout::kGlowPlugDrivePin;
  glow_channel.speed_mode = kMode;
  glow_channel.channel = kGlowChannel;
  glow_channel.intr_type = LEDC_INTR_DISABLE;
  glow_channel.timer_sel = LEDC_TIMER_1;
  glow_channel.duty = 0;
  glow_channel.hpoint = 0;
  if (!check_esp("ledc_channel_config(glow)", ledc_channel_config(&glow_channel))) {
    return false;
  }

  ledc_channel_config_t fan_channel = {};
  fan_channel.gpio_num = pinout::kBlowerFanDrivePwmPin;
  fan_channel.speed_mode = kMode;
  fan_channel.channel = kFanChannel;
  fan_channel.intr_type = LEDC_INTR_DISABLE;
  fan_channel.timer_sel = LEDC_TIMER_2;
  fan_channel.duty = 0;
  fan_channel.hpoint = 0;
  if (!check_esp("ledc_channel_config(fan)", ledc_channel_config(&fan_channel))) {
    return false;
  }

  led_strip_config_t led_config = {};
  led_config.strip_gpio_num = pinout::kLedPwmPin;
  led_config.max_leds = 1;
  led_config.led_pixel_format = LED_PIXEL_FORMAT_GRB;
  led_config.led_model = LED_MODEL_WS2812;
  led_config.flags.invert_out = false;

  led_strip_rmt_config_t rmt_config = {};
  rmt_config.resolution_hz = 10 * 1000 * 1000;
  rmt_config.flags.with_dma = false;

  const esp_err_t led_err = led_strip_new_rmt_device(&led_config, &rmt_config, &g_led_strip);
  if (led_err == ESP_OK) {
    g_led_available = true;
  } else {
    DEV_LOGW(kTag, "LED init skipped: %s", esp_err_to_name(led_err));
    g_led_strip = nullptr;
    g_led_available = false;
  }

  g_initialized = true;
  force_safe_outputs();
  DEV_LOGI(kTag, "Output control initialized");
  return true;
}

void tick(uint32_t now_ms) {
  if (!g_initialized) {
    return;
  }
  update_sequence(now_ms);
  update_reminder(now_ms);
  render_indicator(now_ms);
}

void set_indicator(Indicator indicator_value) {
  g_indicator = indicator_value;
}

Indicator indicator() {
  return g_indicator;
}

void set_fan_off() {
  g_fan_percent = 0.0f;
  write_fan(0.0f);
}

void set_fan_percent(float percent) {
  if (!g_initialized) {
    g_fan_percent = 0.0f;
    return;
  }
  const float clamped = clamp_percent(percent);
  const bool waking = g_fan_percent <= 0.0f && clamped > 0.0f;
  if (waking) {
    ledc_set_duty(kMode, kFanChannel, kMaxDuty10Bit);
    ledc_update_duty(kMode, kFanChannel);
    vTaskDelay(pdMS_TO_TICKS(kFanWakePulseMs));
  }
  g_fan_percent = clamped;
  write_fan(clamped);
}

float fan_percent() {
  return g_fan_percent;
}

bool fan_active() {
  return g_fan_percent > 0.0f;
}

void set_glow_off() {
  set_glow_percent(0.0f);
}

void set_glow_percent(float percent) {
  if (!g_initialized) {
    g_glow_percent = 0.0f;
    return;
  }
  g_glow_percent = clamp_percent(percent);
  write_glow(g_glow_percent);
}

float glow_percent() {
  return g_glow_percent;
}

bool glow_active() {
  return g_glow_percent > 0.0f;
}

void play_startup_jingle() {
  queue_sequence({{kBuzzerNoteMid, 95, 28},
                  {kBuzzerNoteHigh, 95, 28},
                  {kBuzzerNoteBright, 170, 0}});
}

void play_button_press() {
  queue_sequence({{kBuzzerFreq, 35, 0}});
}

void play_mode_entry_beep() {
  queue_sequence({{kBuzzerNoteHigh, 75, 0}});
}

void play_mode_exit_beep() {
  queue_sequence({{kBuzzerNoteLow, 75, 0}});
}

void play_cycle_armed_beeps() {
  queue_sequence({{kBuzzerNoteMid, 65, 50}, {kBuzzerNoteHigh, 65, 50}, {kBuzzerNoteBright, 85, 0}});
}

void play_countdown_beep() {
  queue_sequence({{kBuzzerNoteMid, 80, 0}});
}

void play_countdown_go_beep() {
  queue_sequence({{kBuzzerNoteBright, 120, 0}});
}

void play_phase_complete_chirp() {
  queue_sequence({{kBuzzerNoteMid, 75, 25}, {kBuzzerNoteHigh, 105, 0}});
}

void play_cycle_complete_jingle() {
  queue_sequence(
      {{kBuzzerNoteLow, 100, 35}, {kBuzzerNoteMid, 120, 35}, {kBuzzerNoteHigh, 150, 40}, {kBuzzerNoteBright, 220, 0}});
}

void start_post_cycle_reminder() {
  g_tones.reminder_enabled = true;
  g_tones.reminder_playing = false;
  g_tones.reminder_deadline_ms = 15000;
}

void stop_post_cycle_reminder() {
  g_tones.reminder_enabled = false;
  g_tones.reminder_playing = false;
  buzzer_off();
}

void play_fault_pattern() {
  stop_post_cycle_reminder();
  queue_sequence({{kBuzzerNoteLow, 260, 90},
                  {kBuzzerNoteFault, 260, 90},
                  {kBuzzerNoteLow, 260, 100},
                  {kBuzzerNoteFault, 360, 0}});
}

void force_safe_outputs() {
  stop_post_cycle_reminder();
  g_fan_percent = 0.0f;
  g_glow_percent = 0.0f;
  set_indicator(Indicator::OFF);
  write_fan(0.0f);
  write_glow(0.0f);
  buzzer_off();
}

void prepare_for_light_sleep() {
  if (!g_initialized) {
    return;
  }

  force_safe_outputs();
  configure_sleep_hold_low(static_cast<gpio_num_t>(pinout::kBuzzerPin));
  configure_sleep_hold_low(static_cast<gpio_num_t>(pinout::kBlowerFanDrivePwmPin));
  configure_sleep_hold_low(static_cast<gpio_num_t>(pinout::kGlowPlugDrivePin));
}

void restore_after_light_sleep() {
  if (!g_initialized) {
    return;
  }

  const auto buzzer_pin = static_cast<gpio_num_t>(pinout::kBuzzerPin);
  const auto fan_pin = static_cast<gpio_num_t>(pinout::kBlowerFanDrivePwmPin);
  const auto glow_pin = static_cast<gpio_num_t>(pinout::kGlowPlugDrivePin);

  restore_held_pin(buzzer_pin);
  restore_held_pin(fan_pin);
  restore_held_pin(glow_pin);

  restore_ledc_channel(buzzer_pin, kBuzzerChannel, LEDC_TIMER_0);
  restore_ledc_channel(fan_pin, kFanChannel, LEDC_TIMER_2);
  restore_ledc_channel(glow_pin, kGlowChannel, LEDC_TIMER_1);
}

}  // namespace output_control
