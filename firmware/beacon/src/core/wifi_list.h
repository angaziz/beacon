#pragma once
#include <stdint.h>
#include <stdbool.h>

// Pure saved-network list (Arduino-free, host-testable). No locking, no NVS here: the persistence
// layer (nvs.cpp) owns a working copy + the cross-core mutex and calls these under it. Passwords are
// data, never logged/displayed by callers.
#define WIFI_SSID_CAP   33   // 32 + NUL
#define WIFI_PASS_CAP   65   // 64 + NUL (WPA2 max)
#define WIFI_MAX_SAVED   6

typedef struct { char ssid[WIFI_SSID_CAP]; char pass[WIFI_PASS_CAP]; } wifi_cred_t;
typedef struct { uint8_t count; wifi_cred_t e[WIFI_MAX_SAVED]; } wifi_list_t;

#ifdef __cplusplus
extern "C" {
#endif

int  wifi_list_find(const wifi_list_t* l, const char* ssid);                 // index, or -1
// Append, or update the password of an existing SSID (de-dup). Returns false ONLY when the list is
// full AND the ssid is new (reject; caller surfaces "forget one first"). Empty ssid => false.
bool wifi_list_add(wifi_list_t* l, const char* ssid, const char* pass);
bool wifi_list_remove(wifi_list_t* l, const char* ssid);                     // false if absent

#ifdef __cplusplus
}
#endif
