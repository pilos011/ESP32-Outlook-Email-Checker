# Changelog

모든 주요 변경 사항을 이 파일에 기록합니다.  
형식: [Keep a Changelog](https://keepachangelog.com/ko/1.0.0/)  
버전: [Semantic Versioning](https://semver.org/lang/ko/) (MAJOR.MINOR.PATCH)

---

## [v1.0.11] — 2026-06-29

### 버그 수정

**Device Code Flow 중 OLED 공백 + HTTPS 무한 대기 문제 수정**

- 증상: NVS에 refresh_token이 없는 상태(최초 부팅 또는 NVS 초기화 후)에서 전원 투입 시
  노란색 LED가 깜빡이고 OLED가 공백인 채 무한 대기, BLE `AUTH:` 명령도 동작 안 함
- 원인 1 — OLED 공백: `runDeviceCodeFlow()` 가 Microsoft 서버에 device code 를 요청하는
  HTTPS POST 응답을 받은 뒤 콜백(`disp.showAuth()`)을 호출하는 구조인데,
  `WiFiClientSecure` 에 타임아웃이 없어 서버 무응답 시 POST 자체가 영원히 블로킹됨
  → 콜백 호출 전 단계에서 멈춰 OLED가 비어있는 상태 유지
- 원인 2 — BLE 명령 불가: `runDeviceCodeFlow()` 가 `setup()` 안에서 블로킹 실행 중이므로
  `loop()` 가 시작되지 않아 BLE 명령 처리(`handleBleCommand()`)가 불가능
- 수정
  - `client.setTimeout(15)` 추가 (15초 타임아웃 → POST 무한 대기 방지)
  - `runDeviceCodeFlow()` 호출 전 `disp.showConnecting()` 추가
    → HTTPS 요청 중에도 OLED에 `"OAuth setup... / Connecting to MS"` 표시

**invalid_grant 수신 시 즉시 Device Code Flow 재실행**

- 증상: MFA 만료·비밀번호 변경 등으로 refresh_token이 영구 무효가 되면(HTTP 400 `invalid_grant`),
  기존 코드는 일시 장애로 오인해 5분 × 최대 3회 재부팅 반복 후 Orange LED 고착
- 수정
  - `_refreshAccess()`: HTTP 400 + `invalid_grant` 수신 시 NVS refresh_token 즉시 삭제 (`clearTokens()`)
  - `setup()`: `ensureAccessToken()` 실패 후 `hasRefreshToken() == false` 이면
    재시도 없이 즉시 Device Code Flow 재실행 (5분 대기 불필요)
- 로직 구분

| 실패 유형 | 처리 |
|-----------|------|
| `invalid_grant` (토큰 영구 무효) | NVS 삭제 → 즉시 재인증 |
| 그 외 (네트워크 일시 오류 등) | 기존 5분 × 3회 재시도 유지 |

**refresh_token 만료 시 OLED 재인증 안내 화면 추가**

- 증상: invalid_grant 처리 후 Device Code Flow로 전환 시 왜 재인증이 필요한지 OLED에 표시 없음
- 수정: `Display::showReauth()` 추가 — Device Code Flow 진입 전 3초간 표시

```
OLED 표시 순서:
  Orange LED + "MFA Expired / Re-auth needed"  (3초)
  → Yellow LED + "OAuth setup... / Connecting to MS"
  → "microsoft.com/devicelogin / XXXXXXXX"  (인증 코드 입력 대기)
```

이벤트 로그 태그 추가:

| 태그 | 설명 |
|------|------|
| `OAUTH_REAUTH` | refresh_token 무효(invalid_grant) → 재인증 절차 시작 |

---

## [v1.0.10] — 2026-06-09

### 버그 수정

**내가 보낸 메일(from = MY_EMAIL) 수신 시 알림 제외**

- 증상: 자신이 보낸 메일(예: 자기 자신에게 CC 포함)이 받은편지함에 수신될 경우
  미확인 메일로 집계되어 LED 점등 및 OLED 알림이 발생
- 수정: `Outlook::_analyze()` 에서 `from.emailAddress.address == cfg::MY_EMAIL` 인 메일은 카운트 제외

**비근무시간 WiFi 복구 후 에러 LED·OLED가 07:30까지 유지되는 버그 수정**

- 증상: 비근무시간(예: 새벽)에 WiFi가 끊겼다 자동 복구되어도
  하얀색 LED + "WiFi Connect Error." OLED가 다음 날 07:30 WORK_START까지 그대로 유지됨
- 원인: WiFi 재연결 성공 시 근무시간(`isWorkingHours() == true`) 분기에만 LED·OLED를 복원하고
  비근무시간 분기(`else`)가 없어 `connectWifi()` 가 설정한 `WHITE_PULSE` 상태가 유지됨
- 수정: 비근무시간 `else` 분기 추가 — 정상 재연결·dead 재시도 성공 두 경로 모두 처리
  - LED → OFF
  - OLED → `clear()` 또는 `showIdle()` (`OLED_OFF_HOURS_OFF` 설정에 따라)

---

## [v1.0.9] — 2026-06-08

### 버그 수정

**17:00 퇴근 시 OLED가 이메일 화면 그대로 남는 문제 수정**

- 증상: 17:00 WORK_END 전이 시 LED는 정상 소등되나, OLED는 발신자·제목 스크롤 화면이 그대로 유지됨
- 원인: `loop()` 실행 순서 문제
  1. WORK_END 전이 → `disp.showIdle()` (시계로 전환) ✓
  2. 같은 루프에서 `s_pollDone` 처리 → `updateDisplay(p)` → `disp.showEmail()` **덮어씀** ✗
  - `applyPriority()`는 내부에 `isWorkingHours()` 가드가 있어 LED는 올바르게 동작했지만,
    `updateDisplay()`에는 동일한 가드가 없었음
- 수정: `s_pollDone` 결과 적용 시 `isWorkingHours()` 가드 추가
  - `updateDisplay(p)` 및 `led.flashOnce()` 를 `isWorkingHours()` 조건 하에서만 실행
- 추가 수정: WiFi 재연결 성공 시 LED·OLED 복원도 `isWorkingHours()` 조건 추가
  - 비근무시간에 WiFi가 끊겼다 재연결되면 LED가 다시 켜지는 잠재 버그 방지

---

## [v1.0.8] — 2026-06-08

### 버그 수정

**OAuth 토큰 갱신 실패 시 5분마다 무한 재부팅 + 부저 반복 문제 수정**

- 증상: OAuth 토큰 갱신에 지속적으로 실패하면 5분마다 삑 1회(자가테스트) + 재부팅을 무한 반복
- 수정: RTC 메모리(`RTC_DATA_ATTR`)로 연속 실패 횟수를 추적
  - 1~2회 실패: 기존과 동일하게 5분 후 재부팅 (`REBOOT_OAUTH` 로그)
  - 3회 연속 실패: 재부팅 포기 → `OAUTH_DEAD` 로그 기록
    - Orange LED 점멸 (WiFi 에러와 동일 색상으로 통일)
    - OLED: `OAuth Error` / `BLE: REBOOT` 안내 화면 고정
    - 자동 재시도 없음 — BLE `REBOOT` 명령 또는 파워사이클로 복구
    - 폴링·근무시간 LED/OLED 전환 중단 (BLE 콘솔은 유지)
- 추가: `Display::showOAuthError()` 메서드 신규 추가

이벤트 로그 태그:

| 태그 | 설명 |
|------|------|
| `REBOOT_OAUTH` | OAuth 갱신 실패 (1~2회) → 5분 후 재부팅 |
| `OAUTH_DEAD` | OAuth 갱신 3회 연속 실패 → 재부팅 포기 |

---

## [v1.0.7] — 2026-06-08

### 버그 수정

**07:30 이전에 WORK_START가 반복 발화되어 부저가 계속 울리는 문제 수정**

- 증상: 07:30 이전(예: 07:17)에 부저가 반복적으로 울리고, 이벤트 로그에 `WORK_START`가 20건 이상 기록됨
- 원인: `isWorkingHours()` 내부에서 `getLocalTime(&t, 0)` 이 실패할 때 `return true`(안전값)를 반환하는 로직이 진짜 문제를 유발함
  - WiFi 재연결 시 `connectWifi()`가 `WiFi.disconnect(true)` → `WiFi.mode(WIFI_STA)` → `WiFi.begin()` 을 반복 호출
  - 이 과정에서 LWIP/SNTP 스택이 재초기화되며 `getLocalTime` 이 일시적으로 실패
  - 실패 시 `return true` → `isWorkingHours()` = 근무시간 → WORK_START 전이 발화 + 부저
  - 다음 루프에서 `getLocalTime` 성공 → 실제 시각 07:17 < 07:30 → `return false` → WORK_END 전이
  - 이 두 상태가 루프마다 교번하며 WORK_START가 수십 회 반복 발화
- 수정: `isWorkingHours()` 에 `static bool s_lastKnown = false` 캐시 추가
  - `getLocalTime` 성공 시: 결과를 `s_lastKnown` 에 저장 후 반환
  - `getLocalTime` 실패 시: `s_lastKnown` (마지막 유효 상태) 반환 → 전환 억제
  - 효과: 시간이 일시적으로 불확실해도 `prevWorking` 이 변하지 않아 거짓 전이 없음
- 부가 수정: `WORK_START` 이벤트 로그에 실제 발화 시각(HH:MM) 추가
  - 예: `WORK_START 07:30` → 정상 / `WORK_START 07:17` → 비정상(진단 가능)

---

## [v1.0.6] — 2026-06-05

### 신기능

**NVS 이벤트 로그 — 재부팅 후에도 유지, BLE로 조회**
- 시리얼 연결 없이 주요 이벤트를 추적하기 위해 NVS 기반 순환 로그 추가
- 최대 30건 보관, 초과 시 가장 오래된 항목 자동 덮어씀
- 재부팅 후에도 기록 유지 (NVS Flash 저장)

기록되는 이벤트:

| 태그 | 설명 |
|------|------|
| `BOOT` | 재부팅 원인 (POWERON / SW / PANIC / WDT / BROWNOUT 등) |
| `BOOT_OK` | setup() 정상 완료 |
| `WIFI_OK` | WiFi 연결 성공 + IP |
| `WIFI_FAIL` | 부팅 시 WiFi 연결 실패 |
| `WIFI_DEAD` | 3회 연속 실패 → 포기 |
| `WIFI_BACK` | 30분 재시도 성공 → 정상 복귀 |
| `REBOOT_OAUTH` | OAuth 토큰 갱신 실패 → 5분 후 재부팅 |
| `REBOOT_HEAP` | 힙 부족(< 30KB) → 즉시 재부팅 |
| `REBOOT_24H` | 24시간 정기 재부팅 |
| `WORK_START` | 07:30 근무 시작 전환 |
| `BEEP_SCHED` | 예약 알림 부저 발생 |

BLE 명령:
- `LOG` → 전체 로그 조회 (nRF Connect TX에서 수신)
- `LOG:clear` → 로그 전체 삭제

---

## [v1.0.5] — 2026-06-04

### 개선

**WiFi 포기 후 30분마다 자동 재시도**
- 증상: 3회 연속 WiFi 연결 실패 시 "WiFi Connect Error." 화면이 표시되고 재부팅하기 전까지 복구 불가능 (월요일 출근 시 에러 화면 발견)
- 수정: 포기(`s_wifiDead=true`) 상태에서도 30분마다 단발 재시도
  - 재시도 성공 → `s_wifiDead=false`, LED·OLED 정상 상태로 자동 복귀
  - 재시도 실패 → 30분 후 재예약, 에러 화면 유지
  - 단발 시도이므로 3회 카운트와 무관 (연결 가능해지면 즉시 회복)

---

## [v1.0.4] — 2026-06-01

### 버그 수정

**07:30 근무 시작 시 OLED가 시계 그대로 남아 있는 문제 수정**
- 증상: 07:30 전에 온 미확인 메일이 있는 상태에서 07:30이 되면 LED는 녹색으로 정상 점등되나, OLED는 시계 화면 그대로 유지됨
- 원인: 근무 시작 전환 코드가 `restoreLed(lastPriority)` 로 LED를 복원하면서 OLED는 `disp.showIdle()`(시계)로 무조건 초기화함. 이후 첫 폴링(2~5초 소요)이 완료돼야 `updateDisplay()`가 호출되므로 그 사이 LED·OLED 불일치 상태 지속. 폴링 실패 시 OLED가 영구적으로 시계 화면에 고착됨
- 수정: `disp.showIdle()` → `updateDisplay(lastPriority)` 로 변경. LED와 OLED가 즉시·동시에 동일한 lastPriority 기준으로 복원됨 (메일 있으면 이메일 화면, 없으면 시계)

**비근무시간 부팅 시 메일이 있으면 OLED에 이메일 화면이 표시되는 문제 수정**
- 증상: 비근무시간에 부팅하면 `doPoll()`이 미확인 메일을 찾아 `showEmail()`을 호출, OLED가 이메일 화면을 표시함 (비근무시간이므로 시계가 맞음)
- 원인: 비근무시간 부팅 후처리 블록이 `OLED_OFF_HOURS_OFF=false`(idle) 일 때 OLED를 시계로 복원하지 않았음. 주석 "showIdle이 이미 호출됐으므로 유지" 가 틀린 전제였음
- 수정: `else { disp.showIdle(); }` 추가 → 비근무시간 부팅 후 항상 시계로 복원

---

## [v1.0.3] — 2026-05-28

### 신기능

**BLE 앱수준 PIN 인증**
- BLE 연결 후 첫 명령은 반드시 `AUTH:PIN번호` (예: `AUTH:1234`)
- 잘못된 PIN → `ERR: 잘못된 PIN`; 미인증 명령 → `ERR: 인증 필요 → AUTH:PIN번호`
- BLE 연결 해제 시 인증 상태 자동 초기화 (재연결 시 재인증 필요)
- PIN은 `config.ini` `[ble]` 섹션의 `pin` 키로 설정 (컴파일 시 적용)

### 버그 수정

**최초 부팅 시 NVS `NOT_FOUND` 에러 로그 수정**
- 증상: 최초 부팅 또는 `RESET` 후 `[E][Preferences.cpp:50] begin(): nvs_open failed: NOT_FOUND` ×2 출력
- 원인: `Settings::init()`에서 `_prefs.begin(NVS_NS, readOnly=true)` 사용 시, NVS 네임스페이스가 아직 없으면 `nvs_open(NVS_READONLY)` 실패
- 수정: `readOnly=false` (read-write)로 오픈 → 최초 부팅에도 네임스페이스 자동 생성, 에러 없음

**`No core dump partition found` 부팅 에러 수정**
- 증상: 부팅 시 `E (302) esp_core_dump_flash: No core dump partition found!` ×2 출력
- 원인: 파티션 테이블에 coredump 파티션 없음
- 수정: `partitions_16MB.csv`에 64 KB coredump 파티션 추가 (`0x810000`)
- ⚠️ **파티션 테이블 변경**: 다음 플래시 시 "Erase Flash" 또는 전체 재플래시 필요

---

## [v1.0.2] — 2026-05-28

### 버그 수정

**WiFi + BLE 동시 사용 시 abort() 크래시 수정**
- 증상: BLE 광고 시작(`Ble::begin`) 후 `connectWifi()` 호출 시 즉시 `abort()` → 재부팅 반복
- 원인: `WiFi.setSleep(false)` 호출이 BLE 활성 상태에서 ESP-IDF에 의해 금지됨. WiFi와 BLE 동시 사용 시 모뎀 슬립이 필수이며, 이를 끄면 `pm_set_sleep_type`에서 `abort()` 발생
- 수정: `WiFi.setSleep(false)` → `WiFi.setSleep(true)` (10초 폴링 주기에서 레이턴시 영향 없음)

---

## [v1.0.1] — 2026-05-28

### 버그 수정

**근무시간 중 24시간 정기 재부팅 방지**
- 증상: 기기를 근무시간(예: 08:15)에 처음 켜면, 정확히 24시간 후 근무시간 중에 재부팅이 발생하여 메일 알림 상태가 초기화됨
- 원인: `millis() > 86400000UL` 조건이 근무시간 여부를 고려하지 않음
- 수정: 비근무시간까지 재부팅 유예. 25시간 초과 시 NTP 미동기화 대비 강제 재부팅

**폴링 태스크 생성 실패 시 동기 폴백으로 메일 감지 보장**
- 증상: 장시간(23h+) 구동 후 FreeRTOS 힙 단편화로 `xTaskCreatePinnedToCore(16KB)` 실패 → 07:30 근무 시작 후에도 폴링이 계속 실패 → 메일 미감지, OLED 시계만 표시
- 원인: 태스크 생성 실패 시 10초 후 재시도하지만, 힙 상태가 동일하면 계속 실패
- 수정: 태스크 생성 실패 시 `doPoll()`을 동기로 즉시 호출하여 메일 감지 보장

---

## [v1.0.0] — 2026-05-27

### 최초 릴리즈

**하드웨어**
- ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM)
- WS2812 RGB LED (GPIO 48)
- Passive Buzzer (GPIO 13)
- 128×32 SSD1306 OLED (SDA=8, SCL=9)

**핵심 기능**
- Microsoft Outlook Graph API OAuth 2.0 (Device Code Flow) 인증
- 받은편지함 폴링 → VIP·단독수신·일반 미확인 메일 우선순위 분류
- 우선순위별 RGB LED (빨강 점멸 / 파랑 점멸 / 초록 고정)
- 메일 도착 비프 알림, OLED 발신자/제목 스크롤 표시
- 근무시간(월~금 07:30~17:00) 외 LED 소등, OLED 시계 표시
- 예약 알림 부저 (최대 8개, config.ini 또는 BLE로 설정)
- FreeRTOS 비동기 폴링 (OLED 시계 블로킹 방지)
- PSRAM 활용 HTTP/JSON 처리 (64KB 버퍼)

**BLE NUS 무선 설정 콘솔** (nRF Connect 앱)
- 장치명: `OutlookLED`
- 런타임 설정 변경 + NVS 영구 저장 (재부팅 후 유지)
- 지원 명령: STATUS / WIFI / BRIGHT / BLINK / POLL / WORK / VIP / SCHED / RESET / REBOOT

**3계층 설정 관리**
1. `config.ini` → 빌드 시 `config_generated.h` 자동 생성 (컴파일 기본값)
2. NVS (Non-Volatile Storage) — 부팅 시 오버라이드
3. BLE NUS — 런타임 즉시 변경 + NVS 저장

**BLE 명령 입력 검증 (버그 수정 포함)**
- `BRIGHT:abc` 비숫자 입력 차단 (0 무음 설정 방지)
- `WORK:-100:1700` 음수 입력 차단 (근무시간 오염 방지)
- `SCHED:del:abc` 비숫자 입력 차단 (인덱스 0 오삭제 방지)
- `SCHED:del` 예약 0개일 때 `"0~-1"` 오류 메시지 수정
- 예약 스케줄 듀얼 키 중복 방지 (분+날짜 조합 → 날짜 변경 시 재발화 허용)
- NVS poll_sec=0/blink_ms=0 로드 시 기본값 복원
