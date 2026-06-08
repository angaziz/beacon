#pragma once
#include "core/hublink.h"

// Bluedroid (Arduino BLE) implementation of the FROZEN HubLink (core/hublink.h). NUS GATT
// peripheral, bonded LE-Secure-Connections, newline-delimited JSON framing (tech.md §7.1). The
// device is the peripheral; the macOS hub is the central.
//
// Callback-context rule (P2 spec §3.2 / D8): the Bluedroid RX-write callback runs in the BT stack
// task and ONLY copies bytes into an inbound stream buffer; loop() (pumped from the Core-0 hub task)
// reassembles frames, invokes onFrame, and drains the outbound stream buffer (notify in safe
// chunks). send() is therefore safe to call from Core-1 (the buddy decide path).
class HubLinkBle : public HubLink {
public:
  bool begin() override;                 // init BLE + security + GATT + advertise
  bool isConnected() override;
  void onFrame(hub_frame_cb cb) override; // status-frame handler (called from loop())
  bool send(const char* json, size_t len) override; // enqueue a device->hub command (copies)
  void loop() override;                   // pump from a Core-0 task
};

// Pairing surface for the Settings pair overlay (P2 spec §3.4, D3). The passkey is a static 6-digit
// value generated once at begin() and displayed while a central is pairing.
uint32_t hublink_ble_passkey(void);
bool     hublink_ble_is_pairing(void);
