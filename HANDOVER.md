# Outlook Mail LED — 통합 문서

> 개발자 핸드오버 + 사용자 가이드를 통합한 단일 참조 문서입니다.  
> 이전 `README.md`의 내용을 이 문서에 병합했으며 README.md는 삭제해도 됩니다.

---

## 1. 프로젝트 개요

ESP32-S3-N16R8 보드를 이용해 Microsoft Outlook 받은편지함을 폴링하고, 새 메일 우선순위에 따라 RGB LED와 부저로 알림을 주는 IoT 장치입니다. OLED 디스플레이에는 인증 코드·시계·발신자/제목을 표시합니다.

- **플랫폼**: ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM)
- **빌드 도구**: PlatformIO (Arduino 프레임워크)
- **프로젝트 경로**: `D:\Personal\outlook-led`

### LED 색상 의미 (한눈에)

| 색상 | 조건 |
|------|------|
| 🔴 빨강 점멸 | `vip_senders`에 등록한 발신인이 보낸 메일 (수신인 포함) |
| 🔵 파랑 점멸 | 내가 **유일한 수신인** (To: 나 1명, CC 비어있음) |
| 🟢 초록 상시 | 그 외 읽지 않은 메일 |
| ⚫ 꺼짐 | 미확인 메일 없음 / 비근무시간 |

> 우선순위: **빨강 > 파랑 > 초록**. 동시에 여러 조건이 맞으면 가장 높은 색만 표시.

---

## 2. 하드웨어 구성 및 핀 설정

### 2-1. 핀 맵

| 부품 | GPIO | 비고 |
|------|------|------|
| WS2812 RGB LED | **48** | 보드 내장 NeoPixel (DevKitC-1 / YD-ESP32-S3) |
| Passive Buzzer | **21** | PWM LEDC ch.4, 2700 Hz |
| OLED SDA | **8** | I2C (Wire) |
| OLED SCL | **9** | I2C (Wire) |

> ESP32-S3-Zero 보드를 쓰는 경우 LED 핀은 GPIO 21.

### 2-2. 핀 제약 (ESP32-S3)

| 핀 범위 | 이유 | 상태 |
|---------|------|------|
| GPIO 19, 20 | USB JTAG — 충돌 | ❌ 부저 금지 |
| GPIO 33~37 | PSRAM 전용 — 충돌 | ❌ 금지 |
| GPIO 21, 13, 1 | 안전한 출력 핀 | ✅ 권장 |

### 2-3. OLED 사양 (중요!)

- **모델**: 0.91인치 128×**32** SSD1306 I2C
- **U8g2 드라이버**: `U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C`
- **주의**: 외관상 스펙 시트가 128×64로 표기되어 있어도 실제 패널은 128×32임 (→ 시행착오 섹션 참고)
- **I2C 주소**: 기본 0x3C

---

## 3. 소프트웨어 아키텍처

### 3-1. 소스 파일 구조

```
outlook-led/
├── config.ini                  ← 기본값 설정 파일 (이것만 편집)
├── scripts/
│   └── gen_config.py           ← 빌드 전 config.ini → config_generated.h 변환
└── src/
    ├── main.cpp                ← setup() / loop() 진입점
    ├── config_generated.h      ← 자동 생성 (편집 금지)
    ├── Settings.h / .cpp       ← 런타임 설정 (NVS 우선, 없으면 config.ini 기본값)
    ├── Ble.h / .cpp            ← BLE NUS 무선 설정 콘솔 (nRF Connect 앱)
    ├── Display.h / .cpp        ← OLED 표시 매니저
    ├── Led.h / .cpp            ← WS2812 RGB LED 상태 머신
    ├── Buzzer.h / .cpp         ← Passive Buzzer 논블로킹 비프
    ├── Oauth.h / .cpp          ← Microsoft OAuth 2.0 Device Code Flow
    └── Outlook.h / .cpp        ← Microsoft Graph API 폴링
```

### 3-2. 실행 흐름

