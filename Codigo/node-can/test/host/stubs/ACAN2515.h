#ifndef HOST_ACAN2515_H
#define HOST_ACAN2515_H

#include <cstdint>
#include <vector>

class CANMessage {
 public:
  uint32_t id = 0;
  bool ext = false;
  bool rtr = false;
  uint8_t len = 0;
  uint8_t data[8] = {0};
};

class ACAN2515 {
 public:
  std::vector<CANMessage> sent;
  unsigned capacity = 16;      // simula o buffer de transmissão do driver
  unsigned pendingInDriver = 0;
  uint16_t transmitBufferCount(uint8_t) const { return static_cast<uint16_t>(pendingInDriver); }
  bool tryToSend(const CANMessage& msg) {
    if (pendingInDriver >= capacity) return false;
    ++pendingInDriver;
    sent.push_back(msg);
    return true;
  }
};

#endif
