#include "panel_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr uint32_t SESSION_TIMEOUT_MS = 10000;
constexpr uint32_t OFFER_SWITCH_HOLD_MS = 15000;
constexpr uint32_t DISCOVERY_GIVEUP_MS = 60000;
constexpr uint32_t STATUS_PERIOD_MS = 60000;
constexpr uint32_t MARK_RETRY_MS = 3000;
constexpr uint32_t DRAFT_IDLE_MS = 1500;
constexpr uint32_t DRAFT_REDRAW_MS = 30000;
constexpr uint32_t DRAFT_ABANDON_MS = 90000;  /* nota esquecida: grava o que foi digitado */
constexpr uint32_t AWAKE_IDLE_MS = 20000;
constexpr uint32_t KEY_AWAKE_MS = 60000;      /* depois de digitar, fica acordado mais tempo */
constexpr uint32_t POWER_HOLD_MS = 1500;      /* segurar o botão de energia desliga */
constexpr uint32_t POWER_HOLD_SHOW_MS = 200;  /* antes disso é um toque */
constexpr uint32_t ALERT_AWAKE_MS = 180000;   /* na bateria, fica acordado chamando o operador */
constexpr uint32_t REPEAT_YOU_S = 60;
constexpr uint32_t REPEAT_ATTENTION_S = 300;
constexpr uint32_t NO_DATA_MS = 30000;
constexpr uint32_t BOOT_NO_DATA_MS = 60000;
constexpr uint32_t VALID_FOR_S = 3600;
constexpr uint8_t CMD_PING = 0x14;
constexpr uint8_t CMD_PONG = 0x15;
constexpr uint8_t CMD_RELEASE = 0x1B;
constexpr uint16_t MAGIC = IOC_PANEL_MAGIC;

enum Cond : uint8_t { C_SENSOR_BOUND, C_SENSOR_SECURE, C_SENSOR_NOT_SECURE, C_SENSOR_UNBOUND, C_LEADER_CHANGE, C_NODES_PRESENT };

struct Step { const char* text; Cond cond; uint32_t timeoutMs; };
struct Routine { const char* name; const Step* steps; uint8_t count; };

const Step DATA_PLANE[] = {
    {"Associe o sensor pela TUI (F7) e aperte Enter.", C_SENSOR_BOUND, 60000},
    {"Aguarde a sessão autenticada; aperte Enter para medir.", C_SENSOR_SECURE, 60000},
    {"Desligue o Pico e aperte Enter no mesmo instante.", C_SENSOR_NOT_SECURE, 30000},
    {"Religue o Pico e aperte Enter.", C_SENSOR_SECURE, 90000},
    {"Desassocie pela TUI e aperte Enter.", C_SENSOR_UNBOUND, 30000},
};
const Step LEADER_FAILURE[] = {
    {"Desligue o Node líder e aperte Enter no mesmo instante.", C_LEADER_CHANGE, 30000},
    {"Religue o Node e aperte Enter.", C_NODES_PRESENT, 60000},
};
const Routine ROUTINES[] = {
    {"Plano de dados (seção 10)", DATA_PLANE, sizeof(DATA_PLANE) / sizeof(DATA_PLANE[0])},
    {"Falha do líder", LEADER_FAILURE, sizeof(LEADER_FAILURE) / sizeof(LEADER_FAILURE[0])},
};
constexpr uint8_t ROUTINE_COUNT = sizeof(ROUTINES) / sizeof(ROUTINES[0]);

const char* condName(Cond c) {
  switch (c) {
    case C_SENSOR_BOUND: return "vínculo";
    case C_SENSOR_SECURE: return "sessão autenticada";
    case C_SENSOR_NOT_SECURE: return "fim da sessão";
    case C_SENSOR_UNBOUND: return "desassociação";
    case C_LEADER_CHANGE: return "novo líder";
    default: return "todos os Nodes";
  }
}

void putU16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }

/* Dias desde 1970-01-01 (algoritmo de Howard Hinnant). */
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool isSensor(const ioc_view_sensor& s) { return s.used && s.profile != IOC_PANEL_PROFILE; }

uint32_t itemKey(const ioc_page_item& it) {
  return (static_cast<uint32_t>(it.code) << 16) | (static_cast<uint32_t>(it.node) << 8) | it.child;
}

bool hasKey(const uint32_t* set, uint8_t n, uint32_t key) {
  for (uint8_t i = 0; i < n; ++i) if (set[i] == key) return true;
  return false;
}

/* Conjuntos de alertas na NVS: "0b020100,0d010101". */
void keysToStr(const uint32_t* set, uint8_t n, char* out, size_t cap) {
  size_t used = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < n && used + 10 < cap; ++i) {
    used += static_cast<size_t>(snprintf(out + used, cap - used, i ? ",%08lx" : "%08lx", static_cast<unsigned long>(set[i])));
  }
}

uint8_t strToKeys(const char* text, uint32_t* set, uint8_t cap) {
  uint8_t n = 0;
  while (text && *text && n < cap) {
    char* end = nullptr;
    const unsigned long v = strtoul(text, &end, 16);
    if (end == text) break;
    set[n++] = static_cast<uint32_t>(v);
    text = *end == ',' ? end + 1 : end;
  }
  return n;
}

}  // namespace

/* ------------------------------------------------------------------ início */

void PanelApp::begin(PanelHal* hal, const PanelConfig& cfg) {
  hal_ = hal;
  cfg_ = cfg;
  // begin() também é usado na transição Wi-Fi -> USB. Uma sessão
  // autenticada do transporte anterior não pode sobreviver à troca.
  mode_ = Mode::Idle;
  parent_ = 0;
  child_ = 0;
  tx_ = rxLast_ = 0;
  lastValidMs_ = 0;
  memset(sessionKey_, 0, sizeof(sessionKey_));
  const uint32_t now = hal_->millis();
  bootMs_ = now;
  lastActivityMs_ = now;
  rng_ ^= static_cast<uint32_t>(cfg_.uuid) ^ static_cast<uint32_t>(cfg_.uuid >> 32) ^ now ^
          static_cast<uint32_t>(hal_->epochNow());
  if (rng_ == 0) rng_ = 1;
  ioc_view_init(&view_, now);
  memset(&refresh_, 0, sizeof(refresh_));
  ioc_dl_clear(&dl_);
  ioc_dl_clear(&drawn_);
  markSeq_ = static_cast<uint16_t>(hal_->loadU32("mseq", 0));
  normalSinceEpoch_ = hal_->loadU32("normal", 0);
  page_ = static_cast<uint8_t>(hal_->loadU32("page", PAGE_SITUATION));
  if (page_ >= PAGE_COUNT || page_ == PAGE_RADIO) page_ = PAGE_SITUATION;
  cfg_.sleepNormalS = static_cast<uint16_t>(hal_->loadU32("sleep", cfg_.sleepNormalS));
  annLevel_ = static_cast<uint8_t>(hal_->loadU32("alevel", 0));
  annEpoch_ = hal_->loadU32("aep", 0);
  {
    char keys[IOC_PAGE_MAX_ITEMS * 9 + 1];
    annN_ = hal_->loadStr("anns", keys, sizeof(keys)) ? strToKeys(keys, annKeys_, IOC_PAGE_MAX_ITEMS) : 0;
    ackN_ = hal_->loadStr("acks", keys, sizeof(keys)) ? strToKeys(keys, ackKeys_, IOC_PAGE_MAX_ITEMS) : 0;
  }
  off_ = Off::None;
  powerPressed_ = false;
  powerReleasedSeen_ = false;
  // A tela mostra "Painel desligado": troca pela página de partida já.
  splash_ = hal_->loadU32("offpg", 0) != 0;
  if (cfg_.userBoot) {
    scene_ = LedScene::Boot;
    sceneStart_ = now;
    tones_.play(Melody::Boot);
  }
  for (int i = 0; i < IOC_PAGE_MARKS; ++i) {
    char key[4] = {'m', static_cast<char>('0' + i), 0, 0};
    if (!hal_->loadStr(key, recent_[i], sizeof(recent_[i]))) recent_[i][0] = '\0';
  }
  restoreRefresh(now);
  const uint8_t last = static_cast<uint8_t>(hal_->loadU32("node", 0));
  if (last > 0 && last < 32) startDiscovery(last);  // volta direto ao Node de antes
}

