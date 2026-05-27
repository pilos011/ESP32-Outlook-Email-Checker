// src/Ble.h — BLE NUS (Nordic UART Service) 설정 콘솔
// nRF Connect 앱으로 명령 전송 → 런타임 설정 변경
// 보안 없음(암호화 X) — 내부 사무실 전용 장치 실용 타협
#pragma once
#include <Arduino.h>

namespace Ble {
  void   begin(const char* deviceName);  // BLE 초기화 + 광고 시작
  bool   hasPendingCommand();            // 처리할 명령이 있으면 true
  String dequeueCommand();               // 명령 꺼내기 (큐 클리어)
  void   send(const String& msg);        // 폰으로 응답 전송
  bool   isConnected();
}
