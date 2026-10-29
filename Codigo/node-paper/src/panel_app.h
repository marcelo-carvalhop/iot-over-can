#pragma once
/*
 * Lógica do painel em papel eletrônico, sem dependência de hardware.
 *
 * Roda no PaperColor (main.cpp fornece o PanelHal com M5Unified, Wi-Fi, BLE,
 * USB e CardKB) e na bancada virtual (sim_network.cpp fornece um PanelHal
 * simulado). Tudo o que decide o comportamento — enlace autenticado com o
 * Node, retrato, relevância, página, política de atualização, marcas,
 * roteiros, teclado, sono e desligamento — está aqui e é exercitado pelos
 * cenários da bancada.
 */
#include <stddef.h>
#include <stdint.h>

#include "ioc_link.h"
#include "ioc_page.h"
#include "ioc_panel_proto.h"
#include "ioc_view.h"
#include "panel_signal.h"

#define PANEL_FW_VERSION "0.21.0"

/* Teclas do CardKB v1.1 (I2C 0x5F). */
enum : uint8_t {
  KEY_BACKSPACE = 0x08, KEY_TAB = 0x09, KEY_ENTER = 0x0D, KEY_ESC = 0x1B,
  KEY_LEFT = 0xB4, KEY_UP = 0xB5, KEY_DOWN = 0xB6, KEY_RIGHT = 0xB7
};

enum PanelPage : uint8_t { PAGE_SITUATION = 0, PAGE_NETWORK, PAGE_ROUTINE, PAGE_RADIO, PAGE_HELP, PAGE_COUNT };

class PanelHal {
 public:
  virtual ~PanelHal() {}
  virtual uint32_t millis() = 0;
  /* Segundos UTC do relógio de tempo real; continua contando durante o sono. */
  virtual int64_t epochNow() = 0;
  virtual bool clockValid() = 0;
  virtual void setClock(int64_t epoch) = 0;
  /* Wi-Fi: associa ao ponto de acesso IOC-NN; USB: avisa a ponte. */
  virtual void joinNode(uint8_t node) = 0;
  virtual void leaveNode() = 0;
  virtual bool linkUp() = 0;
  virtual bool sendDatagram(const uint8_t* data, size_t len) = 0;
  /* Inicia o desenho da página (no papel, 15 a 30 s, em outra tarefa). */
  virtual void render(const ioc_dl& dl) = 0;
  virtual bool renderBusy() = 0;
  /* Os dois LEDs RGB do topo (0xRRGGBB); ver panel_signal.h. */
  virtual void leds(uint32_t rgb0, uint32_t rgb1) = 0;
  /* Uma nota, sem bloquear; o PanelApp encadeia as melodias. */
  virtual void tone(uint16_t hz, uint16_t ms) = 0;
  virtual void readSensors(uint8_t& batteryPct, uint16_t& batteryMv, int16_t& tempC10, uint16_t& rh10, bool& usb) = 0;
  virtual bool keyboardPresent() = 0;
  /* Desliga e religa após `seconds` (no aparelho, pelo RTC; não retorna). O
   * botão de energia acorda o painel antes da hora. */
  virtual void sleepFor(uint32_t seconds) = 0;
  /* Desliga tudo: rádios, LEDs, som, teclado e o próprio painel (não retorna). */
  virtual void powerOff() = 0;
  virtual void storeU32(const char* key, uint32_t value) = 0;
  virtual uint32_t loadU32(const char* key, uint32_t fallback) = 0;
  virtual void storeStr(const char* key, const char* value) = 0;
  virtual bool loadStr(const char* key, char* out, size_t cap) = 0;
  /* Varredura de rádio para a página Rádio; false quando não há rádio (USB). */
  virtual bool radioSurvey(char* out, size_t cap) = 0;
  virtual void log(const char* line) = 0;
};

struct PanelConfig {
  uint64_t uuid = 0;
  uint8_t devKey[IOC_LINK_KEY_LEN] = {0};
  bool canSleep = false;        /* bateria e Wi-Fi: dorme entre verificações */
  uint16_t sleepNormalS = 600;  /* rede normal */
  uint16_t sleepAttentionS = 120;
  int16_t tzMin = -180;
  bool userBoot = true;         /* ligado pelo operador (false: acordou pelo RTC) */
};

