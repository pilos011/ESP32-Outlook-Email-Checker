# Changelog

모든 주요 변경 사항을 이 파일에 기록합니다.  
형식: [Keep a Changelog](https://keepachangelog.com/ko/1.0.0/)  
버전: [Semantic Versioning](https://semver.org/lang/ko/) (MAJOR.MINOR.PATCH)

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
