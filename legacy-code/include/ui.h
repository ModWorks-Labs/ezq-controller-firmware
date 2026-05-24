#ifndef UI_H
#define UI_H

namespace ui_config {

constexpr unsigned int kBuzzerFrequencyHz = 2730;
constexpr unsigned int kBuzzerNoteLowHz = 2460;
constexpr unsigned int kBuzzerNoteMidHz = 2730;
constexpr unsigned int kBuzzerNoteHighHz = 2980;
constexpr unsigned int kBuzzerNoteBrightHz = 3220;
constexpr unsigned int kBuzzerNoteFaultHz = 2320;
constexpr unsigned int kBuzzerDutyByVolume[] = {0, 128, 256, 384, 511};

}

void setupUi();
void updateUi();
bool uiIsBusy();
void playStartupIndication();
void playWakeIndication();
void playButtonPressTone();
void playShutdownJingle();

void setUiReadyIdle();
void setUiForIdle(bool fan_off, float fan_throttle_percent);
void setUiArmingFlashYellow(bool on_phase);
void setUiCycleReadyYellow();
void setUiHeatingOrange();
void setUiStokingBlue();
void setUiFaultLatched();
void setUiOff();

void playCycleArmedBeeps();
void playCountdownBeep();
void playCountdownGoBeep();
void playPhaseCompleteChirp();
void playCycleCompleteJingle();
void startPostCycleReminderBeep();
void stopPostCycleReminderBeep();
void triggerUiFault();
bool uiFaultActive();

#endif
