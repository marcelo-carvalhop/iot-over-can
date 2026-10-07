#ifndef HOST_NIMBLEDEVICE_H
#define HOST_NIMBLEDEVICE_H

#include <string>

#define BLE_GAP_CONN_MODE_NON 0

class NimBLEAdvertisementData {
 public:
  std::string manufacturer;
  bool setManufacturerData(const std::string& data) { manufacturer = data; return true; }
};

class NimBLEAdvertising {
 public:
  bool advertising = false;
  std::string manufacturer;  // última oferta anunciada
  void setConnectableMode(int) {}
  void setMinInterval(uint16_t) {}
  void setMaxInterval(uint16_t) {}
  bool setAdvertisementData(const NimBLEAdvertisementData& data) { manufacturer = data.manufacturer; return true; }
  bool start() { advertising = true; return true; }
  bool stop() { advertising = false; return true; }
};

class NimBLEDevice {
 public:
  static NimBLEAdvertising* getAdvertising() {
    static NimBLEAdvertising instance;
    return &instance;
  }
};

#endif
