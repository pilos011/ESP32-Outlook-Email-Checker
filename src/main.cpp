// src/main.cpp — Outlook Mail LED 메인

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "config_generated.h"
#include "Led.h"
#include "Buzzer.h"
#include "Oauth.h"
#include "Outlook.h"
#include "Display.h"

Led      led;
Buzzer   buzz;
Oauth    oauth;
Outlook  outlook;
Display  disp;

unsigned long lastPoll      = 0;
unsigned long lastWifiCheck = 0;
int           lastPriority  = -2;
int           wifiFailCount = 0;
bool          s_wifiDead    = false;
int           s_prevUnreadCount = -1;

// ── 비동기 폴링 (코어 0 FreeRTOS 태스크) ─────────────────────────────────────
static volatile bool s_pollBusy = false;   // 태스크 실행 중 플래그
static volatile bool s_pollDone = false;   // 태스크 완료 → 메인루프 결과 적용
static int           s_taskPollP = -2;     // 태스크가 기록한 우선순위 결과

// ── 예약 알림 (하드코딩) ──────────────────────────────────────────────────────
// 변경 시 config.ini 의 sched_N 항목 수정 후 재빌드 필요
// wday: 0=일 1=월 2=화 3=수 4=목 5=금 6=토 7=평일(월~금)
struct SchedEntry { uint8_t hour, minute, wday; };
static const SchedEntry SCHEDS[] = {
  {8,  20, 1},   // 월요일 08:20
  {11, 30, 7},   // 평일  11:30
  {12, 30, 7},   // 평일  12:30
  {17,  0, 7},   // 평일  17:00
};
static const int SCHED_COUNT = (int)(sizeof(SCHEDS) / sizeof(SCHEDS[0]));

static int           s_beepRemaining = 0;
static unsigned long s_beepNextAt    = 0;

static void triggerDoubleBeep() {
  s_beepRemaining = 2;
  s_beepNextAt    = millis();
}

static void updateDoubleBeep() {
  if (s_beepRemaining <= 0) return;
  if (millis() < s_beepNextAt) return;
  buzz.beep(cfg::BUZ_DURATION_MS);
  --s_beepRemaining;
  s_beepNextAt = millis() + cfg::BUZ_DURATION_MS + 200UL;
}

static void checkSchedule() {
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 120) return;
  static int s_lastFiredMin = -1;
  int nowMin = t.tm_hour * 60 + t.tm_min;
  if (nowMin == s_lastFiredMin) return;
  for (int i = 0; i < SCHED_COUNT; ++i) {
    const SchedEntry& s = SCHEDS[i];
    if (t.tm_hour != s.hour || t.tm_min != s.minute) continue;
    bool dayOk = (s.wday == 7) ? (t.tm_wday >= 1 && t.tm_wday <= 5)
                               : (t.tm_wday == (int)s.wday);
    if (!dayOk) continue;
    Serial.printf("[SCHED] 예약 알림 wday=%d %02d:%02d\n",
                  t.tm_wday, s.hour, s.minute);
    triggerDoubleBeep();
    s_lastFiredMin = nowMin;
    break;
  }
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
static void connectWifi() {
  Serial.printf("[WIFI] %s 연결 중...\n", cfg::WIFI_SSID);
  led.setState(LedState::WHITE_PULSE);

  WiFi.setAutoReconnect(false);   // 자동 재연결 끔 — 수동으로 관리
  WiFi.disconnect(true);          // 이전 연결 + 내부 상태 완전 초기화
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cfg::WIFI_SSID, cfg::WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    led.update();
    delay(50);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] 연결 실패");
    return;
  }
  wifiFailCount = 0;
  Serial.printf("[WIFI] 연결됨  ip=%s  rssi=%d dBm\n",
    WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

// ── NTP 시간 동기화 (한국 표준시 UTC+9, DST 없음) ────────────────────────────
static void syncNtp() {
  configTime(9 * 3600, 0, "pool.ntp.org", "time.google.com");
  Serial.print("[NTP] 동기화 중");
  struct tm t;
  for (int i = 0; i < 20; ++i) {             // 최대 10초
    if (getLocalTime(&t, 500) && t.tm_year >= 120) {
      static const char* const DAYS[] = {"일","월","화","수","목","금","토"};
      Serial.printf("\n[NTP] %04d-%02d-%02d(%s) %02d:%02d KST\n",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        DAYS[t.tm_wday],
        t.tm_hour, t.tm_min);
      return;
    }
    Serial.print(".");
    led.update();
  }
  Serial.println("\n[NTP] 동기화 실패 — 시간 없이 계속 (LED 항상 표시)");
}

// ── 근무시간 판별 ─────────────────────────────────────────────────────────────
// · cfg::WORK_HOURS_ONLY = false 이면 항상 true (24시간 동작)
// · NTP 미동기화(year < 2020) 시 안전하게 true 반환
static bool isWorkingHours() {
  if (!cfg::WORK_HOURS_ONLY) return true;              // 기능 비활성화 → 항상 동작
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 120) return true;  // 미동기화 → 안전값
  if (t.tm_wday == 0 || t.tm_wday == 6) return false;        // 일(0), 토(6)
  int m = t.tm_hour * 60 + t.tm_min;
  return (m >= cfg::WORK_START_MIN) && (m < cfg::WORK_END_MIN);
}

