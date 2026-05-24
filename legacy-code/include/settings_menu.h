#ifndef SETTINGS_MENU_H
#define SETTINGS_MENU_H

void setupSettingsMenu();
void updateSettingsMenu();
bool settingsMenuIsActive();
bool settingsMenuBlocksCycleManager();
bool settingsMenuOwnsUi();
bool settingsMenuAllowsSleep();
bool settingsMenuPreventsSleep();
void enterSettingsMenu();
void notifySettingsMenuButtonPressed(unsigned long press_started_ms);
void notifySettingsMenuButtonReleased(unsigned long release_ms);

#endif