```
setup()
  ├─ Settings::init()   → NVS 오버라이드 → config.ini 기본값 순으로 모든 설정 로드
  ├─ OLED 초기화
  ├─ LED 자가테스트 (CYAN 1초)
  ├─ Buzzer 자가테스트 (삐 80ms)
  ├─ Ble::begin()       → BLE NUS 광고 시작 (WiFi 연결 전 — 자격증명 변경 가능)
  ├─ WiFi 연결 (최대 30초, Settings::wifiSsid/Pass 사용)
  ├─ NTP 동기화 (KST UTC+9)
  ├─ OAuth: NVS에 refresh_token 있으면 → access_token 갱신
  │          없으면 → Device Code Flow (OLED에 코드 표시)
  └─ 초기 폴링 → 시계 또는 이메일 화면

loop() (최대 속도, disp.update()가 실질 주기 결정)
  ├─ led.update()       → LED 점멸/펄스 처리
  ├─ buzz.update()      → 비프 타이머 처리
  ├─ disp.update()      → IDLE 1초 시계갱신 / EMAIL 순차 스크롤
  ├─ updateDoubleBeep() → 예약 알림 이중비프
  ├─ checkSchedule()    → Settings::scheds[] 기반 예약 알림
  ├─ BLE 명령 처리      → handleBleCommand() (논블로킹, 큐에서 하나씩 처리)
  ├─ 힙 감시 (<30KB → 재부팅)
  ├─ 24시간 정기 재부팅
  ├─ 근무시간 전환 감지 → LED/OLED 처리
  ├─ WiFi 감시 (5초마다, 3회 실패 → ORANGE LED)
  └─ 폴링 (Settings::pollSec 마다, 근무시간 중에만, FreeRTOS 코어1 비동기)
```

### 3-3. 비동기 폴링 구조

HTTP/TLS 폴링이 메인 루프를 블로킹하여 OLED 시계가 멈추는 문제를 해결하기 위해 FreeRTOS 태스크를 사용합니다.

```
메인 루프 (코어 0)          폴링 태스크 (코어 1, 16KB 스택)
─────────────────           ──────────────────────────────
s_pollBusy = true  ──────→  pollTaskFn()
disp.update() 계속           oauth.ensureAccessToken()
led.update() 계속            outlook.pollHighestPriority()
                             s_taskPollP = 결과
                   ←──────  s_pollDone = true, s_pollBusy = false
applyPriority(s_taskPollP)
updateDisplay(s_taskPollP)
```

> **주의**: 태스크를 코어 0에 고정하면 WiFi 스택과 충돌합니다. 반드시 **코어 1** 사용.  
> **주의**: 스택 10KB는 mbedTLS SSL 핸드셰이크에 부족합니다. **16384** 이상 필요.

---

## 4. 현재 기능 상세

### 4-1. 메일 우선순위 (MailPriority)

| 우선순위 | 조건 | LED | 비프 |
|----------|------|-----|------|
| `VIP_TO_ME` (3) | `vip_senders` 목록의 발신인 | 🔴 빨강 점멸 | ✅ 1회 |
| `SOLE_TO_ME` (2) | 내가 유일한 수신인(To:) | 🔵 파랑 점멸 | ❌ |
| `OTHER_UNREAD` (1) | 그 외 읽지 않은 메일 | 🟢 초록 상시 | ❌ |
| `NONE` (0) | 읽지 않은 메일 없음 | ⚫ 소등 | ❌ |

GREEN 상태에서 새 메일이 추가로 오면 300ms 한 번 깜빡(`flashOnce`)합니다.

### 4-2. LED 상태 전체

| LedState | 색상/패턴 | 의미 |
|----------|-----------|------|
| `RED_BLINK` | 🔴 점멸 (blink_ms) | VIP 발신인 미읽음 |
| `BLUE_BLINK` | 🔵 점멸 (blink_ms) | 나 혼자 To: |
| `GREEN_SOLID` | 🟢 상시 점등 | 기타 미읽음 |
| `WHITE_PULSE` | ⚪ 사인파 펄스 | WiFi 연결 중 |
| `YELLOW_PULSE` | 🟡 사인파 펄스 | OAuth 인증 대기 |
| `PURPLE_PULSE` | 🟣 사인파 펄스 | 인증 오류 |
| `CYAN_FLASH` | 🩵 빠른 점멸 | 부팅 자가테스트 |
| `ORANGE_SLOW_BLINK` | 🟠 0.5s ON / 29.5s OFF | WiFi 포기 |
| `OFF` | ⚫ | 소등 |

