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
  uint16_t starts = 0;       // inícios do anúncio, para os cenários
  bool start() { advertising = true; ++starts; return true; }
  bool stop() { advertising = false; return true; }
};

class NimBLEAdvertisedDevice {
 public:
  std::string manufacturer;
  int rssi = -127;
  bool haveManufacturerData() const { return !manufacturer.empty(); }
  std::string getManufacturerData() const { return manufacturer; }
  int getRSSI() const { return rssi; }
};

class NimBLEScanCallbacks {
 public:
  virtual ~NimBLEScanCallbacks() {}
  virtual void onResult(const NimBLEAdvertisedDevice*) {}
};

class NimBLEScan {
 public:
  NimBLEScanCallbacks* callbacks = nullptr;
  bool scanning = false;
  void setScanCallbacks(NimBLEScanCallbacks* cb, bool) { callbacks = cb; }
  void setDuplicateFilter(int) {}
  void setActiveScan(bool) {}
  uint16_t interval = 0;
  uint16_t window = 0;     // janela pedida (unidades de 0,625 ms), para os cenários
  uint16_t starts = 0;     // reinícios da varredura
  void setInterval(uint16_t v) { interval = v; }
  void setWindow(uint16_t v) { window = v; }
  void setMaxResults(uint8_t) {}
  bool start(uint32_t, bool, bool) { scanning = true; ++starts; return true; }
  bool stop() { scanning = false; return true; }
  bool isScanning() const { return scanning; }
};

class NimBLEDevice {
 public:
  static void init(const std::string&) {}
  static NimBLEAdvertising* getAdvertising() {
    static NimBLEAdvertising instance;
    return &instance;
  }
  static NimBLEScan* getScan() {
    static NimBLEScan instance;
    return &instance;
  }
};

#endif
