// src/Outlook.cpp
#include "Outlook.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include "config_generated.h"
#include "Settings.h"

#define GRAPH_DEBUG false   // 상세 로그 필요 시 true

// ── PSRAM 쓰기 전용 스트림 ────────────────────────────────────────────────────
// HTTPClient::writeToStream() 이 청크 디코딩까지 처리한 뒤 여기로 씀.
// DRAM String 힙 할당 없이 PSRAM 버퍼에 직접 수신.
class PsramStream : public Stream {
public:
  PsramStream(char* buf, size_t cap) : _buf(buf), _cap(cap), _len(0) {}

  size_t write(uint8_t c) override {
    if (_len < _cap - 1) { _buf[_len++] = (char)c; return 1; }
    return 0;
  }
  size_t write(const uint8_t* data, size_t n) override {
    size_t room = _cap - 1 - _len;
    size_t wr   = (n < room) ? n : room;
    memcpy(_buf + _len, data, wr);
    _len += wr;
    return wr;
  }
  void   terminate()        { _buf[_len] = '\0'; }
  size_t length()     const { return _len; }

  // write-only → read 인터페이스는 사용 안 함
  int available() override { return 0; }
  int read()      override { return -1; }
  int peek()      override { return -1; }

private:
  char*  _buf;
  size_t _cap;
  size_t _len;
};

// ── ArduinoJson PSRAM 커스텀 얼로케이터 ────────────────────────────────────
struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override            { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void  deallocate(void* p) override           { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
static PsramAllocator s_psramAlloc;

// ── PSRAM HTTP 응답 버퍼 (64 KB, 최초 1회 할당) ───────────────────────────
static char*         s_httpBuf = nullptr;
static const size_t  HTTP_BUF  = 65536;   // 64 KB — 상위 10개 메시지에 충분

// ── 유틸리티 ──────────────────────────────────────────────────────────────────
static bool emailEquals(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    ++a; ++b;
  }
  return *a == 0 && *b == 0;
}

static bool isVipSender(const char* addr) {
  if (!addr) return false;
  // cfg:: 컴파일 상수 대신 Settings:: 런타임 목록 사용 (BLE로 변경 가능)
  for (int i = 0; i < Settings::vipCount; ++i)
    if (emailEquals(addr, Settings::vip[i])) return true;
  return false;
}

// 부팅 1회용 진단 — String 사용 OK (일회성)
static int graphGet(const char* url, const String& token, String& outBody) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (!https.begin(client, url)) return -1;
  https.addHeader("Authorization", String("Bearer ") + token);
  https.addHeader("Accept", "application/json");
  int code = https.GET();
  outBody  = https.getString();
  https.end();
  return code;
}

void Outlook::dumpDiagnostics(const String& accessToken) {
  Serial.println();
  Serial.println("============== GRAPH 진단 ==============");

  // /me
  {
    String body;
    int code = graphGet(
      "https://graph.microsoft.com/v1.0/me?$select=displayName,mail,userPrincipalName,id",
      accessToken, body);
    Serial.printf("[ME] HTTP %d\n", code);
    if (code == 200) {
      JsonDocument d;
      if (!deserializeJson(d, body)) {
        const char* mail = d["mail"] | "";
        const char* upn  = d["userPrincipalName"] | "";
        Serial.printf("  displayName       : %s\n", (const char*)(d["displayName"] | ""));
        Serial.printf("  mail              : %s\n", mail);
        Serial.printf("  userPrincipalName : %s\n", upn);
        Serial.printf("  config my_email   : %s\n", cfg::MY_EMAIL);
        bool match = emailEquals(mail, cfg::MY_EMAIL) || emailEquals(upn, cfg::MY_EMAIL);
        Serial.printf("  → my_email 매칭   : %s\n", match ? "OK ✓" : "❌ 불일치!");
      }
    }
  }

  // 폴더 목록
  {
    String body;
    int code = graphGet(
      "https://graph.microsoft.com/v1.0/me/mailFolders"
        "?$top=25&$select=displayName,unreadItemCount,totalItemCount",
      accessToken, body);
    Serial.printf("\n[FOLDERS] HTTP %d\n", code);
    if (code == 200) {
      JsonDocument d;
      if (!deserializeJson(d, body)) {
        Serial.println("  폴더명                        unread   total");
        for (JsonObject f : d["value"].as<JsonArray>()) {
          Serial.printf("  %-30s %6d   %6d\n",
            (const char*)(f["displayName"] | ""),
            (int)(f["unreadItemCount"] | 0),
            (int)(f["totalItemCount"]  | 0));
        }
      }
    }
  }

  Serial.println("========================================");
  Serial.println();
}

