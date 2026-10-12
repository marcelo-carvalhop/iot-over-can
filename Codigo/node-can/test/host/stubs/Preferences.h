// Substituto da biblioteca Preferences do Arduino-ESP32 (NVS) para a bancada
// virtual. Os dados ficam na bancada (sim_nvs_get/sim_nvs_put, sim_api.h),
// por Node: sobrevivem ao reinício da instância, como a NVS do ESP32.
// Incluído dentro do namespace de cada instância (ver Arduino.h).
#ifndef HOST_PREFERENCES_H
#define HOST_PREFERENCES_H

class Preferences {
 public:
  bool begin(const char* name, bool readOnly = false) {
    ns_ = name ? name : "";
    readOnly_ = readOnly;
    open_ = true;
    return true;
  }
  void end() { open_ = false; }

  size_t putBytes(const char* key, const void* value, size_t len) {
    if (!open_ || readOnly_ || !key) return 0;
    ::sim_nvs_put(IOT_NODE_ID, ns_ + "/" + key, static_cast<const uint8_t*>(value), len);
    return len;
  }

  // Como no ESP32: 0 quando o valor não existe ou não cabe em maxLen.
  size_t getBytes(const char* key, void* buf, size_t maxLen) {
    if (!open_ || !key) return 0;
    return ::sim_nvs_get(IOT_NODE_ID, ns_ + "/" + key, static_cast<uint8_t*>(buf), maxLen);
  }

 private:
  std::string ns_;
  bool readOnly_ = false;
  bool open_ = false;
};

#endif
