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
    Serial.println("[BLE] 연결 끊김 → 재광고");
    BLEDevice::startAdvertising();
  }
};

// ── RX 수신 콜백 (BLE 태스크에서 호출됨) ────────────────────────────────────
class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String val = c->getValue().c_str();
    for (char ch : val) {
      if (ch == '\n' || ch == '\r') {
        String cmd = s_cmdBuf;
        s_cmdBuf = "";
        cmd.trim();
        if (cmd.length() == 0) continue;
        portENTER_CRITICAL(&s_mux);
        if (!s_hasPending) {     // 이전 명령이 아직 처리 중이면 드롭
          s_pendingCmd = cmd;
          s_hasPending = true;
        }
        portEXIT_CRITICAL(&s_mux);
        Serial.printf("[BLE] rx: %s\n", cmd.c_str());
      } else {
        if (s_cmdBuf.length() < 128) s_cmdBuf += ch;   // 오버플로 방지
      }
    }
  }
};

// ── 공개 인터페이스 ───────────────────────────────────────────────────────────
namespace Ble {

void begin(const char* name) {
  BLEDevice::init(name);
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
  // 100바이트씩 청크 전송 (BLE 스택이 MTU에 맞게 분할)
  const size_t CHUNK = 100;
  for (size_t i = 0; i < out.length(); i += CHUNK) {
    String part = out.substring(i, i + CHUNK);
    s_txChar->setValue(part.c_str());
    s_txChar->notify();
    delay(15);
  }
}

bool isConnected() { return s_connected; }

} // namespace Ble
