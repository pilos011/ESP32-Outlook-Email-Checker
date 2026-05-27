// src/Led.h — WS2812 NeoPixel 기반 RGB LED 점멸 매니저
#pragma once

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

enum class LedState : uint8_t {
  OFF,             // 점멸 끔
  RED_BLINK,       // VIP 발신인 메일 unread (최우선)
  BLUE_BLINK,      // 내가 유일한 수신인 (unread)
  GREEN_SOLID,     // 그 외 unread 메일 있음 (점멸 X, 항상 켜짐)
  WHITE_PULSE,     // WiFi 연결 중
  YELLOW_PULSE,    // OAuth 인증 대기 중
  PURPLE_PULSE,    // 인증 에러
  CYAN_FLASH,      // 부팅 자가테스트
  ORANGE_SLOW_BLINK // WiFi 3회 연속 실패 — 재연결 포기
};

class Led {
public:
  void begin(int pin, uint8_t brightness, uint16_t blinkMs);
  void setState(LedState s);
  void flashOnce();                           // GREEN_SOLID 상태에서 300ms 깜빡 후 복원
  void setBrightness(uint8_t v);              // BLE 밝기 명령으로 즉시 적용
  void setBlinkMs(uint16_t ms);              // BLE 점멸주기 명령으로 적용
  LedState state() const { return _state; }
  void update();   // loop()에서 자주 호출
private:
  Adafruit_NeoPixel _strip{1, -1, NEO_GRB + NEO_KHZ800};
  LedState _state = LedState::OFF;
  uint16_t _blinkMs = 500;
  uint8_t  _brightness = 80;
  bool     _on = false;
  unsigned long _lastTick   = 0;
  bool          _flashActive = false;
  unsigned long _flashEndAt  = 0;
  void _show(uint8_t r, uint8_t g, uint8_t b);
};