struct PanelMark {
  bool used = false;
  bool acked = false;
  uint16_t seq = 0;
  uint8_t kind = 0;
  uint8_t step = 0;
  uint32_t createdMs = 0;
  uint32_t lastSendMs = 0;
  char text[IOC_PANEL_MARK_TEXT_MAX + 1] = {0};
};

class PanelApp {
 public:
  void begin(PanelHal* hal, const PanelConfig& cfg);
  // Ponte USB reconectada ou transporte substituído: exige novo handshake.
  void restartLink();
  void loop();

  void onOffer(const uint8_t* mfg, size_t len);
  void onDatagram(const uint8_t* data, size_t len);
  void onKey(uint8_t code);
  void onButton(char button);
  void onPowerClick(bool doubleClick);
  /* Estado do botão de energia (lido do M5PM1): segurar 1,5 s desliga. */
  void onPowerPressed(bool pressed);
  void onPowerHold() { shutdown(); }
  /* Tempo de tela forçado (por exemplo, ao entrar na USB). */
  void requestRefresh() { forceRefresh_ = true; }

  /* Estado para a bancada e para o firmware. */
  bool sessionUp() const { return mode_ == Mode::Bound; }
  uint8_t parentNode() const { return parent_; }
  uint8_t childId() const { return child_; }
  uint8_t level() const { return level_; }
  uint8_t page() const { return page_; }
  uint16_t refreshes() const { return refresh_.count; }
  uint32_t pageHash() const { return refresh_.hash; }
  const ioc_dl& lastDrawn() const { return drawn_; }
  const ioc_dl& composed() const { return dl_; }
  const ioc_view& view() const { return view_; }
  uint16_t marksPending() const;
  uint16_t marksAcked() const { return marksAcked_; }
  bool composing() const { return composing_; }
  bool snapshotReceived() const { return view_.snapshot_done != 0; }
  uint32_t sleepSeconds() const;
  bool unacked() const { return unacked_; }
  uint16_t alertsAnnounced() const { return alertsAnnounced_; }
  uint16_t acks() const { return acks_; }
  uint32_t ledColor(uint8_t i) const { return ledOut_[i & 1]; }
  bool poweringOff() const { return off_ != Off::None; }
  bool powerOffDone() const { return off_ == Off::Finished; }

 private:
  enum class Mode : uint8_t { Idle, Discovery, Bound };
  enum class Off : uint8_t { None, WaitRender, Drawing, Done, Finished };

  void startDiscovery(uint8_t node);
  void sendHello();
  void sendEnvelope(const uint8_t* key, uint8_t type, uint32_t counter, const uint8_t* payload, size_t len);
  void sendData(const uint8_t* inner, size_t len);
  void sendPanelHello();
  void sendStatus(uint8_t extraFlags, uint16_t sleepS);
  void handleData(const uint8_t* inner, size_t len);
  void maintainLink(uint32_t now);
  void flushMarks(uint32_t now);
  void addMark(uint8_t kind, uint8_t step, const char* text, uint32_t createdMs);
  void rememberMark(const PanelMark& m);
  void routineTick(uint32_t now);
  void routineMark();
  void submitComposer(uint32_t now);
  void updateAlert(uint32_t now);
  bool acknowledge(uint32_t now);
  void saveAlert();
  void updateSignals(uint32_t now);
  void shutdownTick(uint32_t now);
  void composeSplash(const ioc_page_ctx& ctx);
  bool slashCommand(const char* text);
  void compose(uint32_t now);
  void composeRoutine(const ioc_page_ctx& ctx);
  void composeRadio(const ioc_page_ctx& ctx);
  void composeHelp(const ioc_page_ctx& ctx);
  void composeDraft(const ioc_page_ctx& ctx);
  void composeNoData(const ioc_page_ctx& ctx, uint32_t lastMs);
  void composeShutdown(const ioc_page_ctx& ctx);
  void fillCtx(ioc_page_ctx& ctx, uint32_t now);
  uint32_t msFromEpoch(uint32_t epoch, uint32_t now) const;
  uint32_t epochFromMs(uint32_t ms, uint32_t now) const;
  void restoreRefresh(uint32_t now);
  void saveRefresh(uint32_t now);
  void maybeRender(uint32_t now);
  void maybeSleep(uint32_t now);
  void shutdown();
  void userActivity(uint32_t now) { lastActivityMs_ = now; }
  void draftHeader(char* out, size_t cap) const;

