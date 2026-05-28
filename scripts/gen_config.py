# scripts/gen_config.py
# 빌드 직전에 ../config.ini -> ../src/config_generated.h 로 변환
# 별도 파일시스템 사용 안 함. 펌웨어에 const char* 로 박힘.

import configparser
import os
from pathlib import Path

Import("env")  # PlatformIO injects this

PROJECT_DIR = Path(env["PROJECT_DIR"])
CONFIG_INI = PROJECT_DIR / "config.ini"
OUTPUT_H   = PROJECT_DIR / "src" / "config_generated.h"


def c_escape(s: str) -> str:
    if s is None:
        return ""
    return (s.replace("\\", "\\\\")
             .replace('"', '\\"')
             .replace("\n", "\\n")
             .replace("\r", ""))


def get(cfg, section, key, default=""):
    try:
        return cfg.get(section, key).strip()
    except Exception:
        return default


def parse_hhmm(s: str, default_min: int = 0) -> int:
    """'HH:MM' 문자열을 자정 이후 분(minute)으로 변환"""
    try:
        h, m = s.strip().split(":")
        return int(h) * 60 + int(m)
    except Exception:
        return default_min


def main():
    if not CONFIG_INI.exists():
        print(f"!! config.ini not found at {CONFIG_INI}")
        print("   복사할 템플릿: config.ini.example")
        env.Exit(1)

    cfg = configparser.ConfigParser(inline_comment_prefixes=(";", "#"))
    cfg.read(CONFIG_INI, encoding="utf-8")

    wifi_ssid     = get(cfg, "wifi",     "ssid")
    wifi_pass     = get(cfg, "wifi",     "password")
    my_email      = get(cfg, "outlook",  "my_email").lower()
    vip_csv       = get(cfg, "outlook",  "vip_senders").lower()
    client_id     = get(cfg, "outlook",  "client_id")
    tenant        = get(cfg, "outlook",  "tenant", "consumers")
    poll_interval = get(cfg, "poll",     "interval_sec", "30")
    top_n         = get(cfg, "poll",     "top_n",        "5")
    led_pin       = get(cfg, "led",      "pin",          "48")
    led_bright    = get(cfg, "led",      "brightness",   "80")
    blink_ms      = get(cfg, "led",      "blink_ms",     "500")
    buz_pin       = get(cfg, "buzzer",   "pin",          "21")
    buz_volume    = get(cfg, "buzzer",   "volume",       "50")
    buz_freq      = get(cfg, "buzzer",   "freq",         "2700")
    buz_duration  = get(cfg, "buzzer",   "duration_ms",  "200")
    ble_pin            = get(cfg, "ble",  "pin",              "1234")

    oled_sda           = get(cfg, "oled", "sda",              "8")
    oled_scl           = get(cfg, "oled", "scl",              "9")
    off_hours_oled_str = get(cfg, "oled", "off_hours_oled",   "idle")
    oled_off_hours_off = off_hours_oled_str.lower().strip() == "off"
    scroll_subject_ms  = get(cfg, "oled", "scroll_subject_ms", "50")
    scroll_sender_ms   = get(cfg, "oled", "scroll_sender_ms",  "50")

    # [schedule]
    work_hours_only_str = get(cfg, "schedule", "work_hours_only", "false")
    work_hours_only     = work_hours_only_str.lower() in ("true", "1", "yes")
    work_start_str      = get(cfg, "schedule", "work_start", "07:30")
    work_end_str        = get(cfg, "schedule", "work_end",   "17:00")
    work_start_min      = parse_hhmm(work_start_str, 7 * 60 + 30)
    work_end_min        = parse_hhmm(work_end_str,  17 * 60)

    # [schedule] 예약 알림 파싱 (최대 8개, 형식 "HHMM:w")
    try:
        sched_n = int(get(cfg, "schedule", "sched_count", "0"))
    except ValueError:
        sched_n = 0
    scheds = []
    for i in range(min(sched_n, 8)):
        val = get(cfg, "schedule", f"sched_{i}", "").strip()
        parts = val.split(":")
        if len(parts) == 2 and len(parts[0]) == 4:
            try:
                hour = int(parts[0][:2]); minn = int(parts[0][2:])
                wday = int(parts[1].strip())
                if 0 <= hour <= 23 and 0 <= minn <= 59 and 0 <= wday <= 7:
                    scheds.append((hour, minn, wday))
            except ValueError:
                pass

    def _pad8(lst, default=0):
        return ", ".join(str(x) for x in (lst + [default] * 8)[:8])
    sched_hours = _pad8([s[0] for s in scheds])
    sched_mins  = _pad8([s[1] for s in scheds])
    sched_wdays = _pad8([s[2] for s in scheds])

    # vip_senders → C 배열
    vip_list = [v.strip() for v in vip_csv.split(",") if v.strip()]
    if vip_list:
        vip_array = ",\n  ".join(f'"{c_escape(v)}"' for v in vip_list)
    else:
        vip_array = '""'

    bool_str = lambda b: "true" if b else "false"

    header = f"""// AUTO-GENERATED FROM config.ini — DO NOT EDIT
// 빌드할 때마다 scripts/gen_config.py 가 덮어씁니다.
#pragma once

namespace cfg {{

constexpr const char* WIFI_SSID  = "{c_escape(wifi_ssid)}";
constexpr const char* WIFI_PASS  = "{c_escape(wifi_pass)}";

constexpr const char* BLE_PIN    = "{c_escape(ble_pin)}";   // BLE 설정 콘솔 접근 PIN

constexpr const char* MY_EMAIL   = "{c_escape(my_email)}";
constexpr const char* CLIENT_ID  = "{c_escape(client_id)}";
constexpr const char* TENANT     = "{c_escape(tenant)}";

constexpr const char* VIP_SENDERS[] = {{
  {vip_array}
}};
constexpr int VIP_SENDERS_COUNT = {len(vip_list)};

constexpr unsigned long POLL_INTERVAL_MS = {int(poll_interval)}UL * 1000UL;
constexpr int  TOP_N         = {int(top_n)};

constexpr int  LED_PIN       = {int(led_pin)};
constexpr int  LED_BRIGHTNESS= {int(led_bright)};
constexpr int  BLINK_MS      = {int(blink_ms)};

constexpr int  BUZ_PIN          = {int(buz_pin)};
constexpr int  BUZ_VOLUME       = {int(buz_volume)};
constexpr int  BUZ_FREQ         = {int(buz_freq)};
constexpr int  BUZ_DURATION_MS  = {int(buz_duration)};

constexpr int  OLED_SDA      = {int(oled_sda)};
constexpr int  OLED_SCL      = {int(oled_scl)};
constexpr bool OLED_OFF_HOURS_OFF = {bool_str(oled_off_hours_off)};  // true=화면끄기 / false=시계표시
constexpr unsigned long SCROLL_SUBJECT_MS = {int(scroll_subject_ms)}UL;  // 제목 스크롤 속도 (ms/px)
constexpr unsigned long SCROLL_SENDER_MS  = {int(scroll_sender_ms)}UL;   // 발신자 스크롤 속도 (ms/px)

// [schedule] — 근무시간 제어
constexpr bool WORK_HOURS_ONLY = {bool_str(work_hours_only)};
constexpr int  WORK_START_MIN  = {work_start_min};   // {work_start_str} → 자정 이후 {work_start_min}분
constexpr int  WORK_END_MIN    = {work_end_min};   // {work_end_str} → 자정 이후 {work_end_min}분

// [schedule] — 예약 알림 기본값 (BLE로 런타임 변경 가능, 최대 8개)
constexpr int     SCHED_COUNT_DEFAULT   = {len(scheds)};
constexpr uint8_t SCHED_HOUR_DEFAULT[8] = {{ {sched_hours} }};
constexpr uint8_t SCHED_MIN_DEFAULT[8]  = {{ {sched_mins} }};
constexpr uint8_t SCHED_WDAY_DEFAULT[8] = {{ {sched_wdays} }};

}} // namespace cfg
"""

    OUTPUT_H.parent.mkdir(parents=True, exist_ok=True)
    if OUTPUT_H.exists() and OUTPUT_H.read_text(encoding="utf-8") == header:
        print(f">> config_generated.h 변경 없음")
        return

    OUTPUT_H.write_text(header, encoding="utf-8")
    print(f">> generated {OUTPUT_H.relative_to(PROJECT_DIR)}")
    print(f"   ble.pin            = {'*' * len(ble_pin)} ({len(ble_pin)}자리)")
    print(f"   wifi.ssid          = {wifi_ssid}")
    print(f"   outlook.my_email   = {my_email}")
    print(f"   outlook.tenant     = {tenant}")
    print(f"   vip senders        = {vip_list}")
    print(f"   poll.top_n         = {top_n}")
    print(f"   led pin/brightn    = {led_pin} / {led_bright}")
    print(f"   buzzer pin/vol     = {buz_pin} / {buz_volume}  ({buz_freq}Hz, {buz_duration}ms)")
    print(f"   work_hours_only    = {work_hours_only}")
    print(f"   work_start~end     = {work_start_str} ~ {work_end_str}")
    print(f"   off_hours_oled     = {'off' if oled_off_hours_off else 'idle'}")
    print(f"   scroll subject/sender = {scroll_subject_ms}ms / {scroll_sender_ms}ms per px")
    print(f"   schedules          = {len(scheds)}개: " +
          ", ".join(f"{s[0]:02d}:{s[1]:02d}(w={s[2]})" for s in scheds))


main()
