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
constexpr uint32_t DRAFT_IDLE_MS = 2500;
constexpr uint32_t DRAFT_REDRAW_MS = 30000;
constexpr uint32_t AWAKE_IDLE_MS = 20000;
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

}  // namespace

/* ------------------------------------------------------------------ início */

void PanelApp::begin(PanelHal* hal, const PanelConfig& cfg) {
  hal_ = hal;
  cfg_ = cfg;
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
  drawnLevel_ = static_cast<uint8_t>(hal_->loadU32("alevel", 0));
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
  forceRefresh_ = false;  // a página no papel continua válida
}

void PanelApp::saveRefresh(uint32_t now) {
  hal_->storeU32("rhash", refresh_.hash);
  hal_->storeU32("rlevel", refresh_.level);
  hal_->storeU32("rcount", refresh_.count);
  hal_->storeU32("rlast", epochFromMs(refresh_.last_ms, now));
  hal_->storeU32("rvalid", epochFromMs(refresh_.valid_until_ms, now));
}

/* ------------------------------------------------------------------ enlace */

void PanelApp::startDiscovery(uint8_t node) {
  const uint32_t now = hal_->millis();
  if (node != parent_ || mode_ == Mode::Idle) hal_->joinNode(node);
  parent_ = node;
  mode_ = Mode::Discovery;
  discoverySinceMs_ = now;
  nextHelloMs_ = now;
  memset(sessionKey_, 0, sizeof(sessionKey_));
  hal_->storeU32("node", node);
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
  for (int i = 0; i < IOC_LINK_NONCE_LEN; ++i) {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    nonce_[i] = static_cast<uint8_t>(rng_ >> 24);
  }
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
                                  (hal_->keyboardPresent() ? IOC_PANEL_ST_KEYBOARD : 0));
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
  snprintf(line, sizeof(line), "%s %s", t, m.text[0] ? m.text : "intervenção física");
  memcpy(recent_[0], line, sizeof(recent_[0]) - 1);
  recent_[0][sizeof(recent_[0]) - 1] = '\0';
  for (int i = 0; i < IOC_PAGE_MARKS; ++i) {
    char key[4] = {'m', static_cast<char>('0' + i), 0, 0};
    hal_->storeStr(key, recent_[i]);
  }
}

