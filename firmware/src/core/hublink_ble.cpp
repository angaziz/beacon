#include "core/hublink_ble.h"
#include "core/hub_proto.h"
#include "util/log.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <esp_random.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

// NOTE: the pioarduino esp32s3 libs back the Arduino BLE API with NimBLE (CONFIG_BT_NIMBLE_ENABLED),
// not Bluedroid. We use only the stack-agnostic BLE* wrapper (no raw esp_ble_* / esp_gap_ble_api.h),
// so the transport works on either backing. tech.md §5 names Bluedroid canonical; see the P2 plan's
// status note on this toolchain reality. HubLink keeps the screens unaware of the stack either way.

// NUS UUIDs (tech.md §7.1). Device = peripheral; RX = central->device (write), TX = device->central (notify).
#define NUS_SVC "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Outbound notify chunking. We size each notify to the negotiated ATT payload (MTU - 3) at send time,
// not a fixed 20 B: a fixed 20 B forces ~65 back-to-back notify() calls for a large frame (e.g. the
// #105 ticker report), which overruns the controller's notify buffer under WiFi+BLE coexistence and
// silently drops/corrupts chunks. MTU-sized chunks cut that to a handful. HUB_TX_CHUNK_MIN is the safe
// ATT-default payload used until the central upgrades the MTU; HUB_TX_CHUNK_MAX bounds the on-stack
// chunk buffer (247 MTU - 3). The hub reassembles on '\n' regardless of chunk size.
#define HUB_TX_CHUNK_MIN 20
#define HUB_TX_CHUNK_MAX 244
#define HUB_SB_BYTES 2048   // inbound + outbound stream buffers

static BLEServer*         s_server  = nullptr;
static BLECharacteristic* s_tx      = nullptr;
static StreamBufferHandle_t s_in    = nullptr;   // BT-task writes, loop() reads
static StreamBufferHandle_t s_out   = nullptr;   // send() writes, loop() drains/notifies
static hub_frame_cb       s_frame_cb = nullptr;
static hub_reassembler_t  s_reasm;
static volatile bool      s_connected = false;
static volatile bool      s_pairing   = false;
static uint32_t           s_passkey   = 0;
static bool               s_inited    = false;

// --- BLE callbacks (run in the BT stack task: copy + return only, never touch the DataStore) ---

class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    const uint8_t* d = c->getData();
    size_t n = c->getLength();
    if (d && n && s_in) xStreamBufferSend(s_in, d, n, 0);   // drop on full; the hub will resend
  }
};

class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { s_connected = true;  LOGI("hub: central connected"); }
  void onDisconnect(BLEServer* s) override {
    s_connected = false; s_pairing = false;
    LOGW("hub: central disconnected; re-advertising");
    s->getAdvertising()->start();                            // Bluedroid needs a manual re-advertise
  }
};

class SecCb : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return s_passkey; }            // DisplayOnly: we own the key
  void onPassKeyNotify(uint32_t pass) override { s_passkey = pass; s_pairing = true; LOGI("hub: passkey %06u", (unsigned)pass); }
  bool onConfirmPIN(uint32_t) override { return true; }
  bool onSecurityRequest() override { s_pairing = true; return true; }  // accept bonding
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {     // NimBLE callback signature
    s_pairing = false;
    LOGI("hub: pairing %s", (desc && desc->sec_state.encrypted) ? "OK (bonded)" : "done");
  }
};

static void reasm_emit(const char* line, size_t len, void*) {
  if (s_frame_cb) s_frame_cb(line, len);
}

uint32_t hublink_ble_passkey(void)  { return s_passkey; }
bool     hublink_ble_is_pairing(void) { return s_pairing; }

bool HubLinkBle::begin() {
  if (s_inited) return true;

  s_in  = xStreamBufferCreate(HUB_SB_BYTES, 1);
  s_out = xStreamBufferCreate(HUB_SB_BYTES, 1);
  if (!s_in || !s_out) { LOGE("hub: stream buffer alloc failed"); return false; }
  hub_reassembler_reset(&s_reasm);

  char name[20];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(name, sizeof(name), "Beacon-%04X", (unsigned)(mac & 0xFFFF));
  BLEDevice::init(name);
  BLEDevice::setMTU(247);                       // request larger MTU; loop() sizes notifies to the negotiated value

  // Security: LE Secure Connections + bonding. Default = MITM passkey (IO cap DisplayOnly); build with
  // -DHUBLINK_JUSTWORKS for the Just-Works fallback (still bonded + encrypted, no MITM) per D3.
  s_passkey = (esp_random() % 900000u) + 100000u;   // static 6-digit passkey, displayed while pairing
  BLEDevice::setSecurityCallbacks(new SecCb());
  BLESecurity sec;
#ifdef HUBLINK_JUSTWORKS
  sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);   // bonded + encrypted, no MITM (D3 fallback)
  sec.setCapability(ESP_IO_CAP_NONE);
#else
  sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  sec.setCapability(ESP_IO_CAP_OUT);            // DisplayOnly: device shows the passkey, Mac enters it
  sec.setPassKey(true, s_passkey);              // static passkey shown on the pair overlay
#endif
  sec.setKeySize(16);

  s_server = BLEDevice::createServer();
  s_server->setCallbacks(new SrvCb());
  BLEService* svc = s_server->createService(NUS_SVC);

  BLECharacteristic* rx = svc->createCharacteristic(
      NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);   // non-bonded central cannot write
  rx->setCallbacks(new RxCb());

  s_tx = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
  s_tx->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  s_tx->addDescriptor(new BLE2902());                        // CCCD so the central can subscribe

  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SVC);
  adv->setScanResponse(true);
  adv->start();

  s_inited = true;
  LOGI("hub: advertising as '%s' (passkey %06u)", name, (unsigned)s_passkey);
  return true;
}

bool HubLinkBle::isConnected() { return s_connected; }
void HubLinkBle::onFrame(hub_frame_cb cb) { s_frame_cb = cb; }

bool HubLinkBle::send(const char* json, size_t len) {
  if (!s_inited || !s_connected || !s_out) return false;
  // Copy the whole frame into the outbound buffer (frozen contract: enqueue + copy). All-or-nothing.
  if (xStreamBufferSpacesAvailable(s_out) < len) return false;
  return xStreamBufferSend(s_out, json, len, 0) == len;
}

void HubLinkBle::loop() {
  if (!s_inited) return;

  // Inbound: drain bytes the BT task queued, reassemble into whole frames -> onFrame.
  uint8_t tmp[128];
  size_t n;
  while ((n = xStreamBufferReceive(s_in, tmp, sizeof(tmp), 0)) > 0)
    hub_reassembler_feed(&s_reasm, (const char*)tmp, n, reasm_emit, nullptr);

  // Outbound: notify queued command/frame bytes while connected, chunked to the negotiated ATT payload
  // (MTU - 3) so a multi-chunk frame needs far fewer notify() calls (see HUB_TX_CHUNK_* above). Falls
  // back to the ATT-default payload until the central upgrades the MTU.
  if (s_connected && s_tx) {
    uint16_t mtu = s_server ? s_server->getPeerMTU(s_server->getConnId()) : 0;
    size_t cap = (mtu > 23) ? (size_t)(mtu - 3) : HUB_TX_CHUNK_MIN;
    if (cap > HUB_TX_CHUNK_MAX) cap = HUB_TX_CHUNK_MAX;
    uint8_t chunk[HUB_TX_CHUNK_MAX];
    while ((n = xStreamBufferReceive(s_out, chunk, cap, 0)) > 0) {
      s_tx->setValue(chunk, n);
      s_tx->notify();
    }
  }
}