int Outlook::pollHighestPriority(const String& accessToken) {
  // ── PSRAM 버퍼 초기화 (최초 1회) ──────────────────────────────────────────
  if (!s_httpBuf) {
    s_httpBuf = (char*)heap_caps_malloc(HTTP_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_httpBuf) {
      Serial.println("[GRAPH] PSRAM 버퍼 할당 실패");
      return -1;
    }
    Serial.printf("[GRAPH] PSRAM 버퍼 %u KB 할당 완료\n", HTTP_BUF / 1024);
  }

  // ── HTTPS 요청 ─────────────────────────────────────────────────────────────
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  char url[256];
  snprintf(url, sizeof(url),
    "https://graph.microsoft.com/v1.0/me/mailFolders/inbox/messages"
    "?$top=%d&$select=from,toRecipients,ccRecipients,isRead,subject"
    "&$orderby=receivedDateTime%%20desc",
    cfg::TOP_N);

  if (!https.begin(client, url)) return -1;
  https.addHeader("Authorization", String("Bearer ") + accessToken);
  https.addHeader("Accept", "application/json");

  int code = https.GET();
  if (code == 401) {
    https.end();
    Serial.println("[GRAPH] 401 Unauthorized → token 갱신 필요");
    return -401;
  }
  if (code != 200) {
    Serial.printf("[GRAPH] HTTP %d\n", code);
    https.end();
    return -1;
  }

  // ── PSRAM 스트림으로 응답 수신 (DRAM String 할당 없음) ─────────────────────
  PsramStream ps(s_httpBuf, HTTP_BUF);
  int written = https.writeToStream(&ps);
  ps.terminate();
  https.end();

  if (written < 0) {
    Serial.printf("[GRAPH] 응답 수신 실패: %d\n", written);
    return -1;
  }
  size_t bodyLen = ps.length();

  if (GRAPH_DEBUG) {
    Serial.printf("[GRAPH] HTTP %d  body=%u B  heap=%u  psram=%u\n",
      code, bodyLen, ESP.getFreeHeap(), ESP.getFreePsram());
    // 첫 600자 (시리얼 폭주 방지)
    char tmp = s_httpBuf[600 < bodyLen ? 600 : bodyLen];
    s_httpBuf[600 < bodyLen ? 600 : bodyLen] = '\0';
    Serial.println(s_httpBuf);
    s_httpBuf[600 < bodyLen ? 600 : bodyLen] = tmp;
  }

  // ── JSON 파싱 (filter + doc 모두 PSRAM 사용) ───────────────────────────────
  // filter: static PSRAM — 최초 1회만 구성
  static JsonDocument* s_filter = nullptr;
  if (!s_filter) {
    s_filter = new JsonDocument(&s_psramAlloc);
    (*s_filter)["value"][0]["from"]["emailAddress"]["name"]             = true;
    (*s_filter)["value"][0]["from"]["emailAddress"]["address"]          = true;
    (*s_filter)["value"][0]["toRecipients"][0]["emailAddress"]["address"] = true;
    (*s_filter)["value"][0]["ccRecipients"][0]["emailAddress"]["address"] = true;
    (*s_filter)["value"][0]["isRead"]   = true;
    (*s_filter)["value"][0]["subject"]  = true;
  }

  JsonDocument doc(&s_psramAlloc);
  DeserializationError err = deserializeJson(
    doc, s_httpBuf, bodyLen, DeserializationOption::Filter(*s_filter));

  if (err) {
    Serial.printf("[GRAPH] JSON parse err: %s\n", err.c_str());
    return -1;
  }

  JsonArray msgs = doc["value"].as<JsonArray>();
  int total = msgs.isNull() ? 0 : (int)msgs.size();

  // 필터 후 0개인데 raw 에 메시지가 있으면 필터 없이 재파싱 (fallback)
  if (total == 0 && strstr(s_httpBuf, "\"id\":") != nullptr) {
    Serial.println("[GRAPH] ⚠️ 필터 후 0개, raw에 메시지 있음 → 필터 없이 재파싱");
    JsonDocument doc2(&s_psramAlloc);
    if (!deserializeJson(doc2, s_httpBuf, bodyLen)) {
      msgs  = doc2["value"].as<JsonArray>();
      total = msgs.isNull() ? 0 : (int)msgs.size();
      Serial.printf("[GRAPH] 필터 없이 재파싱 → %d개\n", total);
      return _analyze(msgs);
    }
  }

  return _analyze(msgs);
}

