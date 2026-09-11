// File "MPR121_GestureHelper"
#include <Arduino.h>
#include "MPR121_GestureHelper.h"

#ifndef _BV
#define _BV(bit) (1 << (bit))
#endif

MPR121_GestureHelper::MPR121_GestureHelper()
  : sensorCount(0),
    touchCallback(nullptr),
    gestureCallback(nullptr),
    debugEnabled(true) {
  // Set default timing values
  LONG_PRESS_TIME = 300;
  DOUBLE_TAP_TIME = 200;
  DEBOUNCE_TIME = 10;
}

void MPR121_GestureHelper::begin(uint16_t longPressTime, uint16_t doubleTapTime, uint16_t debounceTime) {
  LONG_PRESS_TIME = longPressTime;
  DOUBLE_TAP_TIME = doubleTapTime;
  DEBOUNCE_TIME = debounceTime;

  // Initialize all channels
  for (int i = 0; i < MAX_SENSORS * CHANNELS_PER_SENSOR; i++) {
    channels[i].currentState = false;
    channels[i].lastState = false;
    channels[i].longPressActive = false;
    channels[i].doubleTapPossible = false;
    channels[i].justHadDoubleTap = false;
    channels[i].bypassGestures = false;
    channels[i].pressTime = 0;
    channels[i].releaseTime = 0;
  }

  // Initialize sensors
  for (int i = 0; i < MAX_SENSORS; i++) {
    sensors[i].initialized = false;
    sensors[i].lastState = 0;
    sensors[i].currentState = 0;
  }

  if (debugEnabled) {
    Serial.println("MPR121 Gesture Helper initialized");
    Serial.print("Timing: LongPress=");
    Serial.print(LONG_PRESS_TIME);
    Serial.print("ms, DoubleTap=");
    Serial.print(DOUBLE_TAP_TIME);
    Serial.print("ms, Debounce=");
    Serial.print(DEBOUNCE_TIME);
    Serial.println("ms");
  }
}

bool MPR121_GestureHelper::addSensor(uint8_t address) {
  if (sensorCount >= MAX_SENSORS) {
    if (debugEnabled) {
      Serial.println("Cannot add more sensors! Maximum is 4.");
    }
    return false;
  }

  sensors[sensorCount].address = address;

  // Set sensor name based on index
  switch (sensorCount) {
    case 0: sensors[sensorCount].name = "S1"; break;
    case 1: sensors[sensorCount].name = "S2"; break;
    case 2: sensors[sensorCount].name = "S3"; break;
    case 3: sensors[sensorCount].name = "S4"; break;
  }

  if (!sensors[sensorCount].cap.begin(address, &Wire)) {
    if (debugEnabled) {
      Serial.print(sensors[sensorCount].name);
      Serial.print(" (0x");
      Serial.print(address, HEX);
      Serial.println(") not found!");
    }
    return false;
  }

  sensors[sensorCount].cap.setAutoconfig(true);
  sensors[sensorCount].initialized = true;

  if (debugEnabled) {
    Serial.print(sensors[sensorCount].name);
    Serial.print(" (0x");
    Serial.print(address, HEX);
    Serial.println(") initialized!");
  }

  sensorCount++;
  delay(20);  // Give sensor time to stabilize
  return true;
}

// In MPR121_GestureHelper.cpp
void MPR121_GestureHelper::setThresholds(uint8_t sensorIndex, uint8_t touch, uint8_t release) {
  // Check if sensor index is valid
  if (sensorIndex >= sensorCount) {
    if (debugEnabled) {
      Serial.print("Invalid sensor index: ");
      Serial.println(sensorIndex);
    }
    return;
  }

  // Check if sensor is initialized
  if (!sensors[sensorIndex].initialized) {
    if (debugEnabled) {
      Serial.print("Sensor ");
      Serial.print(sensorIndex);
      Serial.println(" not initialized!");
    }
    return;
  }

  // Set thresholds using the MPR121 library
  sensors[sensorIndex].cap.setThresholds(touch, release);
  
  if (debugEnabled) {
    Serial.print(sensors[sensorIndex].name);
    Serial.print(" thresholds set: touch=");
    Serial.print(touch);
    Serial.print(", release=");
    Serial.println(release);
  }
}

void MPR121_GestureHelper::update() {
  if (sensorCount == 0) return;  // No sensors added

  for (uint8_t i = 0; i < sensorCount; i++) {
    if (sensors[i].initialized) {
      processSensor(i);
    }
  }
}

void MPR121_GestureHelper::processSensor(uint8_t sensorIndex) {
  if (!sensors[sensorIndex].initialized) return;

  // Read current touch state
  sensors[sensorIndex].currentState = sensors[sensorIndex].cap.touched();

  // Process each channel
  for (uint8_t ch = 0; ch < CHANNELS_PER_SENSOR; ch++) {
    bool is = sensors[sensorIndex].currentState & _BV(ch);
    processChannel(sensorIndex, ch, is);
  }

  sensors[sensorIndex].lastState = sensors[sensorIndex].currentState;
}

