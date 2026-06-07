// src/Display.cpp
#include "Display.h"
#include "config_generated.h"

#define OLED_W 128
#define OLED_H  32

// 한글 요일 이름 (UTF-8)
static const char* const DAY_KOR[7] = {"일","월","화","수","목","금","토"};

// ── 초기화 ────────────────────────────────────────────────────────────────────
void Display::begin(int sda, int scl) {
  // Wire.begin()을 직접 호출하지 않고 U8g2 내부에 핀 정보를 설정한 뒤
  // _u8g2.begin() 이 Wire.begin(sda, scl)을 한 번만 호출하게 함
  // → "[W] Bus already started in Master Mode" 경고 제거
  u8x8_SetPin_HW_I2C(_u8g2.getU8x8(), U8X8_PIN_NONE,
                      static_cast<uint8_t>(scl),
                      static_cast<uint8_t>(sda));
  if (!_u8g2.begin()) {
    Serial.println("[OLED] 초기화 실패 (연결 확인)");
    return;
  }
  _u8g2.enableUTF8Print();
  Serial.println("[OLED] 초기화 완료 (128x32 UNIVISION)");

  // // ── 확정 진단 (3초) ── 정상 확인 후 이 블록 삭제 ────────────────────────
  // _u8g2.clearBuffer();
  // _u8g2.drawFrame(0, 0, 128, 32);
  // _u8g2.drawLine(0, 0, 127, 31);
  // _u8g2.setFont(u8g2_font_5x7_tf);
  // for (int y = 0; y < 32; y += 8) {
  //   _u8g2.drawHLine(0, y, 10);
  //   char buf[8];
  //   snprintf(buf, sizeof(buf), "y=%d", y);
  //   _u8g2.drawStr(14, y + 6, buf);
  // }
  // _u8g2.sendBuffer();
  // Serial.println("[DIAG] 테두리 4변 닿고 y=0/8/16/24 보이면 128x32 확정");
  // delay(3000);

  _u8g2.clearDisplay();
}

void Display::clear() {
  _mode = Mode::NONE;
  _u8g2.clearDisplay();
}

// ── WiFi 에러 화면 ────────────────────────────────────────────────────────────
void Display::showWifiError() {
  _mode = Mode::NONE;
  _u8g2.clearBuffer();
  _u8g2.setFont(u8g2_font_5x7_tf);
  int x = (OLED_W - (int)_u8g2.getStrWidth("WiFi Connect Error.")) / 2;
  _u8g2.drawStr(x, 20, "WiFi Connect Error.");
  _u8g2.sendBuffer();
}

// ── OAuth 에러 화면 ───────────────────────────────────────────────────────────
// OAuth 토큰 갱신 3회 연속 실패 시 표시 — update()에서 갱신 안 됨 (고정 화면)
void Display::showOAuthError() {
  _mode = Mode::NONE;
  _u8g2.clearBuffer();
  _u8g2.setFont(u8g2_font_5x7_tf);
  // Line 1: "OAuth Error" 중앙 정렬  (baseline y=11)
  int x1 = (OLED_W - (int)_u8g2.getStrWidth("OAuth Error")) / 2;
  _u8g2.drawStr(x1, 11, "OAuth Error");
  // Line 2: 복구 방법 힌트 (baseline y=26)
  int x2 = (OLED_W - (int)_u8g2.getStrWidth("BLE: REBOOT")) / 2;
  _u8g2.drawStr(x2, 26, "BLE: REBOOT");
  _u8g2.sendBuffer();
}

// ── 1) 인증 화면 ──────────────────────────────────────────────────────────────
void Display::showAuth(const char* userCode) {
  _mode = Mode::AUTH;
  _u8g2.clearBuffer();

  // ── 상단: 접속 URL (5×7, baseline y=7, 상단 밀착) ────────────────────────
  _u8g2.setFont(u8g2_font_5x7_tf);
  _u8g2.drawStr(0, 7, "microsoft.com/devicelogin");

  // ── 하단: 인증 코드 (10×20, baseline y=31, 최대 크기, 중앙 정렬) ─────────
  _u8g2.setFont(u8g2_font_10x20_tf);
  const char* code = userCode ? userCode : "";
  int codeW = (int)_u8g2.getStrWidth(code);
  _u8g2.drawStr((OLED_W - codeW) / 2, 31, code);

  _u8g2.sendBuffer();
}