uint32_t PanelApp::msFromEpoch(uint32_t epoch, uint32_t now) const {
  const int64_t delta = hal_->epochNow() - static_cast<int64_t>(epoch);
  return now - static_cast<uint32_t>(delta * 1000);
}

uint32_t PanelApp::epochFromMs(uint32_t ms, uint32_t now) const {
  return static_cast<uint32_t>(hal_->epochNow() - static_cast<int32_t>(now - ms) / 1000);
}

void PanelApp::restoreRefresh(uint32_t now) {
  const uint32_t valid = hal_->loadU32("rvalid", 0);
  const int64_t nowEpoch = hal_->epochNow();
  if (valid == 0 || static_cast<int64_t>(valid) <= nowEpoch) return;
  refresh_.have = 1;
  refresh_.hash = hal_->loadU32("rhash", 0);
  refresh_.level = static_cast<uint8_t>(hal_->loadU32("rlevel", 0));
  refresh_.count = static_cast<uint16_t>(hal_->loadU32("rcount", 0));
  refresh_.last_ms = msFromEpoch(hal_->loadU32("rlast", 0), now);
  refresh_.valid_until_ms = msFromEpoch(valid, now);
  const uint32_t pending = hal_->loadU32("rpend", 0);
  if (pending) {
    refresh_.pending_since_ms = msFromEpoch(pending, now);
    refresh_.pending_hash = hal_->loadU32("rphash", 0);
  }
  forceRefresh_ = false;  // a página no papel continua válida
}

void PanelApp::saveRefresh(uint32_t now) {
  hal_->storeU32("rhash", refresh_.hash);
  hal_->storeU32("rlevel", refresh_.level);
  hal_->storeU32("rcount", refresh_.count);
  hal_->storeU32("rlast", epochFromMs(refresh_.last_ms, now));
  hal_->storeU32("rvalid", epochFromMs(refresh_.valid_until_ms, now));
  hal_->storeU32("rpend", 0);
}

/* ------------------------------------------------------------------ enlace */

void PanelApp::startDiscovery(uint8_t node) {
  const uint32_t now = hal_->millis();
  // Reassociar inclusive ao MESMO Node: após perda de Wi-Fi/USB a camada de
  // transporte precisa reabrir o socket e publicar JOIN para a ponte.
  hal_->joinNode(node);
  parent_ = node;
  child_ = 0;
  mode_ = Mode::Discovery;
  discoverySinceMs_ = now;
  nextHelloMs_ = now;
  tx_ = rxLast_ = 0;
  lastValidMs_ = 0;
  memset(sessionKey_, 0, sizeof(sessionKey_));
  // Um nonce por tentativa de handshake, NÃO um nonce por retransmissão.
  // O Node conserva o desafio para esse HELLO até ele expirar.
  for (int i = 0; i < IOC_LINK_NONCE_LEN; ++i) {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    nonce_[i] = static_cast<uint8_t>(rng_ >> 24);
  }
  hal_->storeU32("node", node);
}

void PanelApp::restartLink() {
  // A USB foi reaberta pela ponte, ou o transporte foi reiniciado.
  // Preservar a preferência do Node, mas nunca reutilizar chave, contadores
  // ou estado Bound da conexão anterior.
  const uint32_t saved = hal_->loadU32("node", 0);
  const uint8_t node = parent_ > 0 && parent_ < 32
                           ? parent_
                           : (saved > 0 && saved < 32 ? static_cast<uint8_t>(saved) : 0);
  mode_ = Mode::Idle;
  parent_ = 0;
  child_ = 0;
  tx_ = rxLast_ = 0;
  lastValidMs_ = 0;
  memset(sessionKey_, 0, sizeof(sessionKey_));
  if (node) startDiscovery(node);
  else hal_->leaveNode();
}

void PanelApp::onOffer(const uint8_t* mfg, size_t len) {
  ioc_offer_t offer;
  if (!ioc_offer_verify(cfg_.devKey, mfg, len, &offer) || offer.uuid != cfg_.uuid) return;
  const uint32_t now = hal_->millis();
  lastOfferMs_ = now;
  if (mode_ == Mode::Bound) return;  // com sessão, ofertas são ignoradas
  const bool idle = parent_ == 0 || mode_ == Mode::Idle;
  const bool maySwitch = offer.node_id != parent_ && now - discoverySinceMs_ > OFFER_SWITCH_HOLD_MS;
  if (idle || maySwitch) startDiscovery(offer.node_id);
}

void PanelApp::sendEnvelope(const uint8_t* key, uint8_t type, uint32_t counter, const uint8_t* payload, size_t len) {
  static uint8_t buf[IOC_PANEL_DATAGRAM_MAX + IOC_ENV_OVERHEAD];
  const size_t n = ioc_env_seal(key, type, counter, payload, len, buf, sizeof(buf));
  if (n) hal_->sendDatagram(buf, n);
}

void PanelApp::sendData(const uint8_t* inner, size_t len) {
  if (mode_ != Mode::Bound || tx_ == 0xFFFFFFFFu) return;
  sendEnvelope(sessionKey_, IOC_MSG_DATA_UP, ++tx_, inner, len);
}

void PanelApp::sendHello() {
  // Retransmissões do mesmo handshake repetem o nonce original.
  ioc_hello_t hello;
  hello.uuid = cfg_.uuid;
  hello.profile = IOC_PANEL_PROFILE;
  hello.protocol = IOC_LINK_VERSION;
  memcpy(hello.sensor_nonce, nonce_, IOC_LINK_NONCE_LEN);
  uint8_t raw[IOC_HELLO_LEN];
  ioc_hello_encode(&hello, raw);
  sendEnvelope(cfg_.devKey, IOC_MSG_HELLO, 0, raw, sizeof(raw));
}

void PanelApp::sendPanelHello() {
  ioc_panel_hello hello;
  hello.version = IOC_PANEL_VERSION;
  hello.flags = IOC_PANEL_HELLO_SNAPSHOT;
  uint8_t bat = 255; uint16_t mv = 0; int16_t t = IOC_PANEL_NA_I16; uint16_t rh = IOC_PANEL_NA_U16; bool usb = false;
  hal_->readSensors(bat, mv, t, rh, usb);
  hello.sleep_s = (cfg_.canSleep && !usb) ? static_cast<uint16_t>(sleepSeconds()) : 0;
  uint8_t raw[IOC_PANEL_HELLO_LEN];
  ioc_panel_hello_encode(&hello, raw);
  sendData(raw, sizeof(raw));
}

void PanelApp::sendStatus(uint8_t extraFlags, uint16_t sleepS) {
  ioc_panel_status st;
  bool usb = false;
  hal_->readSensors(st.battery_pct, st.battery_mv, st.temp_c10, st.rh_10, usb);
  st.flags = static_cast<uint8_t>(extraFlags | (usb ? IOC_PANEL_ST_USB : 0) | (hal_->clockValid() ? IOC_PANEL_ST_CLOCK : 0) |
                                  (hal_->keyboardPresent() ? IOC_PANEL_ST_KEYBOARD : 0) |
                                  (unacked_ ? IOC_PANEL_ST_ALERT : 0));
  st.sleep_s = sleepS;
  st.page_hash = refresh_.hash;
  st.page_seq = static_cast<uint16_t>(refresh_.count);
  st.refreshes = refresh_.count;
  st.uptime_s = (hal_->millis() - bootMs_) / 1000u;
  st.severity = level_;
  uint8_t body[IOC_PANEL_STATUS_BODY], msg[3 + IOC_PANEL_STATUS_BODY];
  ioc_panel_status_encode_body(&st, body);
  const size_t n = ioc_panel_msg(IOC_PANEL_CMD_STATUS, body, sizeof(body), msg, sizeof(msg));
  sendData(msg, n);
  lastStatusMs_ = hal_->millis();
}

