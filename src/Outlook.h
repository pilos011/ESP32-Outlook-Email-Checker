// src/Outlook.h — Microsoft Graph API 받은편지함 폴링
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Led.h"

enum class MailPriority : int {
  NONE          = 0,
  OTHER_UNREAD  = 1,
  SOLE_TO_ME    = 2,
  VIP_TO_ME     = 3
};

class Outlook {
public:
  int  pollHighestPriority(const String& accessToken);
  void dumpDiagnostics(const String& accessToken);
  int         lastUnreadCount() const { return _lastUnreadCount; }
  const char* lastFrom()        const { return _lastFrom; }
  bool        lastIsCC()        const { return _lastIsCC; }
  const char* lastSubject()     const { return _lastSubject; }

private:
  int  _analyze(JsonArray msgs);
  int  _lastUnreadCount = -1;
  char _lastFrom[96]    = {};
  bool _lastIsCC        = false;
  char _lastSubject[128] = {};
};