void PanelApp::addMark(uint8_t kind, uint8_t step, const char* text) {
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
  slot->createdMs = hal_->millis();
  slot->lastSendMs = slot->createdMs - MARK_RETRY_MS;
  snprintf(slot->text, sizeof(slot->text), "%s", text ? text : "");
  hal_->storeU32("mseq", markSeq_);
  if (kind != IOC_PANEL_MARK_RESULT || text) rememberMark(*slot);
  hal_->beep(2);
  flushMarks(slot->createdMs);
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
  addMark(IOC_PANEL_MARK_STEP, static_cast<uint8_t>(step_ + 1), text);
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
  addMark(IOC_PANEL_MARK_RESULT, static_cast<uint8_t>(step_ + 1), text);
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

void PanelApp::submitComposer() {
  composing_ = false;
  draft_[draftLen_] = '\0';
  if (draftLen_ > 0 && !slashCommand(draft_)) addMark(IOC_PANEL_MARK_NOTE, 0, draft_);
  draftLen_ = 0;
  draft_[0] = '\0';
  pageChanged_ = true;
}

void PanelApp::onKey(uint8_t code) {
  const uint32_t now = hal_->millis();
  userActivity(now);
  lastKeyMs_ = now;
  hal_->led(level_, true);
  if (composing_) {
    if (code == KEY_ENTER) {
      submitComposer();
    } else if (code == KEY_ESC) {
      composing_ = false;
      draftLen_ = 0;
      pageChanged_ = true;
    } else if (code == KEY_BACKSPACE) {
      while (draftLen_ > 0) {
        const unsigned char c = static_cast<unsigned char>(draft_[--draftLen_]);
        if ((c & 0xC0u) != 0x80u) break;
      }
      draft_[draftLen_] = '\0';
    } else if (code >= 0x20 && code < 0x7F && draftLen_ < IOC_PANEL_MARK_TEXT_MAX) {
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
      else { composing_ = true; draftLen_ = 0; draft_[0] = '\0'; }
      break;
    default:
      if (code >= 0x20 && code < 0x7F) {
        composing_ = true;
        draftLen_ = 0;
        draft_[draftLen_++] = static_cast<char>(code);
        draft_[draftLen_] = '\0';
      }
      break;
  }
  if (pageChanged_ && page_ == PAGE_RADIO) radioValid_ = hal_->radioSurvey(radio_, sizeof(radio_));
  if (pageChanged_) hal_->storeU32("page", page_);
}

void PanelApp::onButton(char button) {
  const uint32_t now = hal_->millis();
  userActivity(now);
  if (button == 'A') {
    if (page_ == PAGE_ROUTINE) routineMark();
    else addMark(IOC_PANEL_MARK_BUTTON, 0, nullptr);
    pageChanged_ = true;
  } else if (button == 'B') {
    onKey(KEY_TAB);
  } else if (button == 'C') {
    if (page_ == PAGE_RADIO) radioValid_ = hal_->radioSurvey(radio_, sizeof(radio_));
    forceRefresh_ = true;
  }
}

void PanelApp::onPowerClick(bool doubleClick) {
  userActivity(hal_->millis());
  if (doubleClick) shutdown();
  else forceRefresh_ = true;  // atualiza agora
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
  ioc_dl_wrap(&dl_, 8, 520, 384, 16, IOC_INK_BLACK, 0,
              "Enter ou A: marca o passo e mede. Setas: outro roteiro. Os resultados vão para o histórico da TUI.", 2);
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
  int y = ioc_page_header(&dl_, level_, "Teclado e botões", PANEL_FW_VERSION);
  static const char* const lines[] = {
      "A: marca uma intervenção física agora",
      "B ou Tab: próxima página; C: atualizar",
      "Energia: 1 toque atualiza; 2 toques desliga",
      "Digite para escrever uma nota; Enter envia",
      "Esc: cancela a nota / volta à Situação",
      "/hora HH:MM e /data AAAA-MM-DD: relógio",
      "/sono N: verificar a cada N min (bateria)",
      "Roteiro: Enter marca o passo e mede",
      "Setas na página Roteiro: outro roteiro",
  };
  for (const char* line : lines) {
    ioc_dl_text(&dl_, 8, y, 16, IOC_INK_BLACK, 0, 384, line);
    y += 24;
  }
  char id[64];
  snprintf(id, sizeof(id), "Painel 0x%08lX%08lX", static_cast<unsigned long>(cfg_.uuid >> 32),
           static_cast<unsigned long>(cfg_.uuid & 0xFFFFFFFFu));
  ioc_dl_text(&dl_, 8, y + 12, 16, IOC_INK_BLACK, 0, 384, id);
  ioc_page_footer(&dl_, &ctx);
}

void PanelApp::composeDraft(const ioc_page_ctx& ctx) {
  int y = ioc_page_header(&dl_, level_, "Nota do operador", "Enter envia");
  ioc_dl_frame(&dl_, 6, y, 388, 150, IOC_INK_BLACK, 0);
  ioc_dl_wrap(&dl_, 14, y + 8, 372, 24, IOC_INK_BLACK, IOC_DL_VOLATILE, draftLen_ ? draft_ : "_", 5);
  y += 166;
  ioc_dl_wrap(&dl_, 8, y, 384, 16, IOC_INK_BLACK, 0,
              "A nota vai para o histórico da TUI com o horário de agora. Esc cancela. Comandos: /hora, /data, /sono.", 3);
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
  ioc_dl_clear(&dl_);
  int y = ioc_page_header(&dl_, IOC_LEVEL_NORMAL, "Painel desligado", nullptr);
  char t[8], line[160];
  ioc_page_time(&ctx, ctx.now_ms, t);
  snprintf(line, sizeof(line), "Desligado pelo operador às %s.", t);
  y += ioc_dl_wrap(&dl_, 8, y + 8, 384, 24, IOC_INK_BLACK, 0, line, 2) + 20;
  snprintf(line, sizeof(line), "Esta tela não se atualiza mais. Última situação: %s (%d %s).",
           ioc_page_level_title(level_), itemCount_, itemCount_ == 1 ? "item" : "itens");
  y += ioc_dl_wrap(&dl_, 8, y, 384, 16, IOC_INK_BLACK, 0, line, 3) + 12;
  ioc_dl_wrap(&dl_, 8, y, 384, 16, IOC_INK_BLACK, 0, "Aperte o botão de energia uma vez para ligar.", 2);
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
  const bool waitingSnapshot = mode_ == Mode::Bound && !view_.snapshot_done && now - sessionStartMs_ < 5000;
  if (waitingSnapshot) return;  // o retrato chega em seguida: não desenhar um estado incompleto
  compose(now);
  // Canal rápido: o agravamento acende o LED e soa já, sem esperar o papel.
  if (view_.snapshot_done && level_ >= IOC_LEVEL_ATTENTION && level_ > drawnLevel_) hal_->beep(1);
  if (view_.snapshot_done && level_ != drawnLevel_) {
    drawnLevel_ = level_;
    hal_->storeU32("alevel", drawnLevel_);
  }
  hal_->led(level_, true);
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
  if (!ioc_refresh_decide(&refresh_, now, hash, level_, forced ? 1 : 0, &next)) return;
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
  if (!cfg_.canSleep || usb || composing_ || stepRunning_ || hal_->renderBusy() || forceRefresh_ || pageChanged_) return;
  if (now - lastActivityMs_ < AWAKE_IDLE_MS) return;
  const bool ready = mode_ == Mode::Bound && view_.snapshot_done && marksPending() == 0;
  const bool hopeless = mode_ != Mode::Bound && now - bootMs_ > 90000;  // sem rede: não gasta bateria esperando
  if (!ready && !hopeless) return;
  const uint32_t seconds = sleepSeconds();
  if (mode_ == Mode::Bound) sendStatus(IOC_PANEL_ST_SLEEP, static_cast<uint16_t>(seconds));
  char line[48];
  snprintf(line, sizeof(line), "dormindo por %lu s", static_cast<unsigned long>(seconds));
  hal_->log(line);
  hal_->sleepFor(seconds);
}

void PanelApp::shutdown() {
  if (shuttingDown_) return;
  shuttingDown_ = true;
  const uint32_t now = hal_->millis();
  if (mode_ == Mode::Bound) sendStatus(IOC_PANEL_ST_POWER_OFF, 0);
  hal_->beep(3);
  ioc_page_ctx ctx;
  fillCtx(ctx, now);
  composeShutdown(ctx);
  hal_->render(dl_);
  memcpy(&drawn_, &dl_, sizeof(drawn_));
  hal_->storeU32("rvalid", 0);  // a próxima partida redesenha
  hal_->log("desligamento pelo operador");
  hal_->powerOff();
}

void PanelApp::loop() {
  if (shuttingDown_) return;
  const uint32_t now = hal_->millis();
  maintainLink(now);
  flushMarks(now);
  routineTick(now);
  if (forceRefresh_ || pageChanged_ || composing_ || now - lastEvalMs_ >= 1000u) {
    lastEvalMs_ = now;
    maybeRender(now);
  }
  maybeSleep(now);
}