void PanelApp::onDatagram(const uint8_t* data, size_t len) {
  ioc_env_view env;
  if (!ioc_env_parse(data, len, &env)) return;
  const uint32_t now = hal_->millis();
  if (env.type == IOC_MSG_CHALLENGE) {
    if (mode_ != Mode::Discovery || !ioc_env_verify(cfg_.devKey, data, len)) return;
    ioc_challenge_t ch;
    if (!ioc_challenge_decode(env.payload, env.payload_len, &ch)) return;
    if (memcmp(ch.sensor_nonce, nonce_, IOC_LINK_NONCE_LEN) != 0) return;
    ioc_link_derive_session_key(cfg_.devKey, cfg_.uuid, ch.node_id, nonce_, ch.node_nonce, sessionKey_);
    parent_ = ch.node_id;
    child_ = ch.child_id;
    mode_ = Mode::Bound;
    tx_ = 0;
    rxLast_ = 0;
    lastValidMs_ = now;
    sessionStartMs_ = now;
    ioc_confirm_t cf;
    memcpy(cf.node_nonce, ch.node_nonce, IOC_LINK_NONCE_LEN);
    memcpy(cf.sensor_nonce, nonce_, IOC_LINK_NONCE_LEN);
    uint8_t raw[IOC_CONFIRM_LEN];
    ioc_confirm_encode(&cf, raw);
    sendEnvelope(sessionKey_, IOC_MSG_CONFIRM, ++tx_, raw, sizeof(raw));
    ioc_view_set_self(&view_, parent_, child_);
    sendPanelHello();
    sendStatus(0, 0);
    hal_->storeU32("node", parent_);
    char line[64];
    snprintf(line, sizeof(line), "sessao com node=%u filho=%u", parent_, child_);
    hal_->log(line);
    return;
  }
  if (env.type != IOC_MSG_DATA_DOWN || mode_ != Mode::Bound) return;
  if (!ioc_env_verify(sessionKey_, data, len) || !ioc_counter_accept(&rxLast_, env.counter)) return;
  lastValidMs_ = now;
  handleData(env.payload, env.payload_len);
}

void PanelApp::handleData(const uint8_t* inner, size_t len) {
  if (len < 3 || inner[0] != (MAGIC & 0xFF) || inner[1] != (MAGIC >> 8)) return;
  const uint32_t now = hal_->millis();
  switch (inner[2]) {
    case CMD_PING: {
      uint8_t pong[15] = {0};
      putU16(pong, MAGIC);
      pong[2] = CMD_PONG;
      if (len >= 15) memcpy(&pong[11], &inner[11], 4);
      sendData(pong, sizeof(pong));
      break;
    }
    case CMD_RELEASE:
      hal_->log("liberado pelo node");
      mode_ = Mode::Idle;
      parent_ = 0;
      child_ = 0;
      hal_->storeU32("node", 0);
      hal_->leaveNode();
      break;
    case IOC_PANEL_CMD_FRAMES:
      if (ioc_view_batch(&view_, inner, len, now) >= 0) {
        lastDataMs_ = now;
        noDataDrawn_ = false;
      }
      break;
    case IOC_PANEL_CMD_MARK_ACK:
      if (len >= 5) {
        const uint16_t seq = static_cast<uint16_t>(inner[3] | (inner[4] << 8));
        for (PanelMark& m : marks_) {
          if (m.used && !m.acked && m.seq == seq) {
            m.acked = true;
            m.used = false;
            ++marksAcked_;
            tones_.play(Melody::Saved);  // gravada: o Node a levou para a Probe e a TUI
            savedFlashUntil_ = now + 600;
          }
        }
      }
      break;
    default:
      break;
  }
}

void PanelApp::maintainLink(uint32_t now) {
  if (mode_ == Mode::Discovery) {
    if (hal_->linkUp() && static_cast<int32_t>(now - nextHelloMs_) >= 0) {
      rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
      nextHelloMs_ = now + 1500 + rng_ % 2001u;
      sendHello();
    }
    if (now - discoverySinceMs_ > DISCOVERY_GIVEUP_MS && now - lastOfferMs_ > DISCOVERY_GIVEUP_MS / 3) {
      hal_->log("sem sessao: aguardando oferta");
      mode_ = Mode::Idle;
      parent_ = 0;
      hal_->leaveNode();
    }
  } else if (mode_ == Mode::Bound) {
    if (now - lastValidMs_ > SESSION_TIMEOUT_MS) {
      hal_->log("sessao expirada");
      startDiscovery(parent_);
    } else if (now - lastStatusMs_ >= STATUS_PERIOD_MS) {
      sendStatus(0, 0);
    }
  }
}

/* ------------------------------------------------------------------- marcas */

uint16_t PanelApp::marksPending() const {
  uint16_t n = 0;
  for (const PanelMark& m : marks_) n += (m.used && !m.acked) ? 1 : 0;
  return n;
}

void PanelApp::rememberMark(const PanelMark& m) {
  char t[8];
  ioc_page_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.now_ms = hal_->millis();
  ctx.epoch_now = hal_->clockValid() ? hal_->epochNow() : 0;
  ctx.tz_min = cfg_.tzMin;
  ctx.boot_ms = bootMs_;
  ioc_page_time(&ctx, m.createdMs, t);
  for (int i = IOC_PAGE_MARKS - 1; i > 0; --i) memcpy(recent_[i], recent_[i - 1], sizeof(recent_[i]));
  char line[sizeof(recent_[0]) + IOC_PANEL_MARK_TEXT_MAX + 8];
  const char* fallback = m.kind == IOC_PANEL_MARK_FLAG ? "marca do operador" : "intervenção física";
  snprintf(line, sizeof(line), "%s %s", t, m.text[0] ? m.text : fallback);
  memcpy(recent_[0], line, sizeof(recent_[0]) - 1);
  recent_[0][sizeof(recent_[0]) - 1] = '\0';
  for (int i = 0; i < IOC_PAGE_MARKS; ++i) {
    char key[4] = {'m', static_cast<char>('0' + i), 0, 0};
    hal_->storeStr(key, recent_[i]);
  }
}

void PanelApp::addMark(uint8_t kind, uint8_t step, const char* text, uint32_t createdMs) {
  PanelMark* slot = nullptr;
  for (PanelMark& m : marks_) if (!m.used) { slot = &m; break; }
  if (!slot) {  // todas pendentes: descarta a mais antiga
    slot = &marks_[0];
    for (PanelMark& m : marks_) if (static_cast<int32_t>(m.createdMs - slot->createdMs) < 0) slot = &m;
    hal_->log("marca antiga descartada (sem sessao)");
  }
  *slot = PanelMark();
  slot->used = true;
  slot->seq = ++markSeq_;
  slot->kind = kind;
  slot->step = step;
  const uint32_t now = hal_->millis();
  slot->createdMs = createdMs;
  slot->lastSendMs = now - MARK_RETRY_MS;
  snprintf(slot->text, sizeof(slot->text), "%s", text ? text : "");
  hal_->storeU32("mseq", markSeq_);
  if (kind != IOC_PANEL_MARK_RESULT || text) rememberMark(*slot);
  switch (kind) {
    case IOC_PANEL_MARK_ACK: tones_.play(Melody::Ack); break;
    case IOC_PANEL_MARK_NOTE:
    case IOC_PANEL_MARK_RESULT: tones_.play(Melody::Sent); break;
    default: tones_.play(Melody::Flag); break;
  }
  char line[48];
  snprintf(line, sizeof(line), "marca %u %s", slot->seq, ioc_panel_mark_kind_name(kind));
  hal_->log(line);
  flushMarks(now);
}

void PanelApp::flushMarks(uint32_t now) {
  if (mode_ != Mode::Bound) return;
  for (PanelMark& m : marks_) {
    if (!m.used || m.acked || now - m.lastSendMs < MARK_RETRY_MS) continue;
    ioc_panel_mark wire;
    wire.seq = m.seq;
    wire.kind = m.kind;
    wire.step = m.step;
    wire.age_ms = now - m.createdMs;
    wire.text_len = static_cast<uint8_t>(strlen(m.text));
    memcpy(wire.text, m.text, wire.text_len + 1u);
    uint8_t body[IOC_PANEL_MARK_FIXED + IOC_PANEL_MARK_TEXT_MAX], msg[3 + sizeof(body)];
    const size_t blen = ioc_panel_mark_encode_body(&wire, body, sizeof(body));
    const size_t n = ioc_panel_msg(IOC_PANEL_CMD_MARK, body, blen, msg, sizeof(msg));
    sendData(msg, n);
    m.lastSendMs = now;
  }
}

