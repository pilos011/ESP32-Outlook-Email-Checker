// src/Oauth.h — Microsoft Identity Platform OAuth 2.0 Device Code Flow
// 헤드리스 디바이스(ESP32)용 인증 방식.
//   1) /devicecode 호출 -> user_code, verification_uri 발급
//   2) 시리얼/LED로 사용자에게 안내 -> 사용자가 폰에서 코드 입력
//   3) /token 폴링 -> access_token + refresh_token 획득
//   4) refresh_token 은 NVS(Preferences)에 저장 -> 재부팅 후 자동 로그인
#pragma once

#include <Arduino.h>

class Oauth {
public:
  bool begin();                         // NVS에서 refresh_token 읽음
  bool ensureAccessToken();             // 만료되었으면 갱신, 없으면 false
  // onCode: userCode 발급 직후 호출되는 콜백 (OLED 표시 등), nullptr 가능
  bool runDeviceCodeFlow(void (*onCode)(const char* userCode) = nullptr);
  void clearTokens();                   // NVS 비우기 (디버그용)

  const String& accessToken() const { return _accessToken; }
  bool hasRefreshToken() const { return _refreshToken.length() > 0; }

private:
  String _accessToken;
  String _refreshToken;
  unsigned long _accessExpiresAt = 0;   // millis() 기준

  bool _refreshAccess();
  bool _saveRefresh(const String& rt);
  String _loadRefresh();
};
