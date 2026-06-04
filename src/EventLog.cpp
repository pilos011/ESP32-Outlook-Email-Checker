// src/EventLog.cpp — NVS 순환 이벤트 로그
#include "EventLog.h"
#include "Ble.h"
#include <Preferences.h>
#include <time.h>

static const char* EV_NS  = "evlog";
static const int   EV_MAX = 30;           // 최대 보관 이벤트 수

static Preferences _prefs;
static int  _count  = 0;   // 현재 저장된 이벤트 수 (0~EV_MAX)
static int  _head   = 0;   // 가장 오래된 항목의 슬롯 인덱스 (순환 포인터)
static bool _loaded = false;

// ── 내부: NVS에서 포인터 로드 ────────────────────────────────────────────────
static void _load() {
  if (_loaded) return;
  _prefs.begin(EV_NS, false);
  _count  = _prefs.getInt("n", 0);
  _head   = _prefs.getInt("h", 0);
  _prefs.end();
  _loaded = true;
}

// ── 내부: 현재 시각 문자열 ────────────────────────────────────────────────────
static void _timestamp(char* buf, size_t len) {
  struct tm t;
  if (getLocalTime(&t, 0) && t.tm_year >= 120) {
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    strncpy(buf, "-------- --:--:--", len);
  }
}

// ── 공개 인터페이스 ───────────────────────────────────────────────────────────
namespace EventLog {

void begin() { _load(); }

void log(const char* tag, const char* detail) {
  _load();

  char ts[24];
  _timestamp(ts, sizeof(ts));

  char entry[88];
  if (detail && detail[0])
    snprintf(entry, sizeof(entry), "%s %-14s %s", ts, tag, detail);
  else
    snprintf(entry, sizeof(entry), "%s %s", ts, tag);

  // 순환 쓰기: 꽉 찼으면 가장 오래된 슬롯 덮어씀
  int slot;
  if (_count < EV_MAX) {
    slot = (_head + _count) % EV_MAX;
    _count++;
  } else {
    slot = _head;
    _head = (_head + 1) % EV_MAX;
  }

  char key[5];
  snprintf(key, sizeof(key), "e%02d", slot);

  _prefs.begin(EV_NS, false);
  _prefs.putString(key, entry);
  _prefs.putInt("n", _count);
  _prefs.putInt("h", _head);
  _prefs.end();

  Serial.printf("[LOG] %s\n", entry);
}

void sendViaBle() {
  _load();
  _prefs.begin(EV_NS, true);

  char hdr[40];
  snprintf(hdr, sizeof(hdr), "=== 이벤트 로그 (%d/%d건) ===", _count, EV_MAX);
  Ble::send(hdr);

  for (int i = 0; i < _count; i++) {
    int slot = (_head + i) % EV_MAX;
    char key[5];
    snprintf(key, sizeof(key), "e%02d", slot);
    String val = _prefs.getString(key, "");
    if (val.length()) {
      char idx[6];
      snprintf(idx, sizeof(idx), "[%2d] ", i + 1);
      Ble::send(String(idx) + val);
    }
    delay(25);   // BLE 전송 간격 (100바이트 청크 × 15ms + 여유)
  }

  Ble::send("================================");
  _prefs.end();
}

void clear() {
  _prefs.begin(EV_NS, false);
  _prefs.clear();
  _prefs.end();
  _count = 0;
  _head  = 0;
  Serial.println("[LOG] 이벤트 로그 초기화");
}

} // namespace EventLog
