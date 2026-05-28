// src/Settings.cpp — 런타임 설정 NVS 로드/저장
#include "Settings.h"
#include "config_generated.h"
#include <Preferences.h>

static const char* NVS_NS = "app_cfg";   // NVS 네임스페이스 (최대 15자)
static Preferences _prefs;

namespace Settings {

char wifiSsid[64];
char wifiPass[64];
int  brightness;
int  blinkMs;
int  pollSec;
int  workStartMin;
int  workEndMin;
char vip[SET_MAX_VIPS][SET_VIPLEN];
int  vipCount;
SchedEntry scheds[SET_MAX_SCHEDS];
int        schedCount;

// ── 내부 헬퍼 ──────────────────────────────────────────────────────────────

static void _parseVip(const String& csv) {
  vipCount = 0;
  if (csv.length() == 0) return;
  int start = 0;
  for (int i = 0; i <= (int)csv.length() && vipCount < SET_MAX_VIPS; ++i) {
    if (i == (int)csv.length() || csv[i] == ',') {
      String tok = csv.substring(start, i);
      tok.trim(); tok.toLowerCase();
      if (tok.length() > 0 && tok.length() < SET_VIPLEN) {
        strncpy(vip[vipCount], tok.c_str(), SET_VIPLEN - 1);
        vip[vipCount][SET_VIPLEN - 1] = '\0';
        ++vipCount;
      }
      start = i + 1;
    }
  }
}

static String _vipToCsv() {
  String s;
  for (int i = 0; i < vipCount; ++i) {
    if (i) s += ',';
    s += vip[i];
  }
  return s;
}

// 형식: "HHMM:w"  (예: "0820:1", "1130:7")
static String _schedStr(const SchedEntry& e) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%02d%02d:%d", e.hour, e.minute, e.wday);
  return String(buf);
}

static bool _parseSched(const String& s, SchedEntry& e) {
  // "HHMM:w" — 콜론 위치가 반드시 4
  int sep = s.indexOf(':');
  if (sep != 4 || (int)s.length() < 6) return false;
  int h = s.substring(0, 2).toInt();
  int m = s.substring(2, 4).toInt();
  int w = s.substring(5).toInt();
  if (h > 23 || m > 59 || w > 7) return false;
  e.hour = (uint8_t)h;
  e.minute = (uint8_t)m;
  e.wday   = (uint8_t)w;
  return true;
}

// ── init ───────────────────────────────────────────────────────────────────

void init() {
  // 1단계: config.ini 컴파일 기본값
  strncpy(wifiSsid, cfg::WIFI_SSID, sizeof(wifiSsid) - 1);
  strncpy(wifiPass, cfg::WIFI_PASS, sizeof(wifiPass) - 1);
  brightness   = cfg::LED_BRIGHTNESS;
  blinkMs      = cfg::BLINK_MS;
  pollSec      = (int)(cfg::POLL_INTERVAL_MS / 1000UL);
  workStartMin = cfg::WORK_START_MIN;
  workEndMin   = cfg::WORK_END_MIN;

  vipCount = 0;
  for (int i = 0; i < cfg::VIP_SENDERS_COUNT && i < SET_MAX_VIPS; ++i) {
    strncpy(vip[i], cfg::VIP_SENDERS[i], SET_VIPLEN - 1);
    vip[i][SET_VIPLEN - 1] = '\0';
    ++vipCount;
  }

  schedCount = cfg::SCHED_COUNT_DEFAULT;
  if (schedCount > SET_MAX_SCHEDS) schedCount = SET_MAX_SCHEDS;  // 배열 초과 방지
  for (int i = 0; i < schedCount; ++i) {
    scheds[i] = { cfg::SCHED_HOUR_DEFAULT[i],
                  cfg::SCHED_MIN_DEFAULT[i],
                  cfg::SCHED_WDAY_DEFAULT[i] };
  }

  // 2단계: NVS 오버라이드 (키가 존재할 때만)
  // readOnly=false: 최초 부팅 시 네임스페이스가 없어도 자동 생성 (에러 로그 방지)
  // readOnly=true는 네임스페이스 없을 때 nvs_open NOT_FOUND 에러를 출력함
  if (!_prefs.begin(NVS_NS, false)) {
    Serial.println("[CFG] NVS 오픈 실패 — 기본값 사용");
    return;
  }

  String s;
  s = _prefs.getString("wifi_ssid", "");
  if (s.length()) strncpy(wifiSsid, s.c_str(), sizeof(wifiSsid) - 1);
  s = _prefs.getString("wifi_pass", "");
  if (s.length()) strncpy(wifiPass, s.c_str(), sizeof(wifiPass) - 1);

  int v;
  v = _prefs.getInt("brightness", -1); if (v >= 0 && v <= 255)   brightness   = v;
  v = _prefs.getInt("blink_ms",   -1); if (v >= 100 && v <= 5000) blinkMs     = v;
  v = _prefs.getInt("poll_sec",   -1); if (v >= 5 && v <= 3600)  pollSec      = v;
  v = _prefs.getInt("work_start", -1); if (v >= 0 && v < 1440)   workStartMin = v;
  v = _prefs.getInt("work_end",   -1); if (v >= 0 && v < 1440)   workEndMin   = v;
  // start >= end 이면 config.ini 기본값 복원
  if (workEndMin <= workStartMin) {
    workStartMin = cfg::WORK_START_MIN;
    workEndMin   = cfg::WORK_END_MIN;
  }

  // VIP: isKey()로 "의도적 빈 목록" vs "미설정" 구분
  if (_prefs.isKey("vip_list"))
    _parseVip(_prefs.getString("vip_list", ""));

  // 스케줄: isKey()로 "의도적 0개" vs "미설정" 구분
  if (_prefs.isKey("sched_count")) {
    int sc = _prefs.getInt("sched_count", 0);
    schedCount = 0;
    for (int i = 0; i < sc && i < SET_MAX_SCHEDS; ++i) {
      String key = String("sched_") + i;
      String val = _prefs.getString(key.c_str(), "");
      SchedEntry e;
      if (val.length() && _parseSched(val, e))
        scheds[schedCount++] = e;
    }
  }

  _prefs.end();

  Serial.printf("[CFG] WiFi=%s  bright=%d  blink=%dms  poll=%ds\n",
      wifiSsid, brightness, blinkMs, pollSec);
  Serial.printf("[CFG] work=%02d:%02d~%02d:%02d  VIP=%d  scheds=%d\n",
      workStartMin/60, workStartMin%60,
      workEndMin/60,   workEndMin%60,
      vipCount, schedCount);
}