### 4-3. OLED 화면 모드

#### AUTH 화면 (인증 코드)
```
microsoft.com/devicelogin      ← 5×7 소형 폰트, baseline y=7
     BCXR-1A2B                 ← 10×20 대형 폰트, 중앙 정렬, baseline y=31
```

#### IDLE 화면 (시계)
```
  2026-05-26(월)               ← unifont 16px, baseline y=13 (상단 밀착)
   AM 09:30:45                 ← 9×18px 폰트, baseline y=31 (하단 밀착)
```
- 1초마다 갱신
- 비근무시간 동작은 `off_hours_oled` 설정으로 제어

#### EMAIL 화면 (이메일 알림)
```
홍길동 (hong@company.com)      ← unifont 16px, baseline y=13
RE: 오늘 회의 일정 공유합니다   ← unifont 16px, baseline y=29
```

**순차 스크롤 동작** (텍스트가 128px를 초과할 때):

```
[표시 시작]
 ↓ 0.5초 대기
[하단 제목 스크롤] scroll_subject_ms/px 속도로 좌→
 ↓ 마지막 글자 사라지면 즉시 종료, 0.5초 대기
[상단 발신자 스크롤] scroll_sender_ms/px 속도로 좌→
 ↓ 마지막 글자 사라지면 즉시 종료, 0.5초 대기
[다시 하단 제목 스크롤] → 반복
```

- 두 줄이 **동시에 움직이지 않음** — 한 번에 한 줄만 스크롤
- 같은 이메일이 다시 표시될 때(10초 폴링) **스크롤 상태 유지** (리셋 없음)
- 발신자 표시 이름 없으면 이메일 주소만 표시: `hong@company.com`
- 발신자 표시 이름 있으면: `홍길동 (hong@company.com)`

#### WiFi Error 화면
```
    WiFi Connect Error.        ← 5×7, 중앙 정렬
```

### 4-4. 예약 알림 (이중 비프)

`config.ini`의 `[schedule]` 섹션으로 기본값을 정의. **BLE NUS로 런타임 추가/삭제 가능.**  
형식: `HHMM:w` (w: 0=일, 1=월, 2=화, 3=수, 4=목, 5=금, 6=토, **7=평일**)

| 시각 | 요일 (w) | 내용 |
|------|----------|------|
| 월 08:20 | 1 | 팀장회의 |
| 평일 11:30 | 7 | 점심 시작 |
| 평일 12:30 | 7 | 점심 종료 |
| 평일 17:00 | 7 | 퇴근 |

NVS에 `sched_count` 키가 있으면 config.ini 기본값 대신 NVS 값 사용 (BLE로 변경한 내용이 재부팅 후에도 유지됨).

### 4-5. 근무시간 제어

- `work_hours_only = true` 시 월~금 `work_start`~`work_end` 외에는 LED 소등 + 폴링 중단
- 비근무시간 OLED 동작은 `off_hours_oled`로 제어:
  - `idle`: 시계 계속 표시 (탁상시계 역할)
  - `off`: 화면 끄기 (절전)
- 근무시간 재진입 시 LED·OLED 즉시 복원
- 부팅 시 비근무시간이면 `setup()` 안에서 즉시 초기 상태 적용 (거짓 전환 방지)

### 4-6. 안정성 장치

| 장치 | 조건 | 동작 |
|------|------|------|
| 힙 부족 재부팅 | 가용 힙 < 30KB | 즉시 `ESP.restart()` |
| 24시간 정기 재부팅 | `millis() > 86400000` | 메모리 단편화 방지 |
| WiFi 포기 | 3회 연속 실패 | ORANGE LED, 재연결 중단 |
| 401 재시도 | Graph API 401 응답 | 토큰 즉시 갱신 후 1회 재시도 |
| 토큰 갱신 실패 | setup() 중 | 5분 대기 후 재부팅 (NVS 보존) |

### 4-7. 설정 우선순위 (Settings 시스템)

```
config.ini (컴파일 기본값)
    ↓ 빌드 시 config_generated.h 에 constexpr 상수로 박힘
Settings::init() 에서 config.ini 기본값 로드
    ↓ NVS에 해당 키가 있으면 덮어씀
런타임 Settings:: 값 (WiFi/밝기/폴링/근무시간/VIP/예약)
    ↓ BLE 명령 → 즉시 적용 + Settings::saveAll() → NVS 저장
재부팅 후에도 BLE로 변경한 값 유지
```

