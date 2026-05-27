// src/Settings.h — 런타임 설정 (NVS 우선, 없으면 config.ini 기본값)
// BLE NUS 명령으로 런타임 변경 → NVS 저장 → 재부팅 후에도 유지
#pragma once
#include <Arduino.h>

#define SET_MAX_SCHEDS  8
#define SET_MAX_VIPS    8
#define SET_VIPLEN     64

struct SchedEntry {
  uint8_t hour;
  uint8_t minute;
  uint8_t wday;   // 0=일..6=토, 7=평일(월~금)
};

namespace Settings {
  // ── WiFi ──────────────────────────────────────────────────────────────────
  extern char wifiSsid[64];
  extern char wifiPass[64];

  // ── LED ───────────────────────────────────────────────────────────────────
  extern int  brightness;   // 0~255
  extern int  blinkMs;      // 점멸주기 ms

  // ── Poll ──────────────────────────────────────────────────────────────────
  extern int  pollSec;      // 폴링주기 초

  // ── 근무시간 ──────────────────────────────────────────────────────────────
  extern int  workStartMin; // 자정 이후 분 (예: 07:30 = 450)
  extern int  workEndMin;

  // ── VIP 발신인 ────────────────────────────────────────────────────────────
  extern char vip[SET_MAX_VIPS][SET_VIPLEN];
  extern int  vipCount;

  // ── 예약 알림 ─────────────────────────────────────────────────────────────
  extern SchedEntry scheds[SET_MAX_SCHEDS];
  extern int        schedCount;

  // ── 초기화 & NVS ──────────────────────────────────────────────────────────
  void init();             // config.ini 기본값 → NVS 오버라이드 순으로 로드
  void saveAll();          // 현재 값 전체 NVS 저장
  void resetToDefaults();  // NVS 키 전체 삭제 (재부팅은 호출자 책임)

  // ── VIP 헬퍼 ──────────────────────────────────────────────────────────────
  bool addVip(const char* email);   // 중복 체크 후 추가, 성공=true
  bool delVip(const char* email);   // 없으면 false

  // ── 스케줄 헬퍼 ───────────────────────────────────────────────────────────
  bool addSched(uint8_t h, uint8_t m, uint8_t wday);
  bool delSched(int idx);
  void clearScheds();
}
