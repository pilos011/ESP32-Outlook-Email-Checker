// src/main.cpp — Outlook Mail LED 메인

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "config_generated.h"
#include "Settings.h"
#include "Ble.h"
#include "Led.h"
#include "Buzzer.h"
#include "Oauth.h"
#include "Outlook.h"
#include "Display.h"
#include "EventLog.h"
#include <esp_system.h>

// ── WiFi 포기 후 재시도 간격 ─────────────────────────────────────────────────
static constexpr unsigned long WIFI_DEAD_RETRY_MS = 1800000UL;  // 30분

// ── 재부팅 원인 문자열 ────────────────────────────────────────────────────────
static const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_SW:       return "SW";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    // ESP_RST_EXT: ESP32-S3에는 외부 리셋 핀 없음, 이 케이스는 발생하지 않음
    default:               return "UNKNOWN";
  }
}

Led      led;
Buzzer   buzz;
Oauth    oauth;
Outlook  outlook;
Display  disp;

unsigned long lastPoll          = 0;
unsigned long lastWifiCheck     = 0;
unsigned long s_wifiDeadRetryAt = 0;   // 포기 후 30분 재시도 예약 시각
int           lastPriority      = -2;
int           wifiFailCount     = 0;
bool          s_wifiDead        = false;
int           s_prevUnreadCount = -1;

// ── OAuth 연속 실패 카운터 ─────────────────────────────────────────────────────
// RTC RAM: ESP.restart() 후에도 유지, 파워사이클 시 0으로 초기화
// 1~2회 실패 → 5분 후 재부팅(일시 장애 대응)
// 3회 실패   → 재부팅 포기, Orange LED + OLED "OAuth Error" 고정 (s_oauthDead=true)
RTC_DATA_ATTR static int s_oauthFailCount = 0;
static bool s_oauthDead = false;  // true → 폴링·LED·OLED 변경 중단, BLE만 유지

// ── 비동기 폴링 (코어 0 FreeRTOS 태스크) ─────────────────────────────────────
static volatile bool s_pollBusy = false;   // 태스크 실행 중 플래그
static volatile bool s_pollDone = false;   // 태스크 완료 → 메인루프 결과 적용
static int           s_taskPollP = -2;     // 태스크가 기록한 우선순위 결과

// ── BLE 인증 상태 (연결 해제 시 자동 초기화) ──────────────────────────────────
static bool s_bleAuthed   = false;  // AUTH:PIN 명령으로 인증 완료 여부
static bool s_prevBleConn = false;  // 직전 루프의 BLE 연결 상태 (끊김 감지용)

