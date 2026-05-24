#ifndef GLOW_PLUG_H
#define GLOW_PLUG_H

void setupGlowPlug();
void updateGlowPlug();
void turnGlowPlugOn();
void turnGlowPlugOff();
void setGlowPlugPercent(float throttle_percent);
float getGlowPlugPercent();
bool glowPlugIsOn();

#endif
