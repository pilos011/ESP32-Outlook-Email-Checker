// src/Buzzer.cpp
#include "Buzzer.h"

void Buzzer::begin(int pin, uint8_t volume, uint32_t freq) {
  _pin = pin;
  _volume = (volume > 100) ? 100 : volume;
  _freq = freq;

  if (_volume == 0 || _pin < 0) {
    Serial.println("[BUZZ] 비활성화 (volume=0 또는 pin<0)");
    return;
  }

  // Arduino-ESP32 v2.x LEDC API
  ledcSetup(LEDC_CHANNEL, _freq, LEDC_RESOLUTION);
  ledcAttachPin(_pin, LEDC_CHANNEL);
  ledcWrite(LEDC_CHANNEL, 0);   // 시작은 무음

  Serial.printf("[BUZZ] init pin=%d  volume=%d  freq=%uHz\n",
    _pin, _volume, _freq);
}

void Buzzer::beep(uint32_t durationMs) {
  if (!isEnabled()) return;

  // duty 50% (=127/255) 가 square wave 최대 음량
  // volume 0~100 -> duty 0~127 매핑
  uint32_t duty = ((uint32_t)_volume * 127) / 100;
  if (duty == 0) return;

  ledcWrite(LEDC_CHANNEL, duty);
  _stopAt = millis() + durationMs;
  _active = true;
  Serial.printf("[BUZZ] beep %ums (duty=%u)\n", durationMs, duty);
}

void Buzzer::update() {
  if (!_active) return;
  if (millis() >= _stopAt) {
    ledcWrite(LEDC_CHANNEL, 0);
    _active = false;
  }
}
