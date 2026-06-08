#include "core/wifi_list.h"
#include <string.h>

static void set_str(char* dst, size_t cap, const char* src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = 0;
}

int wifi_list_find(const wifi_list_t* l, const char* ssid) {
  if (!ssid || !ssid[0]) return -1;
  for (uint8_t i = 0; i < l->count; i++)
    if (strcmp(l->e[i].ssid, ssid) == 0) return i;
  return -1;
}

bool wifi_list_add(wifi_list_t* l, const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) return false;
  int idx = wifi_list_find(l, ssid);
  if (idx >= 0) { set_str(l->e[idx].pass, WIFI_PASS_CAP, pass); return true; }   // de-dup: update password
  if (l->count >= WIFI_MAX_SAVED) return false;                                  // full + new => reject
  set_str(l->e[l->count].ssid, WIFI_SSID_CAP, ssid);
  set_str(l->e[l->count].pass, WIFI_PASS_CAP, pass);
  l->count++;
  return true;
}

bool wifi_list_remove(wifi_list_t* l, const char* ssid) {
  int idx = wifi_list_find(l, ssid);
  if (idx < 0) return false;
  for (uint8_t i = idx; i + 1 < l->count; i++) l->e[i] = l->e[i + 1];           // shift down, preserve order
  l->count--;
  memset(&l->e[l->count], 0, sizeof(l->e[l->count]));
  return true;
}
