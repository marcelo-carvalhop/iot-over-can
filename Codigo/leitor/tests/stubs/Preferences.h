#pragma once

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace stub {
inline std::mutex mutexNvs;
inline std::map<std::string, std::vector<uint8_t>> nvs;
}  // namespace stub

/* NVS de mentira: sobrevive a um novo setup() dentro do mesmo teste. */
class Preferences {
 public:
  bool begin(const char*, bool) { return true; }
  uint32_t getUInt(const char* k, uint32_t padrao = 0) {
    uint32_t v = padrao;
    return getBytes(k, &v, sizeof(v)) == sizeof(v) ? v : padrao;
  }
  size_t putUInt(const char* k, uint32_t v) { return putBytes(k, &v, sizeof(v)); }
  bool remove(const char* k) {
    std::lock_guard<std::mutex> g(stub::mutexNvs);
    return stub::nvs.erase(k) > 0;
  }
  size_t getBytes(const char* k, void* buf, size_t n) {
    std::lock_guard<std::mutex> g(stub::mutexNvs);
    auto it = stub::nvs.find(k);
    if (it == stub::nvs.end() || it->second.size() > n) return 0;
    std::memcpy(buf, it->second.data(), it->second.size());
    return it->second.size();
  }
  size_t putBytes(const char* k, const void* v, size_t n) {
    std::lock_guard<std::mutex> g(stub::mutexNvs);
    const uint8_t* b = static_cast<const uint8_t*>(v);
    stub::nvs[k].assign(b, b + n);
    return n;
  }
};