- NVS 네임스페이스: `"app_cfg"` (Preferences 라이브러리)
- `isKey()` 를 사용해 "의도적 빈 목록(SCHED:clear / VIP:del 전체)"과 "미설정"을 구별

---

## 5. BLE NUS 무선 설정 콘솔

### 5-1. 연결 방법

1. **nRF Connect** 앱(iOS/Android) 설치
2. 스캔 → **OutlookLED** 장치 선택 → Connect
3. **Nordic UART Service** (UUID: `6E400001-...`) 찾기
4. **TX Characteristic** (`6E400003`) — Notify 활성화 (응답 수신용)
5. **RX Characteristic** (`6E400002`) — 명령 전송용
6. 명령을 UTF-8 + `\n`으로 Write

> ⚠️ 보안 없음(암호화 X) — 내부 사무실 전용 장치 실용 타협 (사용자 명시 요청)

### 5-2. 명령 목록

| 명령 | 형식 | 설명 |
|------|------|------|
| `STATUS` | `STATUS` | 모든 현재 설정 조회 |
| `REBOOT` | `REBOOT` | 장치 재부팅 |
| `RESET` | `RESET` | NVS 초기화 후 재부팅 (config.ini 기본값 복원) |
| `WIFI` | `WIFI:ssid:password` | WiFi 변경 및 즉시 재연결 |
| `BRIGHT` | `BRIGHT:n` (0~255) | LED 밝기 즉시 변경 |
| `BLINK` | `BLINK:ms` (100~5000) | 점멸 주기 즉시 변경 |
| `POLL` | `POLL:sec` (5~3600) | 폴링 주기 변경 |
| `WORK` | `WORK:HHMM:HHMM` | 근무시간 변경 (예: `WORK:0730:1700`) |
| `VIP:list` | `VIP:list` | VIP 목록 조회 |
| `VIP:add` | `VIP:add:email` | VIP 추가 |
| `VIP:del` | `VIP:del:email` | VIP 삭제 |
| `SCHED:list` | `SCHED:list` | 예약 알림 목록 조회 |
| `SCHED:add` | `SCHED:add:HHMM:w` | 예약 추가 (예: `SCHED:add:0820:1`) |
| `SCHED:del` | `SCHED:del:n` | 예약 삭제 (인덱스 번호) |
| `SCHED:clear` | `SCHED:clear` | 예약 전체 삭제 |

### 5-3. 사용 예시

```
→ STATUS
← === 현재 설정 ===
← WiFi: MyOfficeWiFi
← 밝기:5 점멸:500ms 폴링:10s
← 근무:07:30~17:00
← VIP(3):
←  [0] boss@example.com
← 예약(4):
←  [0] 08:20 월
← =================

→ WIFI:NewSSID:NewPassword
← OK 저장 → 재연결 중...
← OK 연결됨: 192.168.1.105

→ SCHED:add:0900:1
← OK 추가:09:00 월

→ BRIGHT:30
← OK brightness=30
```

---

## 6. config.ini 설정 가이드

`config.ini`를 수정하고 빌드하면 `scripts/gen_config.py`가 `src/config_generated.h`를 자동 생성합니다.  
**`config_generated.h`는 직접 편집하지 마십시오.**

> ⚠️ WiFi 비밀번호가 펌웨어 바이너리 안에 포함됩니다. 빌드된 `.bin` 파일을 외부에 공유하지 마십시오.