/* ----------------------------------------------------------------- roteiros */

void PanelApp::routineMark() {
  const Routine& r = ROUTINES[routine_];
  if (step_ >= r.count) {  // concluído: recomeça
    step_ = 0;
    stepResult_[0] = '\0';
    pageChanged_ = true;
    return;
  }
  const uint32_t now = hal_->millis();
  stepRunning_ = true;
  stepStartMs_ = now;
  stepLeader_ = view_.leader;
  char text[IOC_PANEL_MARK_TEXT_MAX + 1];
  snprintf(text, sizeof(text), "%s, passo %u", r.name, static_cast<unsigned>(step_ + 1));
  addMark(IOC_PANEL_MARK_STEP, static_cast<uint8_t>(step_ + 1), text, now);
  snprintf(stepResult_, sizeof(stepResult_), "Medindo: aguardando %s.", condName(r.steps[step_].cond));
  pageChanged_ = true;
}

void PanelApp::routineTick(uint32_t now) {
  if (!stepRunning_) return;
  const Routine& r = ROUTINES[routine_];
  const Step& st = r.steps[step_];
  bool any = false, secure = false, absentNode = false;
  for (const ioc_view_sensor& s : view_.sensors) {
    if (!isSensor(s)) continue;
    any = true;
    if (s.link_state == IOC_WD_LINK_SECURE) secure = true;
  }
  for (uint8_t id = 1; id < IOC_VIEW_NODES; ++id) {
    if (view_.nodes[id].seen && view_.nodes[id].last_ms && view_.nodes[id].absent) absentNode = true;
  }
  bool done = false;
  switch (st.cond) {
    case C_SENSOR_BOUND: {
      for (const ioc_view_sensor& s : view_.sensors) {
        if (isSensor(s) && (s.assoc == 0x03 || s.assoc == 0x04)) done = true;
      }
      break;
    }
    case C_SENSOR_SECURE: done = secure; break;
    case C_SENSOR_NOT_SECURE: done = any && !secure; break;
    case C_SENSOR_UNBOUND: done = !any; break;
    case C_LEADER_CHANGE: done = view_.leader != 0 && view_.leader != stepLeader_; break;
    case C_NODES_PRESENT: done = !absentNode; break;
  }
  const uint32_t elapsed = now - stepStartMs_;
  char text[IOC_PANEL_MARK_TEXT_MAX + 1];
  if (done) {
    snprintf(text, sizeof(text), "Passo %u: %s em %lu,%lu s", static_cast<unsigned>(step_ + 1), condName(st.cond),
             static_cast<unsigned long>(elapsed / 1000u), static_cast<unsigned long>((elapsed % 1000u) / 100u));
  } else if (elapsed >= st.timeoutMs) {
    snprintf(text, sizeof(text), "Passo %u: %s não observado em %lu s", static_cast<unsigned>(step_ + 1),
             condName(st.cond), static_cast<unsigned long>(st.timeoutMs / 1000u));
  } else {
    return;
  }
  stepRunning_ = false;
  memcpy(stepResult_, text, sizeof(stepResult_) - 1);
  stepResult_[sizeof(stepResult_) - 1] = '\0';
  addMark(IOC_PANEL_MARK_RESULT, static_cast<uint8_t>(step_ + 1), text, now);
  ++step_;
  pageChanged_ = true;
}

/* ----------------------------------------------------------------- entradas */

bool PanelApp::slashCommand(const char* text) {
  int hh = 0, mm = 0, y = 0, mo = 0, d = 0, n = 0;
  const int64_t now = hal_->epochNow();
  const int64_t local = now + static_cast<int64_t>(cfg_.tzMin) * 60;
  int64_t midnight = local - ((local % 86400) + 86400) % 86400;
  if (sscanf(text, "/hora %d:%d", &hh, &mm) == 2 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
    hal_->setClock(midnight + hh * 3600 + mm * 60 - static_cast<int64_t>(cfg_.tzMin) * 60);
    snprintf(flash_, sizeof(flash_), "Relógio ajustado para %02d:%02d", hh, mm);
    return true;
  }
  if (sscanf(text, "/data %d-%d-%d", &y, &mo, &d) == 3 && y >= 2024 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
    const int64_t seconds = ((local % 86400) + 86400) % 86400;
    hal_->setClock(daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + seconds -
                   static_cast<int64_t>(cfg_.tzMin) * 60);
    snprintf(flash_, sizeof(flash_), "Data ajustada: %04d-%02d-%02d", y, mo, d);
    return true;
  }
  if (sscanf(text, "/sono %d", &n) == 1 && n >= 1 && n <= 120) {
    cfg_.sleepNormalS = static_cast<uint16_t>(n * 60);
    hal_->storeU32("sleep", cfg_.sleepNormalS);
    snprintf(flash_, sizeof(flash_), "Verificação a cada %d min com a rede normal", n);
    return true;
  }
  if (strncmp(text, "/ajuda", 6) == 0) {
    page_ = PAGE_HELP;
    return true;
  }
  if (text[0] == '/') {
    snprintf(flash_, sizeof(flash_), "Comando desconhecido: %.40s", text);
    return true;
  }
  return false;
}

void PanelApp::submitComposer(uint32_t now) {
  composing_ = false;
  draft_[draftLen_] = '\0';
  pageChanged_ = true;
  if (draftLen_ > 0 && slashCommand(draft_)) {
    tones_.play(flash_[0] && strncmp(flash_, "Comando desconhecido", 20) == 0 ? Melody::Error : Melody::Sent);
  } else {
    // A marca vale desde a primeira tecla: é o instante que o operador quis registrar.
    addMark(draftLen_ ? IOC_PANEL_MARK_NOTE : IOC_PANEL_MARK_FLAG, 0, draftLen_ ? draft_ : nullptr, draftStartMs_);
  }
  draftLen_ = 0;
  draft_[0] = '\0';
  (void)now;
}

void PanelApp::onKey(uint8_t code) {
  const uint32_t now = hal_->millis();
  if (off_ != Off::None) return;
  userActivity(now);
  lastKeyMs_ = now;
  keyFlashUntil_ = now + 80;  // a tecla foi lida: retorno imediato no LED
  const bool printable = code >= 0x20 && code < 0x7F;
  // Com um alerta sem ciência, a primeira tecla é a ciência; uma letra também começa a nota.
  if (acknowledge(now) && !printable) return;
  if (composing_) {
    if (code == KEY_ENTER) {
      submitComposer(now);
    } else if (code == KEY_ESC) {
      composing_ = false;
      draftLen_ = 0;
      draft_[0] = '\0';
      pageChanged_ = true;
      tones_.play(Melody::Error);
      hal_->log("marca cancelada");
    } else if (code == KEY_BACKSPACE) {
      while (draftLen_ > 0) {
        const unsigned char c = static_cast<unsigned char>(draft_[--draftLen_]);
        if ((c & 0xC0u) != 0x80u) break;
      }
      draft_[draftLen_] = '\0';
    } else if (printable && draftLen_ < IOC_PANEL_MARK_TEXT_MAX) {
      draft_[draftLen_++] = static_cast<char>(code);
      draft_[draftLen_] = '\0';
    }
    return;
  }
  switch (code) {
    case KEY_TAB:
    case KEY_RIGHT:
    case KEY_DOWN:
      if (page_ == PAGE_ROUTINE && code == KEY_RIGHT && !stepRunning_) {
        routine_ = static_cast<uint8_t>((routine_ + 1) % ROUTINE_COUNT);
        step_ = 0;
        stepResult_[0] = '\0';
      } else {
        page_ = static_cast<uint8_t>((page_ + 1) % PAGE_COUNT);
      }
      pageChanged_ = true;
      break;
    case KEY_LEFT:
    case KEY_UP:
      if (page_ == PAGE_ROUTINE && code == KEY_LEFT && !stepRunning_) {
        routine_ = static_cast<uint8_t>((routine_ + ROUTINE_COUNT - 1) % ROUTINE_COUNT);
        step_ = 0;
        stepResult_[0] = '\0';
      } else {
        page_ = static_cast<uint8_t>((page_ + PAGE_COUNT - 1) % PAGE_COUNT);
      }
      pageChanged_ = true;
      break;
    case KEY_ESC:
      if (page_ != PAGE_SITUATION) { page_ = PAGE_SITUATION; pageChanged_ = true; }
      break;
    case KEY_ENTER:
      if (page_ == PAGE_ROUTINE) routineMark();
      else addMark(IOC_PANEL_MARK_FLAG, 0, nullptr, now);  // só o instante
      pageChanged_ = true;
      break;
    default:
      if (printable) {  // a primeira tecla abre a marca neste instante
        composing_ = true;
        draftStartMs_ = now;
        draftDrawnMs_ = 0;
        draftLen_ = 0;
        draft_[draftLen_++] = static_cast<char>(code);
        draft_[draftLen_] = '\0';
        tones_.play(Melody::Flag);
        hal_->log("marca aberta pelo teclado");
      }
      break;
  }
  if (pageChanged_ && page_ == PAGE_RADIO) radioValid_ = hal_->radioSurvey(radio_, sizeof(radio_));
  if (pageChanged_) hal_->storeU32("page", page_);
}

