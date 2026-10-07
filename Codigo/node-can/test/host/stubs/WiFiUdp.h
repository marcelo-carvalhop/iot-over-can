#ifndef HOST_WIFIUDP_H
#define HOST_WIFIUDP_H

#include <deque>
#include <vector>

#include "Arduino.h"

struct HostDatagram {
  std::vector<uint8_t> data;
  IPAddress ip;
  uint16_t port = 0;
};

class WiFiUDP {
 public:
  static std::deque<HostDatagram> inbox;   // sensor -> Node
  static std::deque<HostDatagram> outbox;  // Node -> sensor
  uint8_t begin(uint16_t) { return 1; }
  void stop() {}
  void flush() {}
  int parsePacket() {
    if (inbox.empty()) return 0;
    current_ = inbox.front();
    inbox.pop_front();
    return (int)current_.data.size();
  }
  int read(uint8_t* buf, size_t cap) {
    size_t n = current_.data.size() < cap ? current_.data.size() : cap;
    memcpy(buf, current_.data.data(), n);
    return (int)n;
  }
  IPAddress remoteIP() { return current_.ip; }
  uint16_t remotePort() { return current_.port; }
  int beginPacket(const IPAddress& ip, uint16_t port) {
    tx_ = HostDatagram{};
    tx_.ip = ip;
    tx_.port = port;
    return 1;
  }
  size_t write(const uint8_t* buf, size_t len) {
    tx_.data.insert(tx_.data.end(), buf, buf + len);
    return len;
  }
  int endPacket() {
    outbox.push_back(tx_);
    return 1;
  }

 private:
  HostDatagram current_;
  HostDatagram tx_;
};

#endif