```ini
[wifi]
ssid     = MyNetwork
password = MyPassword

[outlook]
my_email    = me@company.com           ; 수신인/참조인 판별에 사용
vip_senders = boss@co.com,cto@co.com  ; 콤마 구분, 적색 LED + 비프
client_id   = xxxxxxxx-xxxx-...        ; Azure AD 앱 등록 client_id
tenant      = xxxxxxxx-xxxx-...        ; 테넌트 ID (개인계정: consumers)

[poll]
interval_sec = 10    ; 폴링 주기 (초). Graph 쓰로틀링 피하려면 30초 이상 권장
top_n        = 5     ; 최신 N개 메일 검사

[led]
pin        = 48      ; WS2812 GPIO (DevKitC-1: 48, Zero: 21)
brightness = 10      ; 0~255 (실내: 10~30 권장)
blink_ms   = 500     ; 점멸 주기 (ms)

[schedule]
work_hours_only = true        ; true = 근무시간만 동작
work_start      = 07:30
work_end        = 17:00

; 예약 알림 기본값 (최대 8개, BLE SCHED:add/del/clear 로 런타임 변경 가능)
; 형식: HHMM:w  (w: 0=일 1=월 2=화 3=수 4=목 5=금 6=토 7=평일)
sched_count = 4
sched_0 = 0820:1   ; 월요일 08:20
sched_1 = 1130:7   ; 평일 11:30
sched_2 = 1230:7   ; 평일 12:30
sched_3 = 1700:7   ; 평일 17:00

[buzzer]
pin         = 21     ; GPIO (안전핀: 21, 13, 1)
volume      = 50     ; 0~100 (0 = 무음)
freq        = 2700   ; Hz (Passive Buzzer만 적용)
duration_ms = 200    ; 비프 길이

[oled]
sda            = 8
scl            = 9
off_hours_oled = off        ; idle = 시계표시 / off = 화면끄기

; 이메일 스크롤 속도 (ms/픽셀, 값이 작을수록 빠름)
; 30 = 빠름 / 50 = 보통 / 80 = 느림
scroll_subject_ms = 30      ; 하단 제목 스크롤 속도
scroll_sender_ms  = 30      ; 상단 발신자 스크롤 속도
```

---

## 7. Azure AD 앱 등록 방법 (최초 설정 시 1회만)

### 7-1. 앱 등록

