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
  void setInterval(uint16_t) {}
  void setWindow(uint16_t) {}
  void setMaxResults(uint8_t) {}
  bool start(uint32_t, bool, bool) { scanning = true; return true; }
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