// ── 2) 유휴 화면 (날짜/시간) ──────────────────────────────────────────────────
void Display::showIdle() {
  _mode = Mode::IDLE;
  _redrawIdle();
}

void Display::_redrawIdle() {
  struct tm t;
  _u8g2.clearBuffer();

  if (!getLocalTime(&t, 0) || t.tm_year < 120) {
    _u8g2.setFont(u8g2_font_5x7_tf);
    _u8g2.drawStr(0, 15, "Syncing time...");
    _u8g2.sendBuffer();
    return;
  }

  // ── Line 1: 날짜 + 한글 요일 (unifont 16px, baseline y=13 → 상단 밀착) ──
  _u8g2.setFont(u8g2_font_unifont_t_korean2);
  char dateBuf[32];
  snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d(%s)",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, DAY_KOR[t.tm_wday]);
  int dateW = (int)_u8g2.getUTF8Width(dateBuf);
  _u8g2.drawUTF8((OLED_W - dateW) / 2, 13, dateBuf);

  // ── Line 2: 시간 + 초 (9×18px, baseline y=31 → 하단 밀착) ───────────────
  // 날짜 descent 끝 y=15(한글 시각적 공백) → 시간 top y=17 → 시각 공백 약 3px
  _u8g2.setFont(u8g2_font_9x18_tf);
  bool pm  = (t.tm_hour >= 12);
  int  h12 = t.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%s %02d:%02d:%02d",
           pm ? "PM" : "AM", h12, t.tm_min, t.tm_sec);
  int timeW = (int)_u8g2.getStrWidth(timeBuf);
  _u8g2.drawStr((OLED_W - timeW) / 2, 31, timeBuf);

  _u8g2.sendBuffer();
}

// ── 3) 이메일 알림 화면 ───────────────────────────────────────────────────────
void Display::showEmail(const char* from, bool isCc, const char* subject) {
  const char* f = from    ? from    : "";
  const char* s = subject ? subject : "";

  // ── 같은 이메일이면 스크롤 상태 유지 (10초 폴링마다 리셋 방지) ─────────────
  // 발신자·제목이 동일하고 이미 EMAIL 모드라면 아무것도 변경하지 않음
  if (_mode == Mode::EMAIL
      && strncmp(_from,    f, sizeof(_from)    - 1) == 0
      && strncmp(_subject, s, sizeof(_subject) - 1) == 0) {
    _isCc = isCc;   // 수신/참조 표시 정도만 갱신 (표시에 영향 없음)
    return;          // 스크롤 리셋 없이 그대로 유지
  }

  // ── 새 이메일: 버퍼·상태 전부 초기화 ────────────────────────────────────
  _mode = Mode::EMAIL;
  _isCc = isCc;

  strncpy(_from,    f, sizeof(_from)    - 1);  _from[sizeof(_from)    - 1] = '\0';
  strncpy(_subject, s, sizeof(_subject) - 1);  _subject[sizeof(_subject) - 1] = '\0';

  _u8g2.setFont(u8g2_font_unifont_t_korean2);
  _labelWidth = 0;
  _senderPx   = (int)_u8g2.getUTF8Width(_from);
  _subjectPx  = (int)_u8g2.getUTF8Width(_subject);

  Serial.printf("[DISP] showEmail sender=%dpx subject=%dpx\n", _senderPx, _subjectPx);

  // ── 순차 스크롤 초기화 ───────────────────────────────────────────────────
  _scrollX         = 0;
  _scrollFromX     = 0;
  _scrollTimer     = millis();
  _scrollMoveTimer = millis();

  // 어느 쪽이 스크롤이 필요한지에 따라 초기 상태 결정
  // 하단(제목) 우선 → 없으면 상단(발신자) → 둘 다 없으면 IDLE
  if (_subjectPx > OLED_W)      _scrollState = ScrollState::BOTTOM_PAUSE;
  else if (_senderPx > OLED_W)  _scrollState = ScrollState::TOP_PAUSE;
  else                           _scrollState = ScrollState::IDLE;

  _redrawEmail();
}

