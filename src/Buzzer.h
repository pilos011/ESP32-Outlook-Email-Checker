// src/Buzzer.h — Passive/Active 부저 단발 비프 (논블로킹)
//   Passive Buzzer: PWM 톤으로 동작, freq 변경 가능
//   Active Buzzer:  freq 무시, ON/OFF 만 작동 (그래도 잘 울림)
//   볼륨은 PWM duty cycle 로 조절 (0~100)
//   - 50% duty 가 최대 음량 → volume 100 일 때 duty=127
//   - duty=0 이면 무음
#pragma once

#include <Arduino.h>

class Buzzer {
public:
  void begin(int pin, uint8_t volume, uint32_t freq);
  void beep(uint32_t durationMs);   // 비프 1회 시작 (논블로킹)
  void update();                    // loop()에서 자주 호출, 끝나면 자동 정지
  bool isEnabled() const { return _volume > 0 && _pin >= 0; }

private:
  int      _pin = -1;
  uint8_t  _volume = 0;             // 0~100
  uint32_t _freq = 2700;
  unsigned long _stopAt = 0;
  bool _active = false;

  static constexpr uint8_t LEDC_CHANNEL    = 4;   // 다른 라이브러리와 충돌 회피
  static constexpr uint8_t LEDC_RESOLUTION = 8;   // 8-bit (0~255)
};