void PanelApp::onButton(char button) {
  const uint32_t now = hal_->millis();
  if (off_ != Off::None) return;
  userActivity(now);
  clickFlashUntil_ = now + 120;
  if (acknowledge(now)) return;
  if (button == 'A') {
    if (page_ == PAGE_ROUTINE) routineMark();
    else addMark(IOC_PANEL_MARK_BUTTON, 0, nullptr, now);
    pageChanged_ = true;
  } else if (button == 'B') {
    onKey(KEY_TAB);
  } else if (button == 'C') {
    if (page_ == PAGE_RADIO) radioValid_ = hal_->radioSurvey(radio_, sizeof(radio_));
    forceRefresh_ = true;
    tones_.play(Melody::Click);
  }
}

void PanelApp::onPowerClick(bool doubleClick) {
  const uint32_t now = hal_->millis();
  if (off_ != Off::None) return;
  userActivity(now);
  if (doubleClick) { shutdown(); return; }
  clickFlashUntil_ = now + 150;
  if (acknowledge(now)) return;
  forceRefresh_ = true;  // um toque: atualiza agora
  tones_.play(Melody::Click);
}

void PanelApp::onPowerPressed(bool pressed) {
  const uint32_t now = hal_->millis();
  if (off_ != Off::None) return;
  // Quem ligou o painel segurando o botão ainda pode estar com o dedo nele:
  // só vale depois de visto solto uma vez.
  if (!powerReleasedSeen_) {
    powerReleasedSeen_ = !pressed;
    return;
  }
  if (pressed && !powerPressed_) {
    powerPressed_ = true;
    powerPressMs_ = now;
    userActivity(now);
  } else if (!pressed) {
    powerPressed_ = false;
  }
}

/* ------------------------------------------------------- alertas e sinais */

void PanelApp::saveAlert() {
  char keys[IOC_PAGE_MAX_ITEMS * 9 + 1];
  keysToStr(annKeys_, annN_, keys, sizeof(keys));
  hal_->storeStr("anns", keys);
  keysToStr(ackKeys_, ackN_, keys, sizeof(keys));
  hal_->storeStr("acks", keys);
  hal_->storeU32("alevel", annLevel_);
  hal_->storeU32("aep", annEpoch_);
}

/* Gestão por exceção no canal rápido: anuncia o que é novo, repete o que está
 * sem ciência e cala o que o operador já viu. A memória sobrevive ao sono. */
void PanelApp::updateAlert(uint32_t now) {
  if (!view_.snapshot_done) return;
  alertN_ = 0;
  for (int i = 0; i < itemCount_ && alertN_ < IOC_PAGE_MAX_ITEMS; ++i) {
    if (items_[i].level >= IOC_LEVEL_ATTENTION) alertKeys_[alertN_++] = itemKey(items_[i]);
  }
  if (level_ < IOC_LEVEL_ATTENTION) {
    if (annLevel_ >= IOC_LEVEL_ATTENTION) {
      tones_.play(Melody::Resolved);
      savedFlashUntil_ = now + 1500;
      hal_->log("alerta resolvido");
    } else if (level_ == IOC_LEVEL_HANDLING && annLevel_ < IOC_LEVEL_HANDLING) {
      tones_.play(Melody::Handling);
    }
    unacked_ = false;
    if (annLevel_ != level_ || annN_ || ackN_) {
      annLevel_ = level_;
      annN_ = 0;
      ackN_ = 0;
      saveAlert();
    }
    return;
  }
  bool fresh = level_ > annLevel_;
  unacked_ = false;
  for (uint8_t i = 0; i < alertN_; ++i) {
    const bool acked = hasKey(ackKeys_, ackN_, alertKeys_[i]);
    if (!acked) unacked_ = true;
    if (!acked && !hasKey(annKeys_, annN_, alertKeys_[i])) fresh = true;
  }
  const uint32_t epoch = static_cast<uint32_t>(hal_->epochNow());
  const uint32_t repeatS = level_ >= IOC_LEVEL_YOU ? REPEAT_YOU_S : REPEAT_ATTENTION_S;
  const bool repeat = unacked_ && epoch - annEpoch_ >= repeatS;
  if (fresh || repeat) {
    tones_.play(level_ >= IOC_LEVEL_YOU ? Melody::You : Melody::Attention);
    if (fresh) annMs_ = now;
    annEpoch_ = epoch;
    ++alertsAnnounced_;
    char line[64];
    snprintf(line, sizeof(line), "alerta nivel=%u %s", level_, fresh ? "novo" : "repetido (sem ciencia)");
    hal_->log(line);
    if (fresh && mode_ == Mode::Bound) sendStatus(0, 0);  // a TUI vê o alerta pendente
  }
  if (fresh || repeat || annLevel_ != level_) {
    memcpy(annKeys_, alertKeys_, sizeof(annKeys_));
    annN_ = alertN_;
    annLevel_ = level_;
    saveAlert();
  }
}

/* Ciência do operador: cala o alerta e registra no histórico quanto tempo levou. */
bool PanelApp::acknowledge(uint32_t now) {
  if (!unacked_) return false;
  unacked_ = false;
  memcpy(ackKeys_, alertKeys_, sizeof(ackKeys_));
  ackN_ = alertN_;
  saveAlert();
  ++acks_;
  const char* what = "alerta";
  for (int i = 0; i < itemCount_; ++i) {
    if (items_[i].level >= IOC_LEVEL_ATTENTION) { what = items_[i].title; break; }
  }
  char text[IOC_PANEL_MARK_TEXT_MAX + 1];
  if (annMs_ && now - annMs_ < 86400000u) {
    snprintf(text, sizeof(text), "Ciente em %lu s: %.72s", static_cast<unsigned long>((now - annMs_) / 1000u), what);
  } else {
    snprintf(text, sizeof(text), "Ciente: %.80s", what);
  }
  addMark(IOC_PANEL_MARK_ACK, level_, text, now);
  if (mode_ == Mode::Bound) sendStatus(0, 0);
  pageChanged_ = true;
  return true;
}