void MPR121_GestureHelper::processChannel(uint8_t sensorIndex, uint8_t channel, bool isTouched) {
  int idx = getChannelIndex(sensorIndex, channel);
  unsigned long currentTime = millis();

  // Update states
  channels[idx].lastState = channels[idx].currentState;
  channels[idx].currentState = isTouched;

  // Bypass mode: immediate touch/release, no debounce, no double-tap, no long-press
  if (channels[idx].bypassGestures) {
    if (channels[idx].currentState != channels[idx].lastState) {
      if (touchCallback) {
        touchCallback(sensorIndex, channel, isTouched);
      }
    }
    return;
  }

  // Normal mode: full gesture processing with debounce
  if (channels[idx].currentState != channels[idx].lastState) {
    if (currentTime - channels[idx].releaseTime > DEBOUNCE_TIME) {

      if (isTouched) {  // Touch detected (finger down)
        channels[idx].pressTime = currentTime;

        // Check for double tap
        if (channels[idx].doubleTapPossible && (currentTime - channels[idx].releaseTime) <= DOUBLE_TAP_TIME) {
          if (debugEnabled) {
            Serial.print(sensors[sensorIndex].name);
            Serial.print(" ch");
            Serial.print(channel);
            Serial.println(" double tap detected!");
          }

          if (gestureCallback) {
            gestureCallback(sensorIndex, channel, "double_tap", 0);
          }

          channels[idx].doubleTapPossible = false;
          channels[idx].justHadDoubleTap = true;
          return;
        } else {
          channels[idx].doubleTapPossible = true;
          channels[idx].justHadDoubleTap = false;
        }

        channels[idx].longPressActive = true;

        if (debugEnabled) {
          Serial.print(sensors[sensorIndex].name);
          Serial.print(" ch");
          Serial.print(channel);
          Serial.println(" touched");
        }

        if (touchCallback) {
          touchCallback(sensorIndex, channel, true);
        }

      } else {  // Release detected (finger up)
        channels[idx].releaseTime = currentTime;

        if (channels[idx].longPressActive) {
          unsigned long pressDuration = currentTime - channels[idx].pressTime;

          if (pressDuration < LONG_PRESS_TIME) {
            if (!channels[idx].justHadDoubleTap) {
              channels[idx].doubleTapPossible = true;
            }
          } else {
            channels[idx].doubleTapPossible = false;
          }

          channels[idx].longPressActive = false;
          channels[idx].justHadDoubleTap = false;
        }

        if (debugEnabled) {
          Serial.print(sensors[sensorIndex].name);
          Serial.print(" ch");
          Serial.print(channel);
          Serial.println(" released");
        }

        if (touchCallback) {
          touchCallback(sensorIndex, channel, false);
        }
      }
    }
  }

  // Check for ongoing long press (continuous check while touched)
  if (channels[idx].currentState && channels[idx].longPressActive) {
    unsigned long pressDuration = currentTime - channels[idx].pressTime;

    if (pressDuration >= LONG_PRESS_TIME) {
      if (debugEnabled) {
        Serial.print(sensors[sensorIndex].name);
        Serial.print(" ch");
        Serial.print(channel);
        Serial.print(" long press detected! (");
        Serial.print(pressDuration);
        Serial.println(" ms)");
      }

      if (gestureCallback) {
        gestureCallback(sensorIndex, channel, "long_press", pressDuration);
      }

      channels[idx].longPressActive = false;
      channels[idx].doubleTapPossible = false;
    }
  }

  // Clear double tap flag if too much time has passed since release
  if (channels[idx].doubleTapPossible && !channels[idx].currentState) {
    if ((currentTime - channels[idx].releaseTime) > DOUBLE_TAP_TIME) {
      channels[idx].doubleTapPossible = false;
      channels[idx].justHadDoubleTap = false;
    }
  }
}

int MPR121_GestureHelper::getChannelIndex(uint8_t sensorIndex, uint8_t channel) {
  return (sensorIndex * CHANNELS_PER_SENSOR) + channel;
}

void MPR121_GestureHelper::onTouchEvent(TouchEventCallback callback) {
  touchCallback = callback;
}

void MPR121_GestureHelper::onGesture(GestureCallback callback) {
  gestureCallback = callback;
}

void MPR121_GestureHelper::setDebug(bool enable) {
  debugEnabled = enable;
}

void MPR121_GestureHelper::setChannelBypassGestures(uint8_t sensorIndex, uint8_t channel, bool bypass) {
  int idx = getChannelIndex(sensorIndex, channel);
  channels[idx].bypassGestures = bypass;
  // Reset gesture state when enabling bypass
  if (bypass) {
    channels[idx].longPressActive = false;
    channels[idx].doubleTapPossible = false;
    channels[idx].justHadDoubleTap = false;
  }
}

void MPR121_GestureHelper::clearAllBypassGestures() {
  for (int i = 0; i < MAX_SENSORS * CHANNELS_PER_SENSOR; i++) {
    channels[i].bypassGestures = false;
    channels[i].longPressActive = false;
    channels[i].doubleTapPossible = false;
    channels[i].justHadDoubleTap = false;
  }
}