// ── LED 복원 (비프 없이, 근무시간 재진입 시 사용) ────────────────────────────
static void restoreLed(int p) {
  switch (p) {
    case (int)MailPriority::VIP_TO_ME:    led.setState(LedState::RED_BLINK);  break;
    case (int)MailPriority::SOLE_TO_ME:   led.setState(LedState::BLUE_BLINK); break;
    case (int)MailPriority::OTHER_UNREAD: led.setState(LedState::GREEN_SOLID); break;
    default:                               led.setState(LedState::OFF);         break;
  }
}

// ── 우선순위 → LED / 비프 적용 ───────────────────────────────────────────────
static void applyPriority(int p) {
  if (p == lastPriority) return;
  int prev   = lastPriority;
  lastPriority = p;

  // 비근무시간: 우선순위는 기억해두되 LED/비프는 건드리지 않음
  if (!isWorkingHours()) return;

  switch (p) {
    case (int)MailPriority::VIP_TO_ME:
      Serial.println("[LED] RED  (VIP → me)");
      led.setState(LedState::RED_BLINK);
      if (prev != (int)MailPriority::VIP_TO_ME) buzz.beep(cfg::BUZ_DURATION_MS);
      break;
    case (int)MailPriority::SOLE_TO_ME:
      Serial.println("[LED] BLUE (sole recipient)");
      led.setState(LedState::BLUE_BLINK);
      break;
    case (int)MailPriority::OTHER_UNREAD:
      Serial.println("[LED] GREEN (other unread)");
      led.setState(LedState::GREEN_SOLID);
      break;
    case (int)MailPriority::NONE:
      Serial.println("[LED] OFF");
      led.setState(LedState::OFF);
      break;
  }
}

// ── 우선순위 → OLED 갱신 ─────────────────────────────────────────────────────
static void updateDisplay(int p) {
  if (p <= (int)MailPriority::NONE) {
    disp.showIdle();
  } else {
    disp.showEmail(outlook.lastFrom(), outlook.lastIsCC(), outlook.lastSubject());
  }
}

// ── Outlook 폴링 ─────────────────────────────────────────────────────────────
static void doPoll() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!oauth.ensureAccessToken()) {
    // 폴링 중 갱신 실패 → NVS 유지, 다음 폴링 때 재시도
    Serial.println("[POLL] token 갱신 실패 → 건너뜀");
    return;
  }

  int p = outlook.pollHighestPriority(oauth.accessToken());

  if (p == -401) {
    Serial.println("[POLL] 401 → token 강제 갱신 후 재시도");
    if (oauth.ensureAccessToken())
      p = outlook.pollHighestPriority(oauth.accessToken());
  }

  if (p < 0) return;

  bool wasGreenSolid   = (led.state() == LedState::GREEN_SOLID);
  int  prevUnread      = s_prevUnreadCount;
  int  newUnread       = outlook.lastUnreadCount();
  s_prevUnreadCount    = newUnread;

  applyPriority(p);
  updateDisplay(p);

  // GREEN_SOLID 상태에서 읽지 않은 메일이 새로 늘었으면 한 번 깜빡
  if (wasGreenSolid
      && p == (int)MailPriority::OTHER_UNREAD
      && newUnread > prevUnread
      && prevUnread >= 0) {
    led.flashOnce();
  }
}

