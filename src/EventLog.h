// src/EventLog.h — NVS 기반 이벤트 로그 (재부팅 후에도 유지)
// BLE 명령 LOG 로 조회, LOG:clear 로 초기화
// 최대 30건 순환 기록 (초과 시 가장 오래된 항목 덮어씀)
#pragma once
#include <Arduino.h>

namespace EventLog {
  void begin();                               // NVS에서 상태 로드 (setup 맨 앞)
  void log(const char* tag,
           const char* detail = "");          // 이벤트 기록
  void sendViaBle();                          // BLE NUS로 전체 로그 전송
  void clear();                               // 로그 전체 삭제
}
