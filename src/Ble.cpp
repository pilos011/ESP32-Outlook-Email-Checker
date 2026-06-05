// src/Ble.cpp — BLE NUS 서버 구현
// Service  UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
// RX Char  UUID: 6E400002 (폰→장치, Write)
// TX Char  UUID: 6E400003 (장치→폰, Notify)
#include "Ble.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define NUS_SVC_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

static BLEServer*         s_server    = nullptr;
static BLECharacteristic* s_txChar    = nullptr;
static volatile bool      s_connected = false;

// ── 협상된 ATT MTU (연결마다 갱신, 기본 최솟값 23) ───────────────────────────
// 페이로드 = s_mtu - 3 바이트 (ATT 헤더 3바이트 제외)
// ex) MTU=247 → 244바이트 페이로드, MTU=512 → 509바이트 페이로드
static uint16_t s_mtu = 23;

// ── 명령 큐 (BLE 태스크 → 메인 루프, portMUX 보호) ─────────────────────────
static portMUX_TYPE  s_mux        = portMUX_INITIALIZER_UNLOCKED;
static String        s_cmdBuf;         // 수신 중 누적 버퍼
static String        s_pendingCmd;     // 완성된 한 줄 명령
static volatile bool s_hasPending = false;

// ── BLE 서버 이벤트 콜백 ─────────────────────────────────────────────────────
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    s_connected = true;
    Serial.println("[BLE] 연결됨");
  }
  void onDisconnect(BLEServer*) override {
    s_connected = false;
    s_mtu = 23;   // 연결 해제 시 MTU 초기화
    Serial.println("[BLE] 연결 끊김 → 재광고");
    BLEDevice::startAdvertising();
  }
  // 폰이 ATT MTU Exchange를 요청하면 호출됨
  // nRF Connect(Android/iOS)는 연결 직후 자동으로 MTU 협상을 시작함
  void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
    s_mtu = param->mtu.mtu;
    Serial.printf("[BLE] MTU 협상 완료: %u → 페이로드 %u바이트/알림\n",
                  s_mtu, s_mtu - 3u);
  }
};

// ── RX 수신 콜백 (BLE 태스크에서 호출됨) ────────────────────────────────────
static void _dispatchCmd(const String& raw) {
  String cmd = raw;
  cmd.trim();
  if (cmd.length() == 0) return;
  portENTER_CRITICAL(&s_mux);
  if (!s_hasPending) {     // 이전 명령이 아직 처리 중이면 드롭
    s_pendingCmd = cmd;
    s_hasPending = true;
  }
  portEXIT_CRITICAL(&s_mux);
  Serial.printf("[BLE] rx: %s\n", cmd.c_str());
}

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String val = c->getValue().c_str();
    bool hadNewline = false;

    for (char ch : val) {
      if (ch == '\n' || ch == '\r') {
        hadNewline = true;
        _dispatchCmd(s_cmdBuf);
        s_cmdBuf = "";
      } else {
        if (s_cmdBuf.length() < 128) s_cmdBuf += ch;   // 오버플로 방지
      }
    }

    // 개행 없이 단일 Write로 전송된 경우 (nRF Connect TEXT 모드 등)
    // — 한 번의 Write = 하나의 완성된 명령으로 처리
    if (!hadNewline && s_cmdBuf.length() > 0) {
      _dispatchCmd(s_cmdBuf);
      s_cmdBuf = "";
    }
  }
};

// ── 공개 인터페이스 ───────────────────────────────────────────────────────────
namespace Ble {

void begin(const char* name) {
  BLEDevice::init(name);
  // setMTU는 반드시 init() 후에 호출해야 함
  // esp_ble_gatt_set_local_mtu()는 Bluedroid 활성화 후에만 유효
  // → 폰(클라이언트)이 MTU Exchange를 요청하면 min(512, 폰MTU)로 협상됨
  // → nRF Connect(Android): 보통 247 또는 517 요청
  BLEDevice::setMTU(512);

  s_server = BLEDevice::createServer();
  s_server->setCallbacks(new ServerCB());

  BLEService* svc = s_server->createService(NUS_SVC_UUID);

  // TX: 장치 → 폰 (Notify)
  s_txChar = svc->createCharacteristic(
      NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  s_txChar->addDescriptor(new BLE2902());

  // RX: 폰 → 장치 (Write / Write Without Response)
  BLECharacteristic* rxChar = svc->createCharacteristic(
      NUS_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_NR);
  rxChar->setCallbacks(new RxCB());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SVC_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  Serial.printf("[BLE] 광고 시작: %s\n", name);
}

bool hasPendingCommand() {
  return s_hasPending;
}

String dequeueCommand() {
  String cmd;
  portENTER_CRITICAL(&s_mux);
  cmd        = s_pendingCmd;
  s_hasPending = false;
  portEXIT_CRITICAL(&s_mux);
  return cmd;
}

void send(const String& msg) {
  if (!s_connected || !s_txChar) return;
  // 개행 보장
  String out = msg.endsWith("\n") ? msg : msg + "\n";

  // ATT 알림 한 건당 페이로드 = 협상된 MTU - 3 (ATT 헤더)
  // MTU 협상 전(기본 23) → CHUNK=20, 협상 후(247 등) → CHUNK=244
  // → 항상 MTU에 맞게 자동 조정되므로 잘림 없이 최대 효율 전송
  const size_t CHUNK = (s_mtu > 3u) ? static_cast<size_t>(s_mtu - 3u) : 20u;

  for (size_t i = 0; i < out.length(); i += CHUNK) {
    String part = out.substring(i, i + CHUNK);
    s_txChar->setValue(part.c_str());
    s_txChar->notify();
    delay(15);
  }
}

bool isConnected() { return s_connected; }

} // namespace Ble
