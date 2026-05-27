// src/Led.cpp
#include "Led.h"

void Led::begin(int pin, uint8_t brightness, uint16_t blinkMs) {
  _brightness = brightness;
  _blinkMs    = blinkMs;
  _strip.updateLength(1);
  _strip.setPin(pin);
  _strip.begin();
  _strip.setBrightness(_brightness);
  _strip.clear();
  _strip.show();
}

void Led::setState(LedState s) {
  if (_state == s) return;
  _state = s;
  _lastTick = millis();
  _on = false;

  // OFF 진입 시 즉시 LED 끄기 (직전 상태 잔상 방지)
  if (s == LedState::OFF) {
    _show(0, 0, 0);
    return;
  }
  // GREEN_SOLID 진입 시 즉시 초록 점등 (점멸 안 함)
  if (s == LedState::GREEN_SOLID) {
    _show(0, 255, 0);
    return;
  }
  update();
}

void Led::_show(uint8_t r, uint8_t g, uint8_t b) {
  _strip.setPixelColor(0, _strip.Color(r, g, b));
  _strip.show();
}

void Led::setBrightness(uint8_t v) {
  _brightness = v;
  _strip.setBrightness(v);
  // 정적 상태(OFF, GREEN_SOLID)는 즉시 재렌더, 나머지는 update()에서 자연 갱신
  switch (_state) {
    case LedState::OFF:         _show(0, 0, 0);   break;
    case LedState::GREEN_SOLID: _show(0, 255, 0); break;
    default: break;
  }
}

void Led::setBlinkMs(uint16_t ms) {
  _blinkMs = ms;
}

void Led::flashOnce() {
  if (_state != LedState::GREEN_SOLID) return;
  _flashActive = true;
  _flashEndAt  = millis() + 300;
  _show(0, 0, 0);
}

void Led::update() {
  unsigned long now = millis();

  // flash-once 복원 처리 (GREEN_SOLID 한 번 깜빡 후 복원)
  if (_flashActive) {
    if (now >= _flashEndAt) {
      _flashActive = false;
      if (_state == LedState::GREEN_SOLID) _show(0, 255, 0);
    }
    return;
  }

  switch (_state) {
    case LedState::OFF:
    case LedState::GREEN_SOLID:
      // setState 에서 이미 한 번 그렸으니 여기선 아무것도 안 함
      return;

    // 점멸 (blink): on/off 반복
    case LedState::RED_BLINK:
    case LedState::BLUE_BLINK: {
      if (now - _lastTick < _blinkMs) return;
      _lastTick = now;
      _on = !_on;
      if (!_on) { _show(0, 0, 0); return; }
      switch (_state) {
        case LedState::RED_BLINK:   _show(255, 0,   0);   break;
        case LedState::BLUE_BLINK:  _show(0,   0,   255); break;
        default: break;
      }
      return;
    }

    // 펄스 (pulse): 사인파 밝기
    case LedState::WHITE_PULSE:
    case LedState::YELLOW_PULSE:
    case LedState::PURPLE_PULSE: {
      float phase = (now % 1500) / 1500.0f;
      float v = (sinf(phase * 2 * PI) + 1.0f) * 0.5f;
      uint8_t br = (uint8_t)(v * 255);
      uint8_t r = 0, g = 0, b = 0;
      if (_state == LedState::WHITE_PULSE)  { r = g = b = br; }
      if (_state == LedState::YELLOW_PULSE) { r = br; g = br; b = 0; }
      if (_state == LedState::PURPLE_PULSE) { r = br; g = 0; b = br; }
      _show(r, g, b);
      return;
    }

    // 부팅 자가테스트
    case LedState::CYAN_FLASH: {
      if (now - _lastTick < 100) return;
      _lastTick = now;
      _on = !_on;
      _show(_on ? 0 : 0, _on ? 200 : 0, _on ? 200 : 0);
      return;
    }

    // WiFi 포기 — 오렌지 0.5s ON / 29.5s OFF (30초 주기)
    case LedState::ORANGE_SLOW_BLINK: {
      unsigned long wait = _on ? 500UL : 29500UL;
      if (now - _lastTick < wait) return;
      _lastTick = now;
      _on = !_on;
      _show(_on ? 255 : 0, _on ? 80 : 0, 0);
      return;
    }
  }
}