// ── 비동기 폴링 태스크 (코어 0에서 실행, 메인루프 블로킹 없음) ─────────────────
static void pollTaskFn(void*) {
  int p = -1;
  if (!s_wifiDead && WiFi.status() == WL_CONNECTED) {
    if (!oauth.ensureAccessToken()) {
      Serial.println("[POLL] token 갱신 실패 → 건너뜀");
    } else {
      p = outlook.pollHighestPriority(oauth.accessToken());
      if (p == -401) {
        Serial.println("[POLL] 401 → token 강제 갱신 후 재시도");
        if (oauth.ensureAccessToken())
          p = outlook.pollHighestPriority(oauth.accessToken());
      }
    }
  }
  s_taskPollP = p;
  s_pollDone  = true;   // 메인루프가 결과를 읽도록 알림
  s_pollBusy  = false;  // 다음 태스크 허용
  vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("╔═══════════════════════════════════════════════╗");
  Serial.println("║   Outlook Mail LED — ESP32-S3-N16R8          ║");
  Serial.println("╚═══════════════════════════════════════════════╝");
  Serial.printf("  Flash  : %u MB\n", spi_flash_get_chip_size() >> 20);
  Serial.printf("  PSRAM  : %u KB %s\n",
    ESP.getPsramSize() >> 10, psramFound() ? "OK" : "미감지 ⚠️");
  Serial.printf("  Heap   : %u KB\n", ESP.getFreeHeap() >> 10);
  Serial.printf("  my_email  : %s\n", cfg::MY_EMAIL);
  Serial.printf("  vip senders: %d개\n", cfg::VIP_SENDERS_COUNT);
  for (int i = 0; i < cfg::VIP_SENDERS_COUNT; ++i)
    Serial.printf("    - %s\n", cfg::VIP_SENDERS[i]);
  Serial.printf("  poll      : %lu ms  top_n=%d\n", cfg::POLL_INTERVAL_MS, cfg::TOP_N);

  // OLED 초기화
  disp.begin(cfg::OLED_SDA, cfg::OLED_SCL);

  // LED 자가테스트
  led.begin(cfg::LED_PIN, cfg::LED_BRIGHTNESS, cfg::BLINK_MS);
  led.setState(LedState::CYAN_FLASH);
  unsigned long t0 = millis();
  while (millis() - t0 < 1000) { led.update(); delay(10); }
  led.setState(LedState::OFF);

  // 부저 자가테스트
  buzz.begin(cfg::BUZ_PIN, cfg::BUZ_VOLUME, cfg::BUZ_FREQ);
  buzz.beep(80);
  unsigned long b0 = millis();
  while (millis() - b0 < 200) { buzz.update(); delay(10); }

  // WiFi + NTP
  connectWifi();
  if (WiFi.status() == WL_CONNECTED) syncNtp();

  // OAuth
  if (!oauth.begin()) {
    Serial.println("[OAUTH] refresh_token 없음 → Device Code Flow");
    led.setState(LedState::YELLOW_PULSE);
    if (!oauth.runDeviceCodeFlow([](const char* code){ disp.showAuth(code); })) {
      led.setState(LedState::PURPLE_PULSE);
      delay(5000);
      ESP.restart();
    }
  } else {
    Serial.println("[OAUTH] refresh_token 발견 → access_token 갱신");
    if (!oauth.ensureAccessToken()) {
      // NVS 보존 — 네트워크 일시 오류일 수 있음. 토큰 폐기하지 않고 5분 후 재시도.
      Serial.println("[OAUTH] 갱신 실패 → NVS 유지, 5분 후 재부팅");
      led.setState(LedState::YELLOW_PULSE);
      unsigned long t0 = millis();
      while (millis() - t0 < 300000UL) { led.update(); delay(20); }
      ESP.restart();
    }
  }

  led.setState(LedState::OFF);
  disp.showIdle();

  outlook.dumpDiagnostics(oauth.accessToken());

  Serial.println("[BOOT] 준비 완료 — 폴링 시작");
  doPoll();
  lastPoll = millis();

  // ── 부팅 시 근무시간 초기 상태 확정 (doPoll 이후 실행 — showIdle 덮어쓰기 방지) ─
  if (cfg::WORK_HOURS_ONLY && !isWorkingHours()) {
    Serial.println("[TIME] 비근무시간 부팅 → 초기 상태 적용");
    led.setState(LedState::OFF);
    if (cfg::OLED_OFF_HOURS_OFF) {
      disp.clear();
      Serial.println("[TIME] OLED 소등 (off_hours_oled=off)");
    }
    // idle 설정이면 showIdle()이 이미 호출됐으므로 그대로 유지
  }
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  led.update();
  buzz.update();
  disp.update();
  updateDoubleBeep();
  checkSchedule();

  // ── 힙 부족 → 즉시 재부팅 ──────────────────────────────────────────────────
  if (ESP.getFreeHeap() < 30000) {
    Serial.printf("[MEM] 힙 부족 %u bytes → 재부팅\n", ESP.getFreeHeap());
    delay(500);
    ESP.restart();
  }

  // ── 24시간 정기 재부팅 (누적 단편화 방지) ──────────────────────────────────
  if (millis() > 86400000UL) {
    Serial.println("[SYS] 24시간 정기 재부팅");
    delay(500);
    ESP.restart();
  }

  // ── 근무시간 상태 추적 → LED 소등/복원 (기능 활성화 시만) ─────────────────
  if (cfg::WORK_HOURS_ONLY) {
    // isWorkingHours()로 초기화 → 부팅 시 거짓 전환(false transition) 방지
    static bool prevWorking = isWorkingHours();
    bool working = isWorkingHours();
    if (working != prevWorking) {
      prevWorking = working;
      if (!working) {
        Serial.println("[TIME] 비근무시간 → LED 소등 / 폴링 중단");
        led.setState(LedState::OFF);
        if (cfg::OLED_OFF_HOURS_OFF) {
          disp.clear();
          Serial.println("[TIME] OLED 소등");
        } else {
          disp.showIdle();
          Serial.println("[TIME] OLED 시계 표시 유지");
        }
      } else {
        Serial.println("[TIME] 근무시간 시작 → LED 복원 / 폴링 재개");
        restoreLed(lastPriority);
        disp.showIdle();         // off 상태였어도 즉시 시계로 복원
        triggerDoubleBeep();     // 출근 알림 부저
        lastPoll = 0;            // 10초 대기 없이 즉시 폴링 트리거
      }
    }
  }

  // ── WiFi 감시 (포기 상태이면 스킵) ───────────────────────────────────────
  if (!s_wifiDead && millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("[WIFI] 끊김 → 재연결 (%d/3)\n", wifiFailCount + 1);
      connectWifi();
      if (WiFi.status() == WL_CONNECTED) {
        // 재연결 성공 → LED 즉시 복원
        restoreLed(lastPriority);
      } else {
        if (++wifiFailCount >= 3) {
          Serial.println("[WIFI] 3회 연속 실패 → 재연결 중지 / Orange LED");
          s_wifiDead = true;
          led.setState(LedState::ORANGE_SLOW_BLINK);
          disp.showWifiError();
        }
        // 실패: connectWifi() 가 이미 WHITE_PULSE 설정함
      }
    }
  }

  // ── 비동기 폴링 결과 적용 (태스크 완료 시) ─────────────────────────────────
  if (s_pollDone) {
    s_pollDone = false;
    int p = s_taskPollP;
    if (p >= 0) {
      bool wasGreenSolid = (led.state() == LedState::GREEN_SOLID);
      int  prevUnread    = s_prevUnreadCount;
      int  newUnread     = outlook.lastUnreadCount();
      s_prevUnreadCount  = newUnread;
      applyPriority(p);
      updateDisplay(p);
      if (wasGreenSolid && p == (int)MailPriority::OTHER_UNREAD
          && newUnread > prevUnread && prevUnread >= 0) {
        led.flashOnce();
      }
    }
  }

  // ── 폴링 주기 도래 시 코어 0 태스크로 비동기 실행 ───────────────────────────
  if (!s_wifiDead && !s_pollBusy
      && millis() - lastPoll >= cfg::POLL_INTERVAL_MS) {
    lastPoll = millis();
    if (isWorkingHours()) {
      s_pollBusy  = true;
      s_pollDone  = false;
      s_taskPollP = -2;
      // 코어 1(APP_CPU): 앱 코드 표준 코어 (코어 0은 WiFi 스택 전용)
      // 스택 16KB: mbedTLS SSL 핸드셰이크 최대 소비량 대응
      BaseType_t ok = xTaskCreatePinnedToCore(
          pollTaskFn, "poll", 16384, nullptr, 1, nullptr, 1);
      if (ok != pdPASS) {
        Serial.println("[POLL] 태스크 생성 실패 → 다음 주기 재시도");
        s_pollBusy = false;  // 실패 시 플래그 해제해 다음 폴링 허용
      }
    }
  }

  delay(20);
}
