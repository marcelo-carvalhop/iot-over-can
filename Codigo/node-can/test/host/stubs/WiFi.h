#ifndef HOST_WIFI_H
#define HOST_WIFI_H

#include "Arduino.h"

enum { WIFI_OFF = 0, WIFI_AP = 2 };

class HostWiFi {
 public:
  bool apUp = false;
  std::string ssid;
  std::string psk;
  int channel = 0;
  bool persistentCredentials = true;
  void persistent(bool enabled) { persistentCredentials = enabled; }
  void mode(int) {}
  bool softAP(const char* s, const char* p, int ch, int, int) {
    ssid = s; psk = p; channel = ch; apUp = true;
    return true;
  }
  bool softAPdisconnect(bool) { apUp = false; return true; }
  IPAddress softAPIP() const { return IPAddress(192, 168, 4, 1); }
};
extern HostWiFi WiFi;

#endif