void Display::_redrawEmail() {
  _u8g2.clearBuffer();
  _u8g2.setFont(u8g2_font_unifont_t_korean2);  // 두 줄 모두 unifont 16px

  // ── 상단: 발신자 (baseline y=13) ─────────────────────────────────────────
  if (_senderPx > 0) {
    _u8g2.drawUTF8(-_scrollFromX, 13, _from);
  }

  // ── 하단: 제목 (baseline y=31) ───────────────────────────────────────────
  // 발신자 하단(y=13)에서 4행 공백(y=14~17) 후 글자 상단 y=18, descent만 클리핑
  if (_subjectPx > 0) {
    _u8g2.drawUTF8(-_scrollX, 29, _subject);
  }

  _u8g2.sendBuffer();
}

// ── update() — loop() 에서 호출 ──────────────────────────────────────────────
void Display::update() {
  unsigned long now = millis();

  // ── 유휴: 1초마다 시간 갱신 ─────────────────────────────────────────────────
  if (_mode == Mode::IDLE) {
    static unsigned long s_lastIdle = 0;
    if (now - s_lastIdle >= 1000UL) {
      s_lastIdle = now;
      _redrawIdle();
    }
    return;
  }

  // ── 이메일: 순차 스크롤 상태머신 ──────────────────────────────────────────
  // 흐름: BOTTOM_PAUSE(0.5s) → BOTTOM_SCROLL(50ms/px, 마지막 글자 사라지면 즉시 종료) →
  //        TOP_PAUSE(0.5s)    → TOP_SCROLL(100ms/px, 마지막 글자 사라지면 즉시 종료) → 반복
  if (_mode == Mode::EMAIL) {
    bool needRedraw = false;

    switch (_scrollState) {

      // ── 하단(제목) 스크롤 불필요 또는 양쪽 모두 짧은 경우 ─────────────────
      case ScrollState::IDLE:
        break;

      // ── 하단(제목) 대기 (0.5s) ──────────────────────────────────────────
      case ScrollState::BOTTOM_PAUSE:
        if (now - _scrollTimer >= 500UL) {
          _scrollState    = ScrollState::BOTTOM_SCROLL;
          _scrollMoveTimer = now;
        }
        break;

      // ── 하단(제목) 스크롤 ────────────────────────────────────────────────
      // 마지막 글자(_scrollX == _subjectPx)가 화면 왼쪽으로 사라지면 즉시 종료
      case ScrollState::BOTTOM_SCROLL:
        if (now - _scrollMoveTimer >= cfg::SCROLL_SUBJECT_MS) {
          _scrollMoveTimer = now;
          _scrollX++;
          needRedraw = true;
          if (_scrollX > _subjectPx) {   // 마지막 픽셀이 왼쪽 밖으로 나간 순간
            _scrollX     = 0;
            _scrollTimer = now;
            // 발신자 스크롤 필요하면 TOP_PAUSE, 아니면 다시 BOTTOM_PAUSE
            _scrollState = (_senderPx > OLED_W)
                           ? ScrollState::TOP_PAUSE
                           : ScrollState::BOTTOM_PAUSE;
          }
        }
        break;

      // ── 상단(발신자) 대기 (0.5s) ────────────────────────────────────────
      case ScrollState::TOP_PAUSE:
        if (now - _scrollTimer >= 500UL) {
          _scrollState     = ScrollState::TOP_SCROLL;
          _scrollMoveTimer = now;
        }
        break;

      // ── 상단(발신자) 스크롤 ──────────────────────────────────────────────
      // 마지막 글자(_scrollFromX == _senderPx)가 화면 왼쪽으로 사라지면 즉시 종료
      case ScrollState::TOP_SCROLL:
        if (now - _scrollMoveTimer >= cfg::SCROLL_SENDER_MS) {
          _scrollMoveTimer = now;
          _scrollFromX++;
          needRedraw = true;
          // 발신자가 화면 밖으로 완전히 사라지면 초기화하고 다음 단계로
          if (_scrollFromX > _senderPx) {   // 마지막 픽셀이 왼쪽 밖으로 나간 순간
            _scrollFromX = 0;
            _scrollTimer = now;
            // 제목 스크롤 필요하면 BOTTOM_PAUSE, 아니면 다시 TOP_PAUSE
            _scrollState = (_subjectPx > OLED_W)
                           ? ScrollState::BOTTOM_PAUSE
                           : ScrollState::TOP_PAUSE;
          }
        }
        break;
    }

    if (needRedraw) _redrawEmail();
  }
}