// 메시지 배열 분석 → 우선순위
int Outlook::_analyze(JsonArray msgs) {
  int total      = msgs.isNull() ? 0 : (int)msgs.size();
  int unreadCount = 0;
  MailPriority best = MailPriority::NONE;

  Serial.printf("[GRAPH] inbox 최신 %d개 검사:\n", total);

  for (JsonObject m : msgs) {
    bool isRead          = m["isRead"] | true;
    const char* fromAddr = m["from"]["emailAddress"]["address"] | "";
    const char* fromName = m["from"]["emailAddress"]["name"]    | "";
    const char* fromDisplay = (fromName && fromName[0]) ? fromName : fromAddr;
    JsonArray tos     = m["toRecipients"].as<JsonArray>();
    JsonArray ccs     = m["ccRecipients"].as<JsonArray>();
    const char* subject = m["subject"] | "";

    bool meInTo = false; int toCount = 0;
    for (JsonObject r : tos) {
      ++toCount;
      if (emailEquals(r["emailAddress"]["address"] | "", cfg::MY_EMAIL)) meInTo = true;
    }
    bool meInCc = false; int ccCount = 0;
    for (JsonObject r : ccs) {
      ++ccCount;
      if (emailEquals(r["emailAddress"]["address"] | "", cfg::MY_EMAIL)) meInCc = true;
    }

    bool vip = isVipSender(fromAddr);

    Serial.printf("  %s from=%-30s to=%d cc=%d %s%s%s subj='%.40s'\n",
      isRead ? "[READ  ]" : "[UNREAD]",
      fromDisplay, toCount, ccCount,
      meInTo ? "[ME-TO]" : "",
      meInCc ? "[ME-CC]" : "",
      vip    ? "[VIP]"   : "",
      subject);

    if (isRead) continue;
    ++unreadCount;

    MailPriority p;
    if (vip)                                         p = MailPriority::VIP_TO_ME;
    else if (meInTo && toCount == 1)                 p = MailPriority::SOLE_TO_ME;
    else                                             p = MailPriority::OTHER_UNREAD;

    if ((int)p > (int)best) {
      best = p;
      // 최고 우선순위 메일 정보 저장 (디스플레이용)
      // 이름이 있으면 "이름 주소", 없으면 주소만
      if (fromName && fromName[0]) {
        snprintf(_lastFrom, sizeof(_lastFrom), "%s (%s)", fromName, fromAddr);
      } else {
        strncpy(_lastFrom, fromAddr, sizeof(_lastFrom) - 1);
        _lastFrom[sizeof(_lastFrom) - 1] = '\0';
      }
      strncpy(_lastSubject, subject,  sizeof(_lastSubject) - 1);
      _lastSubject[sizeof(_lastSubject) - 1] = '\0';
      _lastIsCC = (meInCc && !meInTo);
    }
    if (best == MailPriority::VIP_TO_ME) break;
  }

  Serial.printf("[GRAPH] unread=%d  → priority=%d\n", unreadCount, (int)best);
  _lastUnreadCount = unreadCount;
  return (int)best;
}