1. [entra.microsoft.com](https://entra.microsoft.com) 접속 → 본인 Microsoft 계정으로 로그인
2. 좌측 메뉴 **Identity → Applications → App registrations** → **New registration** 클릭
3. 항목 입력:
   - **Name**: `OutlookMailLED` (아무 이름)
   - **Supported account types**:
     - 개인 outlook.com: **Accounts in any organizational directory and personal Microsoft accounts**
     - 회사 메일만: **Accounts in this organizational directory only**
   - **Redirect URI**: 비워두기
4. **Register** 클릭
5. **Overview** 탭에서 **Application (client) ID** 복사 → `config.ini`의 `client_id`에 붙여넣기
6. 같은 페이지에서 **Directory (tenant) ID** 복사 → `config.ini`의 `tenant`에 붙여넣기

### 7-2. Public Client Flow 활성화 ★ (안 하면 인증 실패)

7. 좌측 메뉴 **Authentication** 클릭
8. 하단 **Allow public client flows** → **Yes** 로 변경
9. **Save** 클릭

### 7-3. API 권한 부여

10. 좌측 메뉴 **API permissions** → **Add a permission** → **Microsoft Graph** → **Delegated permissions**
11. `Mail.Read` 검색 → 체크 → **Add permissions**
12. (회사 계정의 경우) **Grant admin consent** 클릭

> `offline_access`는 자동 포함. 권한 목록에 안 보여도 정상.

---

## 8. 첫 실행 / OAuth 인증 절차

1. 펌웨어 업로드 후 **Serial Monitor (115200 baud)** 열기
2. OLED에 `microsoft.com/devicelogin` + 8자 인증 코드 표시  
   (LED는 🟡 노란 펄스 — 사용자 입력 대기 중)
3. 스마트폰/PC 브라우저에서 `https://microsoft.com/devicelogin` 접속
4. OLED에 표시된 코드 입력 → 본인 Outlook 계정으로 로그인 → 권한 동의
5. 인증 성공 → `refresh_token`이 NVS(Flash)에 저장됨
6. **이후 재부팅 시 자동 로그인** (토큰 만료 시 자동 갱신)

```
[OAUTH] 인증 성공! refresh_token 저장됨
[BOOT] ready — 폴링 시작
[GRAPH] unread=2
  - from=boss@company.com  subj='긴급'  [VIP][ME-TO]
[LED] RED  (VIP -> me)
```

> refresh_token 초기화: `Oauth::clearTokens()` 호출 또는 NVS 전체 삭제

---

## 9. 빌드 및 업로드

```bash
# 업로드 (빌드 포함)
pio run -t upload

# 빌드만
pio run
```

빌드 시 `gen_config.py`가 자동 실행되어 `config_generated.h`가 갱신됩니다.  
콘솔에 다음 로그가 나오면 정상:
```
>> generated src/config_generated.h
   wifi.ssid          = MyNetwork
   outlook.my_email   = me@company.com
   scroll subject/sender = 30ms / 30ms per px
   ...
```

---

## 10. 의존 라이브러리 (platformio.ini)

| 라이브러리 | 용도 |
|-----------|------|
| `U8g2` | OLED 드라이버 + 한글 폰트 |
| `Adafruit NeoPixel` | WS2812 RGB LED |
| `ArduinoJson` | Graph API JSON 파싱 |
| `WiFiClientSecure` + `HTTPClient` | HTTPS 통신 |
| `Preferences` | NVS refresh_token 저장 |

---

## 11. 트러블슈팅

### LED 색이 이상하거나 안 켜짐
- 보드의 NeoPixel 핀 확인: DevKitC-1/YD-ESP32-S3 = 48, ESP32-S3-Zero = 21
- `config.ini`의 `[led] pin` 변경 후 재빌드
- 보드 상단 빨강 LED는 전원 표시등 — 코드로 제어 불가

### 인증 오류: `AADSTS7000218`
→ Azure 앱 **Authentication** 에서 **Allow public client flows = Yes** 미설정. (6-2 단계)

### 인증 오류: `AADSTS50059: No tenant-identifying information`
→ 개인 outlook.com 계정에 `tenant=organizations` 설정. `consumers` 또는 `common`으로 변경.

### 401 Unauthorized 지속
→ 코드가 토큰 자동 갱신을 1회 재시도함. 그래도 안 되면 NVS 토큰 만료.  
→ `oauth.clearTokens()` 임시 호출 후 재빌드/업로드 → 재인증.

### OLED 화면 안 나옴
1. 근무시간 확인: `work_hours_only=true` + `off_hours_oled=off` 조합이면 비근무시간에 의도적으로 꺼짐
2. 시리얼 로그에서 `[OLED] 초기화 완료` 메시지 확인
3. SDA/SCL 핀 배선 및 `config.ini` 값 확인

### 점멸이 너무 빠름/느림
→ `[led] blink_ms` 변경. 250 = 빠르게, 1000 = 느리게.

### 스크롤이 너무 빠름/느림
→ `[oled] scroll_subject_ms` / `scroll_sender_ms` 변경. 30 = 빠름, 80 = 느림.

### Graph 응답에 메모리 부족
→ `[poll] top_n`을 줄이기 (기본 5).

---

## 12. 보안 메모

- `client_id`는 비밀이 아님 (공개 가능).
- **WiFi 비밀번호**와 **refresh_token**은 비밀 정보.
- 펌웨어 바이너리 외부 공유 금지 (WiFi 비번 포함).
- `refresh_token`은 NVS의 `oauth` 네임스페이스에 **평문** 저장. 디바이스 도난 시 메일 읽기 권한 노출 → Azure 포털에서 해당 토큰 revoke 가능.
- `Oauth.cpp` 내 `Serial.println(_accessToken)` — 프로덕션 전 제거 권장.

---

## 13. 시행착오 및 해결 기록

### 🔴 [T-01] OLED 해상도 오판 — 128×64 vs 128×32

**현상**: 스펙 시트 "0.91인치 SSD1306"을 128×64로 가정. 화면 상단이 보이지 않고 하단만 표시됨.

**원인**: 실제 패널은 128×32. SSD1306 칩은 64행 지원이지만 이 패널은 COM32~63(하단)에만 픽셀 연결. 64px 드라이버로 초기화하면 y=0~31 그린 내용이 화면에 안 나옴.

**진단 방법**:
```cpp
_u8g2.clearBuffer();
_u8g2.drawFrame(0, 0, 128, 32);   // 4변 테두리가 화면에 꽉 차면 32px 확정
_u8g2.drawLine(0, 0, 127, 31);
_u8g2.sendBuffer();
delay(3000);
```

**해결**: 드라이버를 `U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C`로 확정.  
`UNIVISION` 변종 = 이 패널의 COM 핀 배열에 맞는 초기화 커맨드 포함.

---

### 🟡 [T-02] 폰트 깨짐 — 드라이버 변종 혼동

**현상**: `NONAME` 변종 → 글자 전체 깨짐(가로줄 패턴). `ALT0` 변종 → 글자는 깨끗하지만 하단 절반 사용 안 됨.

**원인**: COM 핀 배열(sequential/alternate) 초기화 커맨드 불일치.

**결론**: 128×32 전용 드라이버(`UNIVISION`)만이 이 패널에 올바름.

---

### 🟡 [T-03] 한글 상단 클리핑 — baseline y값 오설정

**현상**: `drawUTF8(x, 11, text)` — 한글 글자 상단 2px 잘림.

**원인**: `unifont_t_korean2` ascent = **13px**. baseline y=11이면 글자 상단 = y=−2 → 화면 위로 잘림.

**해결**:
- 발신자(상단 줄): baseline y=**13** (글자 상단 y=0, 화면 꼭대기 밀착)
- 제목(하단 줄): baseline y=**29** (descent 2px가 y=30~31, 화면 맨 아래 밀착)

---

### 🟡 [T-04] PSRAM 미사용 시 HTTP 응답 DRAM 부족

**현상**: Graph API 응답을 `https.getString()`으로 받으면 DRAM 힙 부족.

**해결**:
- `PsramStream` 커스텀 스트림 → `https.writeToStream(&ps)`로 PSRAM 버퍼에 직접 수신
- `PsramAllocator` → ArduinoJson `JsonDocument`도 PSRAM 사용
- 64KB PSRAM 버퍼 최초 1회 할당 후 재사용 (`static char* s_httpBuf`)

---

### 🟡 [T-05] JSON 필터링 후 0개 파싱

**현상**: ArduinoJson filter 사용 시 간헐적으로 `value` 배열 0개 파싱. Raw 응답에는 데이터 있음.

**해결**: 필터 후 0개이고 raw에 `"id":`가 있으면 필터 없이 전체 재파싱하는 fallback 추가.

---

### 🟡 [T-06] OLED 시계 7초마다 멈춤 — HTTP 블로킹

**현상**: 폴링(10초 주기)마다 `https.GET()`이 메인 루프를 블로킹. `disp.update()`가 7~8초간 호출 안 되어 시계가 멈춤.

**해결**: FreeRTOS `xTaskCreatePinnedToCore`로 폴링을 **코어 1에 비동기** 실행.  
`s_pollBusy` / `s_pollDone` / `s_taskPollP` 플래그로 결과를 메인 루프에서 수신.

> ⚠️ 코어 0 고정 시 WiFi 스택 충돌 → 반드시 **코어 1**  
> ⚠️ 스택 10KB 부족 → `16384` 이상 설정

---

### 🟡 [T-07] 비근무시간 부팅 시 OLED 즉시 꺼지지 않음

**현상**: `off_hours_oled=off` 설정인데 비근무시간에 부팅하면 OLED가 꺼지지 않음.

**원인 1차**: `static bool prevWorking = true` 초기화로 인해 첫 루프에서 거짓 전환 발생.  
**원인 2차(잔존)**: `setup()` 실행 순서 — `disp.clear()` 호출 *후* `doPoll()` 을 호출하면 내부 `updateDisplay()` → `disp.showIdle()` 이 clear를 덮어씀.

**해결**:
1. `static bool prevWorking = isWorkingHours()` — 실제 현재 상태로 초기화하여 거짓 전환 방지
2. `setup()` 에서 비근무시간 초기 상태 적용 블록을 `doPoll()` **이후** 로 이동 — `doPoll()` 의 `showIdle()` 덮어쓰기 원천 차단

---

### 🟡 [T-08] 이메일 스크롤이 10초마다 처음으로 리셋

**현상**: 제목이 "변경" 부분까지만 스크롤되고 리셋. 발신자는 영원히 스크롤 안 됨.

**원인**: 10초마다 폴링 완료 → `updateDisplay()` → `showEmail()` 호출 → 스크롤 상태(`_scrollX=0`, `BOTTOM_PAUSE`) 초기화.  
제목 완주에 ~28초 필요한데 10초마다 리셋되어 발신자 스크롤 단계에 절대 도달 불가.

**해결**: `showEmail()` 에서 동일 발신자+제목이면 즉시 `return` (스크롤 유지).  
내용이 바뀐 새 메일이 왔을 때만 상태 초기화.

---

### 🟡 [T-09] 스크롤 후 긴 공백 — 리셋 조건 오설정

**현상**: 마지막 글자가 사라진 뒤 한참(6~13초) 공백 화면이 지속되다가 다음 스크롤 시작.

**원인**: 리셋 조건이 `_scrollX > _subjectPx + OLED_W` — 텍스트가 완전히 사라진 후 추가로 128px(= 128×50ms = 6.4초)를 더 스크롤하도록 설계됨.

**해결**: 조건을 `_scrollX > _subjectPx`로 변경 — 마지막 픽셀이 왼쪽 밖으로 나가는 순간 즉시 다음 단계로 전환.

---

### 🟡 [T-10] Wire.begin() 이중 호출 경고

**현상**: 부팅 시 `[W][Wire.cpp:301] begin(): Bus already started in Master Mode.` 경고.

**원인**: `Display::begin()` 에서 `Wire.begin(sda, scl)` 명시 호출 → `_u8g2.begin()` 내부에서 또 `Wire.begin()` 호출.

**해결**: `Wire.begin()` 직접 호출 제거. 대신 `u8x8_SetPin_HW_I2C(_u8g2.getU8x8(), ...)` 로 U8g2 내부 핀 테이블에 SDA/SCL을 미리 등록.  
`_u8g2.begin()` 이 정확한 핀으로 `Wire.begin(sda, scl)`을 **한 번만** 호출함.

---

### 🟡 [T-11] 07:30 근무시작 시 부저 없음 + 첫 폴링 최대 10초 지연

**현상**: 07:30 이 되어도 부저가 울리지 않고, 이메일 상태가 즉시 반영되지 않음(최대 10초 공백).

**원인 1 (부저)**: `loop()` 의 근무시작 전환 분기(`else` 브랜치)에 `triggerDoubleBeep()` 호출 없음.

**원인 2 (폴링 지연)**: 비근무시간에도 10초마다 `lastPoll = millis()` 를 갱신하기 때문에, 07:30이 되어도 마지막 갱신 후 10초가 차야 첫 폴링이 시작됨.

**해결**:
```cpp
// loop() 근무시작 전환 else 브랜치에 추가
triggerDoubleBeep();   // 출근 알림 부저
lastPoll = 0;          // 즉시 폴링 — 10초 대기 제거
```

---

## 14. 현재 알려진 제약 / TODO

| 항목 | 내용 |
|------|------|
| TLS 인증서 검증 안 함 | `client.setInsecure()` 사용. 내부망 장치 실용적 타협. 보안 강화 시 Root CA 추가 필요. |
| access_token 시리얼 출력 | `Oauth.cpp` 내 `Serial.println(_accessToken)` — 프로덕션 전 제거 권장. |
| 단일 메일박스 | `cfg::MY_EMAIL` 1개만 지원. |
| BLE 보안 없음 | NUS 무선 명령에 암호화/페어링 없음 — 내부 사무실 전용. 사용자 명시 요청에 의해 설계. |

---

## 15. 빠른 참고 — Serial 로그 키워드

| 키워드 | 의미 |
|--------|------|
| `[CFG]` | 부팅 시 Settings 로드 결과 (WiFi/밝기/폴링/근무시간) |
| `[BLE]` | BLE 연결/해제/명령 수신 |
| `[WIFI]` | WiFi 연결/재연결 상태 |
| `[NTP]` | 시간 동기화 |
| `[OAUTH]` | 토큰 발급/갱신 |
| `[GRAPH]` | Graph API 폴링 결과 |
| `[LED]` | LED 상태 전환 |
| `[TIME]` | 근무시간 전환 |
| `[SCHED]` | 예약 알림 발생 |
| `[MEM]` | 힙 부족 재부팅 |
| `[POLL]` | 비동기 폴링 태스크 상태 |
| `[DISP]` | OLED 이메일 표시 (sender/subjectPx 값 포함) |
| `[DIAG]` | OLED 진단 (주석 처리됨) |
