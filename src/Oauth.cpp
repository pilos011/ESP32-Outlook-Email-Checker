// src/Oauth.cpp
#include "Oauth.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "config_generated.h"

// Microsoft Graph 호출에 필요한 권한
// offline_access = refresh_token 발급
// Mail.Read      = 받은편지함 읽기
static const char* SCOPE = "offline_access Mail.Read";

// ───── helpers ─────
static String url_login(const String& path) {
  String u = "https://login.microsoftonline.com/";
  u += cfg::TENANT;
  u += path;
  return u;
}

static String urlencode(const String& s) {
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

// ───── public ─────
bool Oauth::begin() {
  _refreshToken = _loadRefresh();
  return _refreshToken.length() > 0;
}

void Oauth::clearTokens() {
  Preferences p;
  p.begin("oauth", false);
  p.clear();
  p.end();
  _refreshToken = "";
  _accessToken = "";
  _accessExpiresAt = 0;
}

bool Oauth::ensureAccessToken() {
  if (_accessToken.length() > 0 && millis() < _accessExpiresAt) return true;
  if (_refreshToken.length() == 0) return false;
  return _refreshAccess();
}

// ───── private ─────
String Oauth::_loadRefresh() {
  Preferences p;
  p.begin("oauth", true);
  String rt = p.getString("refresh", "");
  p.end();
  return rt;
}

bool Oauth::_saveRefresh(const String& rt) {
  Preferences p;
  if (!p.begin("oauth", false)) return false;
  bool ok = p.putString("refresh", rt) > 0;
  p.end();
  return ok;
}

bool Oauth::_refreshAccess() {
  WiFiClientSecure client;
  client.setInsecure();   // login.microsoftonline.com — 단순화 (root CA 미장착)
  HTTPClient https;
  String url = url_login("/oauth2/v2.0/token");
  if (!https.begin(client, url)) return false;
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "client_id=" + urlencode(cfg::CLIENT_ID) +
                "&grant_type=refresh_token" +
                "&refresh_token=" + urlencode(_refreshToken) +
                "&scope=" + urlencode(SCOPE);

  int code = https.POST(body);
  String resp = https.getString();
  https.end();

  if (code != 200) {
    Serial.printf("[OAUTH] refresh failed: HTTP %d\n%s\n", code, resp.c_str());
    // invalid_grant → refresh_token 영구 무효 (MFA 만료, 비밀번호 변경 등)
    // 재시도해도 동일하게 실패하므로 NVS에서 즉시 삭제 → Device Code Flow 유도
    if (code == 400) {
      JsonDocument errDoc;
      if (!deserializeJson(errDoc, resp)) {
        const char* errCode = errDoc["error"] | "";
        if (strcmp(errCode, "invalid_grant") == 0) {
          Serial.println("[OAUTH] invalid_grant → NVS refresh_token 삭제");
          clearTokens();
        }
      }
    }
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, resp);
  if (err) {
    Serial.printf("[OAUTH] refresh JSON err: %s\n", err.c_str());
    return false;
  }

  _accessToken = doc["access_token"].as<const char*>();
  // refresh_token 은 갱신될 때마다 바뀔 수 있음
  if (doc["refresh_token"].is<const char*>()) {
    String newRt = doc["refresh_token"].as<const char*>();
    if (newRt != _refreshToken) {
      _refreshToken = newRt;
      _saveRefresh(_refreshToken);
    }
  }
  int expiresIn = doc["expires_in"] | 3600;
  // 만료 60초 전에 미리 갱신하도록
  _accessExpiresAt = millis() + (unsigned long)(expiresIn - 60) * 1000UL;
  Serial.printf("[OAUTH] access token refreshed (%ds)\n", expiresIn);
  Serial.println("[OAUTH] ── access_token (jwt.ms 확인용 — 나중에 삭제) ──");
  Serial.println(_accessToken);
  Serial.println("[OAUTH] ─────────────────────────────────────────────────");
  return true;
}

bool Oauth::runDeviceCodeFlow(void (*onCode)(const char* userCode)) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);   // 15초 타임아웃 — 무한 대기 방지
  HTTPClient https;

  // ── 1단계: device code 요청 ──
  String url1 = url_login("/oauth2/v2.0/devicecode");
  if (!https.begin(client, url1)) return false;
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body1 = "client_id=" + urlencode(cfg::CLIENT_ID) +
                 "&scope=" + urlencode(SCOPE);
  int code1 = https.POST(body1);
  String resp1 = https.getString();
  https.end();

  if (code1 != 200) {
    Serial.printf("[OAUTH] devicecode failed: HTTP %d\n%s\n", code1, resp1.c_str());
    return false;
  }

  JsonDocument doc1;
  if (deserializeJson(doc1, resp1)) {
    Serial.println("[OAUTH] devicecode JSON parse error");
    return false;
  }

  String deviceCode      = doc1["device_code"].as<const char*>();
  String userCode        = doc1["user_code"].as<const char*>();
  String verificationUri = doc1["verification_uri"].as<const char*>();
  int    interval        = doc1["interval"] | 5;
  int    expiresIn       = doc1["expires_in"] | 900;

  // OLED 표시 콜백
  if (onCode) onCode(userCode.c_str());

  Serial.println();
  Serial.println("==================================================");
  Serial.println(" OUTLOOK 인증 필요 — 폰/PC 브라우저에서 아래 단계 수행");
  Serial.println("==================================================");
  Serial.printf(" 1) 접속:  %s\n", verificationUri.c_str());
  Serial.printf(" 2) 코드입력: %s\n", userCode.c_str());
  Serial.printf(" 3) 본인 Outlook 계정으로 로그인 + 권한 동의\n");
  Serial.printf("     (만료까지 %d초)\n", expiresIn);
  Serial.println("==================================================");

  // ── 2단계: 토큰 폴링 ──
  String url2 = url_login("/oauth2/v2.0/token");
  unsigned long start = millis();
  while (millis() - start < (unsigned long)expiresIn * 1000UL) {
    delay((unsigned long)interval * 1000UL);

    if (!https.begin(client, url2)) continue;
    https.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String body2 = "grant_type=urn:ietf:params:oauth:grant-type:device_code"
                   "&client_id=" + urlencode(cfg::CLIENT_ID) +
                   "&device_code=" + urlencode(deviceCode);
    int code2 = https.POST(body2);
    String resp2 = https.getString();
    https.end();

    JsonDocument doc2;
    if (deserializeJson(doc2, resp2)) continue;

    if (code2 == 200) {
      _accessToken = doc2["access_token"].as<const char*>();
      _refreshToken = doc2["refresh_token"].as<const char*>();
      int expIn = doc2["expires_in"] | 3600;
      _accessExpiresAt = millis() + (unsigned long)(expIn - 60) * 1000UL;
      _saveRefresh(_refreshToken);
      Serial.println("[OAUTH] 인증 성공! refresh_token 저장됨");
      Serial.println("[OAUTH] ── access_token (jwt.ms 확인용 — 나중에 삭제) ──");
      Serial.println(_accessToken);
      Serial.println("[OAUTH] ─────────────────────────────────────────────────");
      return true;
    }

    // 진행 중 / 사용자 입력 대기
    const char* err = doc2["error"] | "";
    if (strcmp(err, "authorization_pending") == 0) {
      Serial.print(".");
      continue;
    }
    if (strcmp(err, "slow_down") == 0) {
      interval += 5;
      continue;
    }
    if (strcmp(err, "authorization_declined") == 0 ||
        strcmp(err, "expired_token") == 0 ||
        strcmp(err, "bad_verification_code") == 0) {
      Serial.printf("\n[OAUTH] 실패: %s\n", err);
      return false;
    }
    // 알 수 없는 에러
    Serial.printf("\n[OAUTH] 응답: HTTP %d  %s\n", code2, resp2.c_str());
  }

  Serial.println("\n[OAUTH] 시간 초과");
  return false;
}
