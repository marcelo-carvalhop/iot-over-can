#ifndef HOST_ACAN2515_H
#define HOST_ACAN2515_H

#include <cstdint>
#include <deque>
#include <vector>

class CANMessage {
 public:
  uint32_t id = 0;
  bool ext = false;
  bool rtr = false;
  uint8_t idx = 0;
  uint8_t len = 0;
  uint8_t data[8] = {0};
};

class ACAN2515Settings {
 public:
  enum RequestedMode { NormalMode = 0 << 5, SleepMode = 1 << 5, LoopBackMode = 2 << 5, ListenOnlyMode = 3 << 5 };
  ACAN2515Settings(uint32_t, uint32_t) {}
  RequestedMode mRequestedMode = NormalMode;
  uint16_t mReceiveBufferSize = 32;
  uint16_t mTransmitBuffer0Size = 16;
};

class ACAN2515 {
 public:
  ACAN2515() {}
  template <typename SpiT>
  ACAN2515(uint8_t, SpiT&, uint8_t) {}

  // Fila de transmissão do driver. `sent` guarda os quadros aceitos, na
  // ordem; quem simula o barramento os retira e ajusta `pendingInDriver`.
  std::vector<CANMessage> sent;
  unsigned capacity = 16;
  unsigned pendingInDriver = 0;

  // Fila de recepção.
  std::deque<CANMessage> received;
  uint16_t receiveCapacity = 32;
  uint16_t receivePeak = 0;
  uint32_t receiveOverflows = 0;

  bool listenOnly = false;
  // Laço interno (LoopBackMode): o controlador devolve a si mesmo o que transmite e fica
  // desligado do barramento. Falhas que a bancada pode injetar para exercitar o autoteste.
  bool loopback = false;
  enum LoopbackFault { LB_FAULT_NONE = 0, LB_FAULT_DROP = 1, LB_FAULT_FLIP_BIT = 2, LB_FAULT_REFUSE_MODE = 3 };
  int loopbackFault = LB_FAULT_NONE;
  uint32_t modeChanges = 0;
  uint8_t rec = 0;
  uint8_t tec = 0;
  uint8_t eflg = 0;

  template <typename Isr>
  uint16_t begin(const ACAN2515Settings& settings, Isr) {
    receiveCapacity = settings.mReceiveBufferSize;
    capacity = settings.mTransmitBuffer0Size;
    listenOnly = settings.mRequestedMode == ACAN2515Settings::ListenOnlyMode;
    loopback = settings.mRequestedMode == ACAN2515Settings::LoopBackMode;
    return 0;
  }
  void isr() {}

  uint16_t changeModeOnTheFly(ACAN2515Settings::RequestedMode mode) {
    ++modeChanges;
    // O controlador que não confirma o modo pedido devolve erro (kRequestedModeTimeOut na biblioteca real).
    if (loopbackFault == LB_FAULT_REFUSE_MODE && mode == ACAN2515Settings::LoopBackMode) return 0x100;
    listenOnly = mode == ACAN2515Settings::ListenOnlyMode;
    loopback = mode == ACAN2515Settings::LoopBackMode;
    return 0;
  }

  bool receive(CANMessage& out) {
    if (received.empty()) return false;
    out = received.front();
    received.pop_front();
    return true;
  }
  // Usado pelo simulador do barramento para entregar um quadro a este nó.
  void hostDeliver(const CANMessage& msg) {
    if (loopback) return;  // no laço interno o controlador não está ligado ao barramento
    if (received.size() >= receiveCapacity) {
      ++receiveOverflows;
      eflg |= 0x40;  // RX0OVR
      return;
    }
    received.push_back(msg);
    if (received.size() > receivePeak) receivePeak = static_cast<uint16_t>(received.size());
  }

  uint16_t receiveBufferPeakCount() const { return receivePeak; }
  uint16_t transmitBufferSize(uint8_t) const { return static_cast<uint16_t>(capacity); }
  uint16_t transmitBufferCount(uint8_t) const { return static_cast<uint16_t>(pendingInDriver); }
  uint16_t transmitBufferPeakCount(uint8_t) const { return static_cast<uint16_t>(capacity); }
  uint8_t receiveErrorCounter() { return rec; }
  uint8_t transmitErrorCounter() { return tec; }
  uint8_t errorFlagRegister() { return eflg; }

  bool tryToSend(const CANMessage& msg) {
    if (loopback) {
      // O quadro volta para este nó e nunca toca o barramento.
      if (loopbackFault == LB_FAULT_DROP) return true;  // pino INT sem sinal: nada chega à fila
      CANMessage back = msg;
      if (loopbackFault == LB_FAULT_FLIP_BIT) back.data[0] ^= 0x01;  // bit preso no SPI
      received.push_back(back);
      return true;
    }
    if (pendingInDriver >= capacity) return false;
    ++pendingInDriver;
    sent.push_back(msg);
    return true;
  }
};

#endif