void PanelApp::updateSignals(uint32_t now) {
  Tone t;
  if (tones_.next(now, t)) hal_->tone(t.hz, t.ms);
  if (scene_ == LedScene::Boot && now - sceneStart_ > 1500) scene_ = LedScene::None;
  LedInputs in;
  in.now = now;
  in.level = level_;
  in.synced = view_.snapshot_done != 0;
  in.session = mode_ == Mode::Bound;
  uint8_t bat = 255; uint16_t mv = 0; int16_t tc = 0; uint16_t rh = 0; bool usb = false;
  hal_->readSensors(bat, mv, tc, rh, usb);
  in.usb = usb;
  in.unacked = unacked_;
  in.awakeSinceMs = bootMs_;
  in.composing = composing_;
  in.markPending = marksPending() > 0;
  in.rendering = hal_->renderBusy();
  in.keyFlashUntil = keyFlashUntil_;
  in.savedFlashUntil = savedFlashUntil_;
  in.clickFlashUntil = clickFlashUntil_;
  if (powerPressed_ && now - powerPressMs_ >= POWER_HOLD_SHOW_MS) {
    const uint32_t held = now - powerPressMs_ - POWER_HOLD_SHOW_MS;
    const uint32_t span = POWER_HOLD_MS - POWER_HOLD_SHOW_MS;
    in.holdPermille = static_cast<uint16_t>(held >= span ? 1000u : 1u + held * 999u / span);
  }
  in.scene = scene_;
  in.sceneStart = sceneStart_;
  uint32_t out[2];
  ledFrame(in, out);
  if (off_ == Off::Done || off_ == Off::Finished) out[0] = out[1] = 0;
  if (!ledsSent_ || out[0] != ledOut_[0] || out[1] != ledOut_[1]) {
    ledOut_[0] = out[0];
    ledOut_[1] = out[1];
    ledsSent_ = true;
    hal_->leds(out[0], out[1]);
  }
}

/* --------------------------------------------------------------- composição */

uint32_t PanelApp::sleepSeconds() const {
  if (level_ >= IOC_LEVEL_ATTENTION) return cfg_.sleepAttentionS;
  if (level_ == IOC_LEVEL_HANDLING) return cfg_.sleepNormalS < 300 ? cfg_.sleepNormalS : 300;
  return cfg_.sleepNormalS;
}

void PanelApp::fillCtx(ioc_page_ctx& ctx, uint32_t now) {
  memset(&ctx, 0, sizeof(ctx));
  ctx.now_ms = now;
  ctx.epoch_now = hal_->clockValid() ? hal_->epochNow() : 0;
  ctx.tz_min = cfg_.tzMin;
  ctx.boot_ms = bootMs_;
  bool usb = false;
  uint16_t mv = 0;
  hal_->readSensors(ctx.battery_pct, mv, ctx.temp_c10, ctx.rh_10, usb);
  ctx.usb_power = usb ? 1 : 0;
  ctx.valid_for_s = VALID_FOR_S;
  ctx.serving_node = parent_;
  ctx.self_child = child_;
  ctx.normal_since_ms = normalSinceEpoch_ ? msFromEpoch(normalSinceEpoch_, now) : 0;
  int k = 0;
  if (flash_[0]) ctx.marks[k++] = flash_;
  for (int i = 0; i < IOC_PAGE_MARKS && k < IOC_PAGE_MARKS; ++i) if (recent_[i][0]) ctx.marks[k++] = recent_[i];
}

void PanelApp::composeRoutine(const ioc_page_ctx& ctx) {
  const Routine& r = ROUTINES[routine_];
  char right[24];
  snprintf(right, sizeof(right), "%u de %u", static_cast<unsigned>(routine_ + 1), static_cast<unsigned>(ROUTINE_COUNT));
  int y = ioc_page_header(&dl_, level_, "Roteiro de bancada", right);
  y += ioc_dl_wrap(&dl_, 8, y, 384, 24, IOC_INK_BLACK, 0, r.name, 2) + 6;
  for (uint8_t i = 0; i < r.count && y < 470; ++i) {
    const bool current = i == step_;
    if (i < step_) ioc_dl_symbol(&dl_, 10, y + 2, 16, IOC_SYM_CHECK, IOC_INK_BLACK, 0);
    char num[8];
    snprintf(num, sizeof(num), "%u.", static_cast<unsigned>(i + 1));
    ioc_dl_text(&dl_, 32, y, 16, current ? IOC_INK_BLUE : IOC_INK_BLACK, 0, 0, num);
    const int h = ioc_dl_wrap(&dl_, 56, y, 336, current ? 24 : 16, current ? IOC_INK_BLUE : IOC_INK_BLACK, 0,
                              r.steps[i].text, current ? 3 : 2);
    if (current) ioc_dl_frame(&dl_, 4, y - 4, 392, h + 8, IOC_INK_BLUE, 0);
    y += h + 12;
  }
  if (step_ >= r.count) {
    ioc_dl_text(&dl_, 8, y, 24, IOC_INK_BLACK, 0, 384, "Roteiro concluído.");
    y += 32;
  }
  if (stepResult_[0]) y += ioc_dl_wrap(&dl_, 8, y, 384, 16, IOC_INK_BLACK, 0, stepResult_, 3) + 6;
  ioc_dl_wrap(&dl_, 8, 506, 384, 16, IOC_INK_BLACK, 0,
              "Enter ou A: marca o passo e mede. Setas: outro roteiro. Os resultados vão para o histórico da TUI.", 3);
  ioc_page_footer(&dl_, &ctx);
}

void PanelApp::composeRadio(const ioc_page_ctx& ctx) {
  int y = ioc_page_header(&dl_, level_, "Rádio neste ponto", "C: varrer");
  if (!radioValid_) {
    ioc_dl_wrap(&dl_, 8, y, 384, 24, IOC_INK_BLACK, 0,
                "Varredura indisponível nesta conexão (USB). Use o painel pelo Wi-Fi para medir os canais.", 4);
  } else {
    char line[96];
    const char* p = radio_;
    while (*p && y < 540) {
      const char* e = strchr(p, '\n');
      size_t n = e ? static_cast<size_t>(e - p) : strlen(p);
      if (n >= sizeof(line)) n = sizeof(line) - 1;
      memcpy(line, p, n);
      line[n] = '\0';
      ioc_dl_text(&dl_, 8, y, 16, IOC_INK_BLACK, IOC_DL_VOLATILE, 384, line);
      y += 20;
      p = e ? e + 1 : p + n;
    }
  }
  ioc_page_footer(&dl_, &ctx);
}

void PanelApp::composeHelp(const ioc_page_ctx& ctx) {
  int y = ioc_page_header(&dl_, level_, "Teclado, botões e sinais", PANEL_FW_VERSION);
  static const char* const lines[] = {
      "Digite: abre uma marca no instante da 1ª tecla",
      "Enter grava; sem texto, marca só o instante",
      "#etiqueta no texto: filtro no histórico da TUI",
      "Esc cancela a nota; A: intervenção física",
      "B ou Tab: página; C: atualizar a tela",
      "Energia: 1 toque atualiza ou dá ciência",
      "Segure 1,5 s ou dê 2 toques: desliga",
      "Qualquer tecla ou botão dá ciência do alerta",
      "/hora HH:MM  /data AAAA-MM-DD  /sono N",
  };
  for (const char* line : lines) {
    ioc_dl_text(&dl_, 8, y, 16, IOC_INK_BLACK, 0, 384, line);
    y += 22;
  }
  y += 6;
  ioc_dl_text(&dl_, 8, y, 16, IOC_INK_BLACK, 0, 384, "LED da rede e bipes:");
  y += 24;
  struct Legend { uint8_t ink; uint8_t sym; const char* text; };
  static const Legend legend[] = {
      {IOC_INK_GREEN, IOC_SYM_DOT, "verde: rede normal (só na USB)"},
      {IOC_INK_BLUE, IOC_SYM_DOT, "azul respirando: a rede está tratando, 1 bipe"},
      {IOC_INK_YELLOW, IOC_SYM_TRIANGLE, "amarelo piscando: atenção, 2 bipes"},
      {IOC_INK_RED, IOC_SYM_CROSS, "vermelho alternando: requer você, 3 bipes"},
  };
  for (const Legend& l : legend) {
    ioc_dl_symbol(&dl_, 10, y + 1, 16, l.sym, l.ink, 0);
    ioc_dl_text(&dl_, 32, y, 16, IOC_INK_BLACK, 0, 360, l.text);
    y += 22;
  }
  y += 4;
  ioc_dl_wrap(&dl_, 8, y, 384, 16, IOC_INK_BLACK, 0,
              "Sem ciência, os bipes voltam (1 min em vermelho, 5 min em amarelo). O outro LED mostra o painel: "
              "tecla lida (branco), nota aberta (ciano), marca gravada (verde), tela desenhando (magenta).", 4);
  char id[64];
  snprintf(id, sizeof(id), "Painel 0x%08lX%08lX · teclado %s", static_cast<unsigned long>(cfg_.uuid >> 32),
           static_cast<unsigned long>(cfg_.uuid & 0xFFFFFFFFu), hal_->keyboardPresent() ? "conectado" : "ausente");
  ioc_dl_text(&dl_, 8, 548, 16, IOC_INK_BLACK, IOC_DL_VOLATILE, 384, id);
  ioc_page_footer(&dl_, &ctx);
}