// ── 예약 알림 ─────────────────────────────────────────────────────────────────
// 하드코딩 제거 → Settings::scheds[] 사용 (config.ini 기본값, BLE로 런타임 변경)

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
  // (날짜, 시, 분) 3요소로 중복 방지 — 분(nowMin)만 쓰면 다음 날 같은 시각을 놓침
  static int  s_lastFiredYday = -1;
  static int  s_lastFiredMin  = -1;
  int nowMin  = t.tm_hour * 60 + t.tm_min;
  int nowYday = t.tm_yday + t.tm_year * 366;  // 날짜 식별자 (yday + year 조합)
  // 같은 날의 같은 분이면 재발화 방지, 다른 날이면 허용
  if (nowMin == s_lastFiredMin && nowYday == s_lastFiredYday) return;
  for (int i = 0; i < Settings::schedCount; ++i) {
    const SchedEntry& s = Settings::scheds[i];
    if (t.tm_hour != s.hour || t.tm_min != s.minute) continue;
    bool dayOk = (s.wday == 7) ? (t.tm_wday >= 1 && t.tm_wday <= 5)
                               : (t.tm_wday == (int)s.wday);
    if (!dayOk) continue;
    Serial.printf("[SCHED] 예약 알림 wday=%d %02d:%02d\n",
                  t.tm_wday, s.hour, s.minute);
    char schedDesc[20];
    snprintf(schedDesc, sizeof(schedDesc), "%02d:%02d wday=%d",
             s.hour, s.minute, t.tm_wday);
    EventLog::log("BEEP_SCHED", schedDesc);
    triggerDoubleBeep();
    s_lastFiredMin  = nowMin;
    s_lastFiredYday = nowYday;
    break;
  }
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
// timeoutMs: 부팅 초기 연결 = 30000ms(기본값), loop() 재시도 경로 = 10000ms
static void connectWifi(unsigned long timeoutMs = 30000) {
  Serial.printf("[WIFI] %s 연결 중... (timeout=%lus)\n",
                Settings::wifiSsid, timeoutMs / 1000);
  led.setState(LedState::WHITE_PULSE);

  WiFi.setAutoReconnect(false);   // 자동 재연결 끔 — 수동으로 관리
  WiFi.disconnect(true);          // 이전 연결 + 내부 상태 완전 초기화
  delay(200);
  WiFi.mode(WIFI_STA);
  // BLE와 WiFi 동시 사용 시 모뎀 슬립(true) 필수 — false로 끄면 ESP-IDF가 abort()
  // (10초 폴링 주기에서 슬립 레이턴시는 무시할 수준이므로 성능 영향 없음)
  WiFi.setSleep(true);
  WiFi.begin(Settings::wifiSsid, Settings::wifiPass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    led.update();
    buzz.update();   // 예약 알림 비프 시퀀스 유지
    disp.update();   // OLED 시계 갱신 유지
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
// · NTP 미동기화(year < 2020) 또는 getLocalTime 실패 시:
//   → s_lastKnown(마지막 유효 상태)을 반환하여 전환 억제
//   → false로 초기화 (시간 불명 = 비근무시간 안전값 / 전환 억제 최우선)
// ※ WiFi 재연결 중 WiFi.disconnect(true) 호출 시 LWIP/SNTP 재초기화로
//   getLocalTime이 잠깐 실패할 수 있음. 이때 true를 반환하면 근무시간 시작
//   전이가 거짓 발화되어 부저·LED가 잘못 동작하는 버그를 야기함.
static bool isWorkingHours() {
  if (!cfg::WORK_HOURS_ONLY) return true;   // 기능 비활성화 → 항상 동작
  static bool s_lastKnown = false;          // 마지막으로 확인된 유효 상태 (초기=비근무)
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 120) {
    return s_lastKnown;   // 시간 미확인 → 마지막 유효 상태 유지 (전환 억제)
  }
  if (t.tm_wday == 0 || t.tm_wday == 6) {   // 일(0), 토(6)
    s_lastKnown = false;
    return false;
  }
  int m = t.tm_hour * 60 + t.tm_min;
  s_lastKnown = (m >= Settings::workStartMin) && (m < Settings::workEndMin);
  return s_lastKnown;
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

// ── BLE 명령 처리 ─────────────────────────────────────────────────────────────
static const char* WDAY_NAMES[] = {"일","월","화","수","목","금","토","평일"};

static void handleBleCommand(const String& raw) {
  int   sep  = raw.indexOf(':');
  String cmd  = (sep < 0) ? raw : raw.substring(0, sep);
  String args = (sep < 0) ? ""  : raw.substring(sep + 1);
  cmd.toUpperCase(); cmd.trim();

  // ── AUTH:PIN (인증 — 모든 명령보다 먼저, 인증 없이도 처리) ─────────────────
  if (cmd == "AUTH") {
    if (args == String(cfg::BLE_PIN)) {
      s_bleAuthed = true;
      Ble::send("OK 인증됨");
      Serial.println("[BLE] 인증 성공");
    } else {
      Ble::send("ERR: 잘못된 PIN");
      Serial.println("[BLE] 인증 실패 (잘못된 PIN)");
    }
    return;
  }

  // 인증되지 않은 경우 이하 모든 명령 차단
  if (!s_bleAuthed) {
    Ble::send("ERR: 인증 필요 → AUTH:PIN번호");
    return;
  }

  // ── STATUS ───────────────────────────────────────────────────────────────
  if (cmd == "STATUS") {
    char buf[64];
    Ble::send("=== 현재 설정 ===");
    Ble::send(String("WiFi: ") + Settings::wifiSsid);
    snprintf(buf, sizeof(buf), "밝기:%d 점멸:%dms 폴링:%ds",
             Settings::brightness, Settings::blinkMs, Settings::pollSec);
    Ble::send(buf);
    snprintf(buf, sizeof(buf), "근무:%02d:%02d~%02d:%02d",
             Settings::workStartMin/60, Settings::workStartMin%60,
             Settings::workEndMin/60,   Settings::workEndMin%60);
    Ble::send(buf);
    Ble::send(String("VIP(") + Settings::vipCount + "):");
    for (int i = 0; i < Settings::vipCount; ++i)
      Ble::send(String(" [") + i + "] " + Settings::vip[i]);
    Ble::send(String("예약(") + Settings::schedCount + "):");
    for (int i = 0; i < Settings::schedCount; ++i) {
      auto& s = Settings::scheds[i];
      snprintf(buf, sizeof(buf), " [%d] %02d:%02d %s",
               i, s.hour, s.minute, WDAY_NAMES[s.wday <= 7 ? s.wday : 7]);
      Ble::send(buf);
    }
    Ble::send("=================");
    return;
  }

  // ── RESET — NVS 초기화 → 재부팅 ─────────────────────────────────────────
  if (cmd == "RESET") {
    Ble::send("NVS 초기화 → 재부팅");
    Settings::resetToDefaults();
    delay(500);
    ESP.restart();
    return;
  }

  // ── REBOOT ───────────────────────────────────────────────────────────────
  if (cmd == "REBOOT") {
    Ble::send("재부팅");
    delay(300);
    ESP.restart();
    return;
  }

  // ── WIFI:ssid:pass ────────────────────────────────────────────────────────
  if (cmd == "WIFI") {
    int s2 = args.indexOf(':');
    if (s2 < 1) { Ble::send("ERR: WIFI:ssid:pass"); return; }
    String ssid = args.substring(0, s2);
    String pass = args.substring(s2 + 1);
    ssid.trim();
    if (ssid.length() == 0) { Ble::send("ERR: ssid 비어있음"); return; }
    strncpy(Settings::wifiSsid, ssid.c_str(), sizeof(Settings::wifiSsid) - 1);
    strncpy(Settings::wifiPass, pass.c_str(), sizeof(Settings::wifiPass) - 1);
    Settings::saveAll();
    Ble::send("OK 저장 → 재연결 중...");
    s_wifiDead    = false;
    wifiFailCount = 0;
    connectWifi();
    if (WiFi.status() == WL_CONNECTED) {
      restoreLed(lastPriority);
      Ble::send("OK 연결됨: " + WiFi.localIP().toString());
    } else {
      Ble::send("ERR: WiFi 연결 실패");
    }
    return;
  }

  // ── BRIGHT:n (0~255, 즉시 적용) ───────────────────────────────────────────
  if (cmd == "BRIGHT") {
    // 빈 문자열이나 비숫자 → toInt()=0으로 잘못 적용되는 버그 방지 (SCHED:del 동일 패턴)
    bool allDigits = args.length() > 0;
    for (char c : args) if (!isdigit((unsigned char)c)) { allDigits = false; break; }
    if (!allDigits) { Ble::send("ERR: BRIGHT:0~255"); return; }
    int v = args.toInt();
    if (v > 255) { Ble::send("ERR: 0~255"); return; }
    Settings::brightness = v;
    Settings::saveAll();
    led.setBrightness((uint8_t)v);
    Ble::send("OK brightness=" + String(v));
    return;
  }

  // ── BLINK:ms (100~5000, 즉시 적용) ───────────────────────────────────────
  if (cmd == "BLINK") {
    int v = args.toInt();
    if (v < 100 || v > 5000) { Ble::send("ERR: 100~5000ms"); return; }
    Settings::blinkMs = v;
    Settings::saveAll();
    led.setBlinkMs((uint16_t)v);
    Ble::send("OK blink=" + String(v) + "ms");
    return;
  }

  // ── POLL:sec (5~3600, 즉시 적용) ─────────────────────────────────────────
  if (cmd == "POLL") {
    int v = args.toInt();
    if (v < 5 || v > 3600) { Ble::send("ERR: 5~3600"); return; }
    Settings::pollSec = v;
    Settings::saveAll();
    Ble::send("OK poll=" + String(v) + "s");
    return;
  }

  // ── WORK:HHMM:HHMM (즉시 적용) ───────────────────────────────────────────
  if (cmd == "WORK") {
    int s2 = args.indexOf(':');
    if (s2 != 4 || (int)args.length() < 9) { Ble::send("ERR: WORK:0730:1700"); return; }
    int sh = args.substring(0,2).toInt(), sm2 = args.substring(2,4).toInt();
    int eh = args.substring(5,7).toInt(), em2 = args.substring(7,9).toInt();
    if (sh < 0 || sm2 < 0 || eh < 0 || em2 < 0
        || sh > 23 || sm2 > 59 || eh > 23 || em2 > 59) { Ble::send("ERR: 시/분 범위 초과 (0730~2359)"); return; }
    int sm = sh * 60 + sm2;
    int em = eh * 60 + em2;
    if (sm >= em) { Ble::send("ERR: start >= end"); return; }
    Settings::workStartMin = sm;
    Settings::workEndMin   = em;
    Settings::saveAll();
    char buf[40];
    snprintf(buf, sizeof(buf), "OK 근무:%02d:%02d~%02d:%02d", sm/60,sm%60, em/60,em%60);
    Ble::send(buf);
    return;
  }

  // ── VIP:list / VIP:add:email / VIP:del:email ─────────────────────────────
  if (cmd == "VIP") {
    int s2 = args.indexOf(':');
    String sub = (s2 < 0) ? args : args.substring(0, s2);
    String val = (s2 < 0) ? ""   : args.substring(s2 + 1);
    sub.toUpperCase(); sub.trim();

    if (sub == "LIST") {
      if (Settings::vipCount == 0) { Ble::send("(VIP 없음)"); return; }
      for (int i = 0; i < Settings::vipCount; ++i)
        Ble::send(String("[") + i + "] " + Settings::vip[i]);
    } else if (sub == "ADD") {
      val.trim(); val.toLowerCase();
      if (Settings::addVip(val.c_str())) {
        Settings::saveAll();
        Ble::send("OK VIP추가: " + val);
      } else {
        Ble::send("ERR: 중복이거나 최대(" + String(SET_MAX_VIPS) + ")");
      }
    } else if (sub == "DEL") {
      val.trim(); val.toLowerCase();
      if (Settings::delVip(val.c_str())) {
        Settings::saveAll();
        Ble::send("OK VIP제거: " + val);
      } else {
        Ble::send("ERR: 없는 주소");
      }
    } else {
      Ble::send("ERR: VIP:list / VIP:add:email / VIP:del:email");
    }
    return;
  }

  // ── SCHED:list / SCHED:add:HHMM:w / SCHED:del:n / SCHED:clear ───────────
  if (cmd == "SCHED") {
    int s2 = args.indexOf(':');
    String sub = (s2 < 0) ? args : args.substring(0, s2);
    String val = (s2 < 0) ? ""   : args.substring(s2 + 1);
    sub.toUpperCase(); sub.trim();

    if (sub == "LIST") {
      if (Settings::schedCount == 0) { Ble::send("(예약 없음)"); return; }
      for (int i = 0; i < Settings::schedCount; ++i) {
        auto& s = Settings::scheds[i];
        char buf[32];
        snprintf(buf, sizeof(buf), "[%d] %02d:%02d %s",
                 i, s.hour, s.minute, WDAY_NAMES[s.wday <= 7 ? s.wday : 7]);
        Ble::send(buf);
      }
    } else if (sub == "ADD") {
      // val = "HHMM:w"
      int s3 = val.indexOf(':');
      if (s3 != 4 || (int)val.length() < 6) { Ble::send("ERR: SCHED:add:HHMM:w"); return; }
      int h = val.substring(0,2).toInt(), m = val.substring(2,4).toInt();
      int w = val.substring(5).toInt();
      if (h > 23 || m > 59 || w > 7) { Ble::send("ERR: 시간/요일 범위"); return; }
      if (Settings::addSched(h, m, w)) {
        Settings::saveAll();
        char buf[32];
        snprintf(buf, sizeof(buf), "OK 추가:%02d:%02d %s", h, m, WDAY_NAMES[w]);
        Ble::send(buf);
      } else {
        Ble::send("ERR: 최대 " + String(SET_MAX_SCHEDS) + "개");
      }
    } else if (sub == "DEL") {
      val.trim();
      // 빈 문자열이나 비숫자 → toInt()=0 으로 index 0을 잘못 삭제하는 버그 방지
      bool allDigits = val.length() > 0;
      for (char c : val) if (!isdigit((unsigned char)c)) { allDigits = false; break; }
      if (!allDigits) { Ble::send("ERR: SCHED:del:숫자 (예: SCHED:del:0)"); }
      else {
        int idx = val.toInt();
        if (Settings::delSched(idx)) {
          Settings::saveAll();
          Ble::send("OK 삭제:[" + String(idx) + "]");
        } else if (Settings::schedCount == 0) {
          Ble::send("ERR: (예약 없음)");
        } else {
          Ble::send("ERR: 잘못된 인덱스 (0~" + String(Settings::schedCount - 1) + ")");
        }
      }
    } else if (sub == "CLEAR") {
      Settings::clearScheds();
      Settings::saveAll();
      Ble::send("OK 예약 전체 삭제");
    } else {
      Ble::send("ERR: SCHED:list/add:HHMM:w/del:n/clear");
    }
    return;
  }

  // ── LOG / LOG:clear ──────────────────────────────────────────────────────
  if (cmd == "LOG") {
    if (args.equalsIgnoreCase("CLEAR")) {
      EventLog::clear();
      Ble::send("OK 로그 초기화");
    } else {
      EventLog::sendViaBle();
    }
    return;
  }

  Ble::send("ERR: 모르는 명령 — STATUS 로 현재 설정 확인");
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(800);
  EventLog::begin();                          // NVS 로그 상태 로드 (가장 먼저)
  EventLog::log("BOOT", resetReasonStr());    // 재부팅 원인 기록
  Settings::init();   // NVS → config.ini 기본값 로드 (하드웨어 초기화 전)
  Serial.println();
  Serial.println("╔═══════════════════════════════════════════════╗");
  Serial.println("║   Outlook Mail LED — ESP32-S3-N16R8          ║");
  Serial.println("╚═══════════════════════════════════════════════╝");
  Serial.printf("  Flash  : %u MB\n", spi_flash_get_chip_size() >> 20);
  Serial.printf("  PSRAM  : %u KB %s\n",
    ESP.getPsramSize() >> 10, psramFound() ? "OK" : "미감지 ⚠️");
  Serial.printf("  Heap   : %u KB\n", ESP.getFreeHeap() >> 10);
  Serial.printf("  my_email  : %s\n", cfg::MY_EMAIL);
  Serial.printf("  vip senders: %d개\n", Settings::vipCount);
  for (int i = 0; i < Settings::vipCount; ++i)
    Serial.printf("    - %s\n", Settings::vip[i]);
  Serial.printf("  poll      : %d s  top_n=%d\n", Settings::pollSec, cfg::TOP_N);

  // OLED 초기화
  disp.begin(cfg::OLED_SDA, cfg::OLED_SCL);

  // LED 자가테스트
  led.begin(cfg::LED_PIN, Settings::brightness, Settings::blinkMs);
  led.setState(LedState::CYAN_FLASH);
  unsigned long t0 = millis();
  while (millis() - t0 < 1000) { led.update(); delay(10); }
  led.setState(LedState::OFF);

  // 부저 자가테스트
  buzz.begin(cfg::BUZ_PIN, cfg::BUZ_VOLUME, cfg::BUZ_FREQ);
  buzz.beep(80);
  unsigned long b0 = millis();
  while (millis() - b0 < 200) { buzz.update(); delay(10); }

  // BLE NUS 설정 콘솔 (WiFi 연결 전 시작 — 자격증명 변경 가능)
  Ble::begin("OutlookLED");

  // WiFi + NTP
  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    EventLog::log("WIFI_OK", WiFi.localIP().toString().c_str());
    syncNtp();
  } else {
    EventLog::log("WIFI_FAIL");
  }

  // OAuth
  if (!oauth.begin()) {
    Serial.println("[OAUTH] refresh_token 없음 → Device Code Flow");
    led.setState(LedState::YELLOW_PULSE);
    disp.showConnecting();   // Microsoft 서버 연결 전 OLED 피드백
    if (!oauth.runDeviceCodeFlow([](const char* code){ disp.showAuth(code); })) {
      led.setState(LedState::PURPLE_PULSE);
      delay(5000);
      ESP.restart();
    }
  } else {
    Serial.println("[OAUTH] refresh_token 발견 → access_token 갱신");
    if (!oauth.ensureAccessToken()) {
      Serial.println("[OAUTH] 갱신 실패");
      if (!oauth.hasRefreshToken()) {
        // invalid_grant 등으로 refresh_token 삭제됨 → 재시도 무의미
        // OLED에 만료 안내 표시 후 즉시 Device Code Flow 재실행
        Serial.println("[OAUTH] refresh_token 무효 → 재인증 안내 → Device Code Flow");
        EventLog::log("OAUTH_REAUTH");
        s_oauthFailCount = 0;
        led.setState(LedState::ORANGE_SLOW_BLINK);
        disp.showReauth();                           // "MFA Expired / Re-auth needed"
        unsigned long tReauth = millis();
        while (millis() - tReauth < 3000) { led.update(); delay(20); }
        led.setState(LedState::YELLOW_PULSE);
        disp.showConnecting();                       // "OAuth setup... / Connecting to MS"
        if (!oauth.runDeviceCodeFlow([](const char* code){ disp.showAuth(code); })) {
          led.setState(LedState::PURPLE_PULSE);
          delay(5000);
          ESP.restart();
        }
      } else {
        s_oauthFailCount++;
        Serial.printf("[OAUTH] 갱신 실패 (%d/3)\n", s_oauthFailCount);
        if (s_oauthFailCount >= 3) {
          // 3회 연속 실패 → 재부팅 포기
          s_oauthFailCount = 0;   // 카운터 초기화 (다음 재부팅에서 새로 3회 시도)
          Serial.println("[OAUTH] 3회 연속 실패 → 포기 / Orange LED / OLED 에러");
          EventLog::log("OAUTH_DEAD");
          s_oauthDead = true;
          led.setState(LedState::ORANGE_SLOW_BLINK);
          disp.showOAuthError();
          // setup()을 완료하고 loop()로 진입
          // BLE REBOOT 명령 또는 파워사이클로 복구
        } else {
          // 일시 장애 가능성 → 5분 대기 후 재부팅
          Serial.printf("[OAUTH] NVS 유지, 5분 후 재부팅 (%d/3)\n", s_oauthFailCount);
          EventLog::log("REBOOT_OAUTH");
          led.setState(LedState::YELLOW_PULSE);
          disp.showTokenRetry(s_oauthFailCount, 3);  // "Token refresh fail / Retry N/3 in 5min"
          unsigned long t0 = millis();
          while (millis() - t0 < 300000UL) { led.update(); delay(20); }
          ESP.restart();
        }
      }
    }
  }

  if (!s_oauthDead) {
    // OAuth 정상 → 일반 부팅 완료 처리
    led.setState(LedState::OFF);
    disp.showIdle();

    outlook.dumpDiagnostics(oauth.accessToken());

    Serial.println("[BOOT] 준비 완료 — 폴링 시작");
    EventLog::log("BOOT_OK");
    doPoll();
    lastPoll = millis();

    // ── 부팅 시 근무시간 초기 상태 확정 (doPoll 이후 실행) ────────────────────
    // doPoll()이 메일을 찾으면 showEmail()을 호출했을 수 있으므로
    // 비근무시간이면 OLED를 반드시 올바른 상태로 덮어씀
    if (cfg::WORK_HOURS_ONLY && !isWorkingHours()) {
      Serial.println("[TIME] 비근무시간 부팅 → 초기 상태 적용");
      led.setState(LedState::OFF);
      if (cfg::OLED_OFF_HOURS_OFF) {
        disp.clear();
        Serial.println("[TIME] OLED 소등 (off_hours_oled=off)");
      } else {
        disp.showIdle();   // doPoll이 showEmail()을 호출했어도 시계로 복원
        Serial.println("[TIME] OLED 시계 표시");
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  led.update();
  buzz.update();
  disp.update();
  updateDoubleBeep();
  checkSchedule();

  // ── BLE 연결 해제 감지 → 인증 초기화 ────────────────────────────────────────
  {
    bool nowConn = Ble::isConnected();
    if (s_prevBleConn && !nowConn) {
      s_bleAuthed = false;
      Serial.println("[BLE] 연결 끊김 → 인증 초기화");
    }
    s_prevBleConn = nowConn;
  }

  // ── BLE 명령 처리 ────────────────────────────────────────────────────────────
  if (Ble::hasPendingCommand()) handleBleCommand(Ble::dequeueCommand());

  // ── 힙 부족 → 즉시 재부팅 ──────────────────────────────────────────────────
  if (ESP.getFreeHeap() < 30000) {
    char heapBuf[20];
    snprintf(heapBuf, sizeof(heapBuf), "heap=%uB", ESP.getFreeHeap());
    Serial.printf("[MEM] 힙 부족 %s → 재부팅\n", heapBuf);
    // NVS 쓰기에 최소 ~4KB 연속 블록 필요 → 단편화 심하면 write 실패 가능
    // getMaxAllocHeap()으로 실제 연속 가용 블록 확인 후 로그 기록
    if (ESP.getMaxAllocHeap() >= 8192) {
      EventLog::log("REBOOT_HEAP", heapBuf);
    } else {
      Serial.println("[MEM] 힙 단편화 심각 — EventLog 쓰기 생략");
    }
    delay(500);
    ESP.restart();
  }

  // ── 24시간 정기 재부팅 (누적 단편화 방지) ──────────────────────────────────
  // 근무시간 중 재부팅 방지: 비근무시간까지 대기
  // · 24h 경과 + 비근무시간 → 즉시 재부팅
  // · 25h 경과 → NTP 미동기화 등 예외 상황 대비 강제 재부팅
  if (millis() > 86400000UL) {
    if (!isWorkingHours() || millis() > 90000000UL) {
      char uptimeBuf[20];
      snprintf(uptimeBuf, sizeof(uptimeBuf), "up=%.1fh", millis() / 3600000.0f);
      Serial.printf("[SYS] 24시간 정기 재부팅 (%s)\n", uptimeBuf);
      EventLog::log("REBOOT_24H", uptimeBuf);
      delay(500);
      ESP.restart();
    }
  }

  // ── 근무시간 상태 추적 → LED 소등/복원 (기능 활성화 시만) ─────────────────
  if (cfg::WORK_HOURS_ONLY) {
    // isWorkingHours()로 초기화 → 부팅 시 거짓 전환(false transition) 방지
    static bool prevWorking = isWorkingHours();
    bool working = isWorkingHours();
    if (working != prevWorking) {
      prevWorking = working;
      if (!s_oauthDead) {   // OAuth 에러 상태: Orange LED/OLED 고정 유지
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
          // 실제 발화 시각을 로그에 포함 — 거짓 발화 시 07:30 이전 시각이 기록되어 진단 가능
          char workBuf[12] = "??:??";
          struct tm twk;
          if (getLocalTime(&twk, 0) && twk.tm_year >= 120)
            snprintf(workBuf, sizeof(workBuf), "%02d:%02d", twk.tm_hour, twk.tm_min);
          EventLog::log("WORK_START", workBuf);
          restoreLed(lastPriority);
          // LED와 OLED를 동시에 lastPriority 기준으로 복원
          // · 메일 있으면 → 이메일 화면 (disp.showIdle() 대신 updateDisplay 사용)
          // · 메일 없으면 → 시계 화면
          // 폴링 완료 전에도 LED/OLED가 일치한 상태를 유지하고,
          // 폴링 실패 시에도 마지막 알려진 상태를 표시함
          updateDisplay(lastPriority);
          triggerDoubleBeep();     // 출근 알림 부저
          lastPoll = 0;            // 10초 대기 없이 즉시 폴링 트리거
        }
      }
    }
  }

  // ── WiFi 감시 ────────────────────────────────────────────────────────────
  if (!s_wifiDead) {
    // 정상 감시: 5초마다 연결 상태 확인
    if (millis() - lastWifiCheck > 5000) {
      lastWifiCheck = millis();
      if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WIFI] 끊김 → 재연결 (%d/3)\n", wifiFailCount + 1);
        connectWifi();
        if (WiFi.status() == WL_CONNECTED) {
          // 재연결 성공
          if (isWorkingHours()) {
            restoreLed(lastPriority);
          } else {
            // 비근무시간: connectWifi()가 WHITE_PULSE로 바꾼 LED를 OFF로 되돌림
            led.setState(LedState::OFF);
          }
        } else {
          if (++wifiFailCount >= 3) {
            Serial.println("[WIFI] 3회 연속 실패 → 포기 / Orange LED / 30분 후 재시도 예약");
            EventLog::log("WIFI_DEAD");
            s_wifiDead        = true;
            s_wifiDeadRetryAt = millis() + WIFI_DEAD_RETRY_MS;
            led.setState(LedState::ORANGE_SLOW_BLINK);
            disp.showWifiError();
          }
          // 실패: connectWifi() 가 이미 WHITE_PULSE 설정함
        }
      }
    }
  } else {
    // 포기 상태: 30분마다 한 번 재시도 (단발 시도, 실패해도 3회 카운트 없음)
    if (millis() >= s_wifiDeadRetryAt) {
      Serial.println("[WIFI] 30분 재시도 중...");
      connectWifi(10000);   // 재시도: 10초 타임아웃 (부팅 30초보다 짧게, loop() 블로킹 최소화)
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[WIFI] 재연결 성공 → 정상 복귀");
        EventLog::log("WIFI_BACK", WiFi.localIP().toString().c_str());
        s_wifiDead    = false;
        wifiFailCount = 0;
        // 근무시간 여부에 따라 LED·OLED 복원
        if (isWorkingHours()) {
          restoreLed(lastPriority);
          updateDisplay(lastPriority);
        } else {
          // 비근무시간: Orange LED + WiFiError 화면 해제 → 정상 비근무 상태로 복원
          led.setState(LedState::OFF);
          if (cfg::OLED_OFF_HOURS_OFF) disp.clear();
          else                         disp.showIdle();
        }
      } else {
        Serial.println("[WIFI] 재시도 실패 → 30분 후 재시도");
        s_wifiDeadRetryAt = millis() + WIFI_DEAD_RETRY_MS;  // 30분 후 재예약
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
      applyPriority(p);   // 내부에서 isWorkingHours() 체크 → 비근무시간엔 LED 변경 안 함
      // updateDisplay는 별도 guard 필요:
      // WORK_END가 disp.showIdle()을 호출한 직후 s_pollDone이 처리되면
      // updateDisplay(p)가 showEmail()로 덮어써 이메일 화면이 남는 버그 방지
      if (isWorkingHours()) {
        updateDisplay(p);
        if (wasGreenSolid && p == (int)MailPriority::OTHER_UNREAD
            && newUnread > prevUnread && prevUnread >= 0) {
          led.flashOnce();
        }
      }
    }
  }

  // ── 폴링 주기 도래 시 코어 0 태스크로 비동기 실행 ───────────────────────────
  if (!s_wifiDead && !s_oauthDead && !s_pollBusy
      && millis() - lastPoll >= (unsigned long)Settings::pollSec * 1000UL) {
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
        // 힙 단편화 등으로 태스크 생성 실패 → 동기 폴백으로 즉시 실행
        // (메인루프가 수 초 멈추지만 메일 감지 누락보다 낫다)
        Serial.printf("[POLL] 태스크 생성 실패 (maxAlloc=%u) → 동기 폴백\n",
                      ESP.getMaxAllocHeap());
        doPoll();
        s_pollBusy = false;
      }
    }
  }

  delay(20);
}
