// src/Display.h — 0.91인치 128×32 SSD1306 OLED 표시 매니저 (U8g2)
#pragma once

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

class Display {
public:
  void begin(int sda, int scl);

  // 1) OAuth Device Code 인증 화면
  void showAuth(const char* userCode);

  // 2) 유휴 화면 (날짜/시간)
  void showIdle();

  // 3) 신규 이메일 알림 화면
  //    isCc = true  → 참조인
  //    isCc = false → 수신인
  void showEmail(const char* from, bool isCc, const char* subject);

  void showWifiError();          // WiFi 3회 연속 실패 — 영구 표시
  void showOAuthError();         // OAuth 토큰 갱신 3회 실패 — 영구 표시
  void clear();
  void update();   // loop() 에서 호출 — 시간 갱신 / 제목 스크롤

private:
  // ── 컨트롤러 변종 — 진단 결과 128×32 패널 확정 → UNIVISION 사용 ───────────
  U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C _u8g2{U8G2_R0, U8X8_PIN_NONE};

  enum class Mode : uint8_t { NONE, AUTH, IDLE, EMAIL } _mode{Mode::NONE};

  // 이메일 화면용 버퍼
  char _from[96]    = {};
  char _subject[128] = {};
  bool _isCc        = false;
  int  _labelWidth  = 0;   // (미사용, 0 고정)
  int  _senderPx    = 0;   // sender 전체 픽셀 폭
  int  _subjectPx   = 0;   // subject 전체 픽셀 폭

  // ── 순차 스크롤 상태머신 ────────────────────────────────────────────────────
  // 흐름: BOTTOM_PAUSE → BOTTOM_SCROLL → TOP_PAUSE → TOP_SCROLL → (반복)
  enum class ScrollState : uint8_t {
    IDLE,          // 스크롤 불필요 (양쪽 모두 128px 이하)
    BOTTOM_PAUSE,  // 하단(제목) 스크롤 시작 전 1초 대기
    BOTTOM_SCROLL, // 하단(제목) 좌로 스크롤 중
    TOP_PAUSE,     // 상단(발신자) 스크롤 시작 전 1초 대기
    TOP_SCROLL,    // 상단(발신자) 좌로 스크롤 중
  } _scrollState{ScrollState::IDLE};

  int           _scrollFromX    = 0;   // 발신자 스크롤 오프셋 (px)
  int           _scrollX        = 0;   // 제목 스크롤 오프셋 (px)
  unsigned long _scrollTimer    = 0;   // 상태 진입 시각 (pause 측정용)
  unsigned long _scrollMoveTimer= 0;   // 픽셀 이동 타이밍용

  void _redrawIdle();
  void _redrawEmail();
};