void PanelApp::draftHeader(char* out, size_t cap) const {
  // Instante da marca com segundos: é o que vai para a linha do tempo.
  if (hal_->clockValid()) {
    const int64_t at = hal_->epochNow() - static_cast<int64_t>((hal_->millis() - draftStartMs_) / 1000u) +
                       static_cast<int64_t>(cfg_.tzMin) * 60;
    const int64_t day = ((at % 86400) + 86400) % 86400;
    snprintf(out, cap, "Marca às %02d:%02d:%02d", static_cast<int>(day / 3600), static_cast<int>((day / 60) % 60),
             static_cast<int>(day % 60));
  } else {
    snprintf(out, cap, "Marca do operador");
  }
}

void PanelApp::composeDraft(const ioc_page_ctx& ctx) {
  char title[32];
  draftHeader(title, sizeof(title));
  int y = ioc_page_header(&dl_, level_, title, "Enter grava");
  ioc_dl_frame(&dl_, 6, y, 388, 150, IOC_INK_BLACK, 0);
  ioc_dl_wrap(&dl_, 14, y + 8, 372, 24, IOC_INK_BLACK, IOC_DL_VOLATILE, draftLen_ ? draft_ : "_", 5);
  y += 166;
  y += ioc_dl_wrap(&dl_, 8, y, 384, 16, IOC_INK_BLACK, 0,
                   "A marca já vale a partir da primeira tecla. Enter grava o texto no histórico da TUI "
                   "(faixa OPERATOR), com captura dos dados da rede ao redor.", 4) + 10;
  y += ioc_dl_wrap(&dl_, 8, y, 384, 16, IOC_INK_BLUE, 0,
                   "Use #etiqueta para encontrar depois (ex.: #cabo #ensaio3). Esc cancela.", 3) + 10;
  ioc_dl_wrap(&dl_, 8, y, 384, 16, IOC_INK_BLACK, 0, "Comandos: /hora HH:MM, /data AAAA-MM-DD, /sono N.", 2);
  ioc_page_footer(&dl_, &ctx);
}

void PanelApp::composeNoData(const ioc_page_ctx& ctx, uint32_t lastMs) {
  int y = ioc_page_header(&dl_, IOC_LEVEL_ATTENTION, "Sem dados da rede", nullptr);
  char t[8], line[128];
  if (lastMs) {
    ioc_page_time(&ctx, lastMs, t);
    snprintf(line, sizeof(line), "Última informação recebida às %s. O que estava nesta tela pode estar desatualizado.", t);
  } else {
    snprintf(line, sizeof(line), "Nenhum dado desde que o painel ligou.");
  }
  y += ioc_dl_wrap(&dl_, 8, y, 384, 24, IOC_INK_BLACK, 0, line, 4) + 12;
  if (mode_ == Mode::Discovery) snprintf(line, sizeof(line), "Tentando autenticar com o Node %u.", parent_);
  else snprintf(line, sizeof(line), "Aguardando a oferta de um Node. O painel precisa estar associado na TUI (F7).");
  ioc_dl_wrap(&dl_, 8, y, 384, 16, IOC_INK_BLACK, 0, line, 3);
  ioc_page_footer(&dl_, &ctx);
}

void PanelApp::composeShutdown(const ioc_page_ctx& ctx) {
  // Tela final: precisa dizer de longe que o painel está desligado.
  ioc_dl_clear(&dl_);
  int y = 28 + ioc_page_logo(&dl_, 8, 28, 384) + 16;
  ioc_dl_fill(&dl_, 0, y, IOC_DL_W, 60, IOC_INK_RED, 0);
  const char* off = "Painel desligado";
  ioc_dl_text(&dl_, (IOC_DL_W - ioc_dl_text_width(32, off)) / 2, y + 12, 32, IOC_INK_WHITE, 0, 0, off);
  y += 76;
  char t[8], line[160];
  ioc_page_time(&ctx, ctx.now_ms, t);
  snprintf(line, sizeof(line), "Desligado pelo operador às %s.", t);
  y += ioc_dl_wrap(&dl_, 8, y, 384, 24, IOC_INK_BLACK, 0, line, 2) + 10;
  y += ioc_dl_wrap(&dl_, 8, y, 384, 16, IOC_INK_BLACK, 0,
                   "Esta tela fica assim até o painel ser ligado: não se atualiza e não avisa sobre a rede. "
                   "LEDs, som, rádio e teclado estão desligados.", 4) + 10;
  snprintf(line, sizeof(line), "Última situação vista: %s.", ioc_page_level_title(level_));
  y += ioc_dl_wrap(&dl_, 8, y, 384, 16, IOC_INK_BLACK, 0, line, 2) + 14;
  ioc_dl_wrap(&dl_, 8, y, 384, 24, IOC_INK_BLUE, 0, "Para ligar: aperte o botão de energia.", 2);
  char id[64];
  snprintf(id, sizeof(id), "Painel 0x%08lX%08lX · fw %s", static_cast<unsigned long>(cfg_.uuid >> 32),
           static_cast<unsigned long>(cfg_.uuid & 0xFFFFFFFFu), PANEL_FW_VERSION);
  ioc_dl_line(&dl_, 0, 574, IOC_DL_W, 0, IOC_INK_BLACK, 0);
  ioc_dl_text(&dl_, 8, 579, 16, IOC_INK_BLACK, 0, 384, id);
}

void PanelApp::composeSplash(const ioc_page_ctx& ctx) {
  // Ligado pelo operador sobre a tela "desligado": troca por esta, já.
  ioc_dl_clear(&dl_);
  int y = 28 + ioc_page_logo(&dl_, 8, 28, 384) + 16;
  ioc_dl_fill(&dl_, 0, y, IOC_DL_W, 60, IOC_INK_GREEN, 0);
  const char* on = "Painel ligado";
  ioc_dl_text(&dl_, (IOC_DL_W - ioc_dl_text_width(32, on)) / 2, y + 12, 32, IOC_INK_WHITE, 0, 0, on);
  y += 76;
  char line[128];
  if (parent_) snprintf(line, sizeof(line), "Conectando ao Node %u.", parent_);
  else snprintf(line, sizeof(line), "Procurando um Node da rede.");
  y += ioc_dl_wrap(&dl_, 8, y, 384, 24, IOC_INK_BLACK, 0, line, 2) + 10;
  ioc_dl_wrap(&dl_, 8, y, 384, 16, IOC_INK_BLACK, 0,
              "A situação da rede aparece aqui em seguida. Enquanto isso, o LED da rede já mostra o "
              "estado: pulso laranja procurando o Node, azul recebendo a rede, verde normal.", 4);
  ioc_page_footer(&dl_, &ctx);
}

void PanelApp::compose(uint32_t now) {
  ioc_view_tick(&view_, now);
  ioc_page_ctx ctx;
  fillCtx(ctx, now);
  itemCount_ = ioc_page_assess(&view_, &ctx, items_, IOC_PAGE_MAX_ITEMS);
  level_ = ioc_page_level(items_, itemCount_);
  if (view_.snapshot_done) {
    if (level_ == IOC_LEVEL_NORMAL && normalSinceEpoch_ == 0) {
      normalSinceEpoch_ = static_cast<uint32_t>(hal_->epochNow());
      hal_->storeU32("normal", normalSinceEpoch_);
    } else if (level_ != IOC_LEVEL_NORMAL && normalSinceEpoch_ != 0) {
      normalSinceEpoch_ = 0;
      hal_->storeU32("normal", 0);
    }
    ctx.normal_since_ms = normalSinceEpoch_ ? msFromEpoch(normalSinceEpoch_, now) : 0;
  }
  ioc_dl_clear(&dl_);
  const bool noData = mode_ != Mode::Bound && (lastDataMs_ ? now - lastDataMs_ > NO_DATA_MS : now - bootMs_ > BOOT_NO_DATA_MS);
  if (composing_) {
    composeDraft(ctx);
  } else if (noData && (page_ == PAGE_SITUATION || page_ == PAGE_NETWORK)) {
    composeNoData(ctx, lastDataMs_);
  } else {
    switch (page_) {
      case PAGE_NETWORK: ioc_page_network(&dl_, &view_, &ctx, items_, itemCount_); break;
      case PAGE_ROUTINE: composeRoutine(ctx); break;
      case PAGE_RADIO: composeRadio(ctx); break;
      case PAGE_HELP: composeHelp(ctx); break;
      default: ioc_page_situation(&dl_, &view_, &ctx, items_, itemCount_); break;
    }
  }
}

