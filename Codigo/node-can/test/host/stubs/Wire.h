#ifndef HOST_WIRE_H
#define HOST_WIRE_H

// I2C do Arduino-ESP32, o bastante para compilar a leitura do INA219 (energia.cpp). Sem dispositivo no
// barramento: endTransmission() devolve 2 (NACK no endereço) e nada é lido.
class TwoWire {
 public:
  bool begin(int, int) { return true; }
  void setClock(uint32_t) {}
  void beginTransmission(uint8_t) {}
  size_t write(uint8_t) { return 1; }
  uint8_t endTransmission(bool = true) { return 2; }
  uint8_t requestFrom(uint8_t, uint8_t) { return 0; }
  int available() { return 0; }
  int read() { return -1; }
};
inline TwoWire Wire;

#endif