  PanelHal* hal_ = nullptr;
  PanelConfig cfg_;
  Mode mode_ = Mode::Idle;
  uint8_t parent_ = 0;
  uint8_t child_ = 0;
  uint8_t nonce_[IOC_LINK_NONCE_LEN] = {0};
  uint8_t sessionKey_[IOC_LINK_KEY_LEN] = {0};
  uint32_t tx_ = 0;
  uint32_t rxLast_ = 0;
  uint32_t lastValidMs_ = 0;
  uint32_t discoverySinceMs_ = 0;
  uint32_t lastOfferMs_ = 0;
  uint32_t nextHelloMs_ = 0;
  uint32_t lastStatusMs_ = 0;
  uint32_t sessionStartMs_ = 0;
  uint32_t lastDataMs_ = 0;
  uint32_t bootMs_ = 0;
  uint32_t rng_ = 0x2545F491u;

  ioc_view view_;
  ioc_dl dl_;
  ioc_dl drawn_;
  ioc_refresh refresh_;
  ioc_page_item items_[IOC_PAGE_MAX_ITEMS];
  int itemCount_ = 0;
  uint8_t level_ = 0;
  uint8_t page_ = PAGE_SITUATION;
  bool forceRefresh_ = true;
  bool pageChanged_ = false;
  uint32_t lastRenderMs_ = 0;
  uint32_t lastEvalMs_ = 0;
  uint32_t lastActivityMs_ = 0;
  uint32_t normalSinceEpoch_ = 0;
  bool noDataDrawn_ = false;

  PanelMark marks_[8];
  uint16_t markSeq_ = 0;
  uint16_t marksAcked_ = 0;
  char recent_[IOC_PAGE_MARKS][64] = {{0}};

  bool composing_ = false;
  char draft_[IOC_PANEL_MARK_TEXT_MAX + 1] = {0};
  uint8_t draftLen_ = 0;
  uint32_t lastKeyMs_ = 0;
  uint32_t draftDrawnMs_ = 0;
  uint32_t draftStartMs_ = 0;  /* a marca vale a partir da primeira tecla */
  char flash_[96] = {0};      /* confirmação mostrada na próxima página */

  /* Canal rápido: LEDs, bipes e alertas com ciência. */
  ToneQueue tones_;
  uint32_t ledOut_[2] = {0, 0};
  bool ledsSent_ = false;
  uint32_t keyFlashUntil_ = 0;
  uint32_t savedFlashUntil_ = 0;
  uint32_t clickFlashUntil_ = 0;
  LedScene scene_ = LedScene::None;
  uint32_t sceneStart_ = 0;
  uint32_t alertKeys_[IOC_PAGE_MAX_ITEMS] = {0};
  uint8_t alertN_ = 0;
  uint32_t annKeys_[IOC_PAGE_MAX_ITEMS] = {0};  /* alertas já anunciados */
  uint8_t annN_ = 0;
  uint8_t annLevel_ = 0;
  uint32_t annEpoch_ = 0;
  uint32_t annMs_ = 0;
  uint32_t ackKeys_[IOC_PAGE_MAX_ITEMS] = {0};  /* alertas com ciência do operador */
  uint8_t ackN_ = 0;
  bool unacked_ = false;
  uint16_t alertsAnnounced_ = 0;
  uint16_t acks_ = 0;

  /* Energia. */
  bool powerPressed_ = false;
  bool powerReleasedSeen_ = false;
  uint32_t powerPressMs_ = 0;
  Off off_ = Off::None;
  uint32_t offStepMs_ = 0;
  bool splash_ = false;

  uint8_t routine_ = 0;
  uint8_t step_ = 0;
  bool stepRunning_ = false;
  uint32_t stepStartMs_ = 0;
  uint8_t stepLeader_ = 0;
  char stepResult_[IOC_PANEL_MARK_TEXT_MAX + 1] = {0};

  char radio_[640] = {0};
  bool radioValid_ = false;
};