void PanelApp::maybeRender(uint32_t now) {
  if (splash_) {
    // A tela mostra "Painel desligado": a primeira coisa é dizer que ligou.
    if (hal_->renderBusy()) return;
    ioc_page_ctx ctx;
    fillCtx(ctx, now);
    composeSplash(ctx);
    hal_->render(dl_);
    memcpy(&drawn_, &dl_, sizeof(drawn_));
    splash_ = false;
    hal_->storeU32("offpg", 0);
    memset(&refresh_, 0, sizeof(refresh_));
    forceRefresh_ = true;  // a página da rede entra assim que houver retrato
    hal_->log("pagina de partida desenhada");
    return;
  }
  // Sem o retrato, a página da rede seria um estado incompleto: espera-o (ou o
  // prazo da página "sem dados"), salvo quando o operador pede outra página.
  const bool networkPage = !composing_ && (page_ == PAGE_SITUATION || page_ == PAGE_NETWORK);
  const bool waitingSnapshot = !view_.snapshot_done && (mode_ == Mode::Bound ? now - sessionStartMs_ < 5000
                                                                              : now - bootMs_ < BOOT_NO_DATA_MS);
  if (waitingSnapshot && networkPage && !pageChanged_) return;
  compose(now);
  // Canal rápido: o alerta acende o LED e soa já, sem esperar o papel.
  updateAlert(now);
  if (hal_->renderBusy()) return;
  const uint32_t hash = ioc_dl_hash(&dl_);
  bool forced = forceRefresh_ || pageChanged_;
  if (composing_) {
    // Rascunho: só depois de uma pausa na digitação, e não mais que a cada 30 s.
    forced = now - lastKeyMs_ >= DRAFT_IDLE_MS && (draftDrawnMs_ == 0 || now - draftDrawnMs_ >= DRAFT_REDRAW_MS) &&
             hash != refresh_.hash;
    if (!forced) return;
    draftDrawnMs_ = now;
  }
  uint32_t next = 0;
  const uint32_t pendingBefore = refresh_.pending_since_ms;
  if (!ioc_refresh_decide(&refresh_, now, hash, level_, forced ? 1 : 0, &next)) {
    // A espera por uma melhora sobrevive ao sono: o painel acorda em outro boot.
    if (refresh_.pending_since_ms != pendingBefore) {
      hal_->storeU32("rpend", refresh_.pending_since_ms ? epochFromMs(refresh_.pending_since_ms, now) : 0);
      hal_->storeU32("rphash", refresh_.pending_hash);
    }
    return;
  }
  hal_->render(dl_);
  memcpy(&drawn_, &dl_, sizeof(drawn_));
  lastRenderMs_ = now;
  ioc_refresh_done(&refresh_, now, hash, level_, now + VALID_FOR_S * 1000u);
  saveRefresh(now);
  forceRefresh_ = false;
  pageChanged_ = false;
  flash_[0] = '\0';
  char line[64];
  snprintf(line, sizeof(line), "pagina %u desenhada nivel=%u hash=%08lX", page_, level_, static_cast<unsigned long>(hash));
  hal_->log(line);
}

void PanelApp::maybeSleep(uint32_t now) {
  uint8_t bat = 255; uint16_t mv = 0; int16_t t = 0; uint16_t rh = 0; bool usb = false;
  hal_->readSensors(bat, mv, t, rh, usb);
  if (!cfg_.canSleep || usb || composing_ || stepRunning_ || hal_->renderBusy() || forceRefresh_ || pageChanged_ ||
      splash_ || powerPressed_ || tones_.busy(now)) {
    return;
  }
  if (now - lastActivityMs_ < AWAKE_IDLE_MS) return;
  if (lastKeyMs_ && now - lastKeyMs_ < KEY_AWAKE_MS) return;
  // Alerta sem ciência: fica acordado chamando o operador por um tempo.
  if (unacked_ && annMs_ && now - annMs_ < ALERT_AWAKE_MS) return;
  const bool ready = mode_ == Mode::Bound && view_.snapshot_done && marksPending() == 0;
  const bool hopeless = mode_ != Mode::Bound && now - bootMs_ > 90000;  // sem rede: não gasta bateria esperando
  if (!ready && !hopeless) return;
  const uint32_t seconds = sleepSeconds();
  if (mode_ == Mode::Bound) sendStatus(IOC_PANEL_ST_SLEEP, static_cast<uint16_t>(seconds));
  char line[48];
  snprintf(line, sizeof(line), "dormindo por %lu s", static_cast<unsigned long>(seconds));
  hal_->log(line);
  hal_->leds(0, 0);
  hal_->sleepFor(seconds);
}

/* Desligamento ordenado: avisa o Node, desenha a tela final (com os LEDs
 * vermelhos respirando durante os 15 a 30 s do papel), apaga tudo e desliga. */
void PanelApp::shutdown() {
  if (off_ != Off::None) return;
  const uint32_t now = hal_->millis();
  off_ = Off::WaitRender;
  offStepMs_ = now;
  powerPressed_ = false;
  composing_ = false;
  scene_ = LedScene::Shutdown;
  sceneStart_ = now;
  tones_.clear();
  tones_.play(Melody::Shutdown);
  for (PanelMark& m : marks_) m.lastSendMs = now - MARK_RETRY_MS;  // última chance para as marcas pendentes
  flushMarks(now);
  if (mode_ == Mode::Bound) sendStatus(IOC_PANEL_ST_POWER_OFF, 0);
  ioc_page_ctx ctx;
  fillCtx(ctx, now);
  composeShutdown(ctx);
  hal_->storeU32("rvalid", 0);  // a próxima partida redesenha
  hal_->storeU32("offpg", 1);   // e troca a tela final pela de partida
  hal_->log("desligamento pelo operador");
}

void PanelApp::shutdownTick(uint32_t now) {
  switch (off_) {
    case Off::WaitRender:  // um desenho em curso termina antes
      if (!hal_->renderBusy() || now - offStepMs_ > 40000) {
        hal_->render(dl_);
        memcpy(&drawn_, &dl_, sizeof(drawn_));
        off_ = Off::Drawing;
        offStepMs_ = now;
      }
      break;
    case Off::Drawing:
      if (!hal_->renderBusy() || now - offStepMs_ > 45000) {
        off_ = Off::Done;
        offStepMs_ = now;
        scene_ = LedScene::None;
        tones_.play(Melody::Off);
        hal_->log("tela final desenhada");
      }
      break;
    case Off::Done:
      if (now - offStepMs_ >= 450 && !tones_.busy(now)) {
        off_ = Off::Finished;
        hal_->leds(0, 0);
        hal_->powerOff();
      }
      break;
    default:
      break;
  }
}

void PanelApp::loop() {
  const uint32_t now = hal_->millis();
  updateSignals(now);
  if (off_ != Off::None) {
    shutdownTick(now);
    return;
  }
  if (powerPressed_ && now - powerPressMs_ >= POWER_HOLD_MS) {
    hal_->log("botao de energia segurado: desligando");
    shutdown();
    return;
  }
  maintainLink(now);
  flushMarks(now);
  routineTick(now);
  if (composing_ && now - lastKeyMs_ >= DRAFT_ABANDON_MS) {
    hal_->log("nota gravada por inatividade");
    submitComposer(now);
  }
  if (splash_ || forceRefresh_ || pageChanged_ || composing_ || now - lastEvalMs_ >= 1000u) {
    lastEvalMs_ = now;
    maybeRender(now);
  }
  maybeSleep(now);
}
