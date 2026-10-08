/*
 * Interface entre a bancada virtual (sim_network.cpp) e cada instância do
 * firmware do Node CAN compilada para o computador.
 *
 * Cada instância é o firmware real (src/main.ino e demais arquivos) dentro de
 * um namespace próprio, com variáveis globais próprias; ver README.md.
 */
#ifndef SIM_API_H
#define SIM_API_H

#include <cstdint>
#include <string>
#include <vector>

struct SimCanFrame {
  uint32_t id = 0;
  uint8_t len = 0;
  uint8_t data[8] = {0};
};

struct SimDatagram {
  std::vector<uint8_t> data;
  uint8_t ip[4] = {0, 0, 0, 0};
  uint16_t port = 0;
};

/* Lançada por ESP.restart(): o firmware real não retorna dessa chamada. */
struct SimRestart {};

class SimInstance {
 public:
  virtual ~SimInstance() {}
  virtual uint8_t nodeId() const = 0;
  virtual void boot() = 0;   /* setup() */
  virtual void step() = 0;   /* uma passagem de loop(); pode lançar SimRestart */

  /* Barramento CAN. */
  virtual bool canPeek(SimCanFrame& out) const = 0;  /* próximo quadro a transmitir */
  virtual void canPop() = 0;
  virtual void canDeliver(const SimCanFrame& frame) = 0;
  virtual bool canListenOnly() const = 0;
  virtual void canSetErrorCounters(uint8_t rec, uint8_t tec) = 0;

  /* Porta serial. */
  virtual std::string serialTake() = 0;
  virtual void serialWrite(const std::string& text) = 0;

  /* Rádio BLE: anúncio recebido pelo scanner e oferta anunciada pelo Node. */
  virtual void bleDeliver(const std::string& manufacturer, int rssi) = 0;
  virtual bool bleOffer(std::string& manufacturer) const = 0;

  /* Ponto de acesso Wi-Fi e datagramas UDP. */
  virtual bool apInfo(std::string& ssid, std::string& psk, int& channel) const = 0;
  virtual void wifiStations(int count, int rssi) = 0;
  virtual void udpDeliver(const SimDatagram& datagram) = 0;
  virtual bool udpTake(SimDatagram& datagram) = 0;

  /* Estado interno, para as verificações. */
  virtual int nodeState() const = 0;   /* enum NodeState */
  virtual uint8_t leaderId() const = 0;
};

/* Fornecidas pela bancada. */
uint64_t sim_now_us();
void sim_delay_ms(uint32_t ms);
uint32_t sim_random32();

#endif