// ── saveAll ────────────────────────────────────────────────────────────────

void saveAll() {
  _prefs.begin(NVS_NS, false);   // read-write
  _prefs.putString("wifi_ssid",  wifiSsid);
  _prefs.putString("wifi_pass",  wifiPass);
  _prefs.putInt   ("brightness", brightness);
  _prefs.putInt   ("blink_ms",   blinkMs);
  _prefs.putInt   ("poll_sec",   pollSec);
  _prefs.putInt   ("work_start", workStartMin);
  _prefs.putInt   ("work_end",   workEndMin);
  _prefs.putString("vip_list",   _vipToCsv());
  _prefs.putInt   ("sched_count", schedCount);
  for (int i = 0; i < schedCount; ++i) {
    String key = String("sched_") + i;
    _prefs.putString(key.c_str(), _schedStr(scheds[i]));
  }
  _prefs.end();
}

void resetToDefaults() {
  _prefs.begin(NVS_NS, false);
  _prefs.clear();
  _prefs.end();
}

// ── VIP 헬퍼 ───────────────────────────────────────────────────────────────

bool addVip(const char* email) {
  if (vipCount >= SET_MAX_VIPS) return false;
  for (int i = 0; i < vipCount; ++i)
    if (strcasecmp(vip[i], email) == 0) return false;   // 중복
  strncpy(vip[vipCount], email, SET_VIPLEN - 1);
  vip[vipCount][SET_VIPLEN - 1] = '\0';
  ++vipCount;
  return true;
}

bool delVip(const char* email) {
  for (int i = 0; i < vipCount; ++i) {
    if (strcasecmp(vip[i], email) == 0) {
      for (int j = i; j < vipCount - 1; ++j)
        memcpy(vip[j], vip[j + 1], SET_VIPLEN);
      --vipCount;
      return true;
    }
  }
  return false;
}

// ── 스케줄 헬퍼 ────────────────────────────────────────────────────────────

bool addSched(uint8_t h, uint8_t m, uint8_t wday) {
  if (schedCount >= SET_MAX_SCHEDS) return false;
  scheds[schedCount++] = {h, m, wday};
  return true;
}

bool delSched(int idx) {
  if (idx < 0 || idx >= schedCount) return false;
  for (int i = idx; i < schedCount - 1; ++i)
    scheds[i] = scheds[i + 1];
  --schedCount;
  return true;
}

void clearScheds() {
  schedCount = 0;
}

} // namespace Settings
