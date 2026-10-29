#include "panel_signal.h"

#include "ioc_page.h"

namespace {

constexpr uint16_t NOTE_GAP_MS = 25;

const Tone M_HANDLING[] = {{TONE_HANDLING_HZ, 90}};
const Tone M_ATTENTION[] = {{TONE_ATTENTION_HZ, 120}, {0, 120}, {TONE_ATTENTION_HZ, 120}};
const Tone M_YOU[] = {{TONE_YOU_HZ, 140}, {0, 110}, {TONE_YOU_HZ, 140}, {0, 110}, {TONE_YOU_HZ, 140}};
const Tone M_RESOLVED[] = {{1300, 110}, {1950, 170}};
const Tone M_FLAG[] = {{3200, 30}};
const Tone M_SENT[] = {{2600, 40}};
const Tone M_SAVED[] = {{2000, 60}, {2700, 90}};
const Tone M_ACK[] = {{1600, 70}, {2200, 70}};
const Tone M_BOOT[] = {{880, 90}, {1320, 90}, {1760, 140}};
const Tone M_SHUTDOWN[] = {{1760, 110}, {1320, 110}, {880, 170}};
const Tone M_OFF[] = {{660, 320}};
const Tone M_ERROR[] = {{400, 220}};
const Tone M_CLICK[] = {{2600, 20}};

/* Onda triangular entre lo e hi, em degraus de 4 para poupar escritas nos LEDs. */
uint8_t breathe(uint32_t t, uint32_t period, uint8_t lo, uint8_t hi) {
  const uint32_t p = t % period;
  const uint32_t half = period / 2;
  const uint32_t x = p < half ? p : period - p;
  const uint32_t v = lo + (static_cast<uint32_t>(hi - lo) * x) / half;
  return static_cast<uint8_t>(v & ~3u);
}

bool before(uint32_t now, uint32_t until) { return static_cast<int32_t>(until - now) > 0; }

}  // namespace

void ToneQueue::play(Melody m) {
  const Tone* seq = nullptr;
  uint8_t n = 0;
#define IOC_MELODY(name) seq = name; n = sizeof(name) / sizeof(name[0]); break
  switch (m) {
    case Melody::Handling: IOC_MELODY(M_HANDLING);
    case Melody::Attention: IOC_MELODY(M_ATTENTION);
    case Melody::You: IOC_MELODY(M_YOU);
    case Melody::Resolved: IOC_MELODY(M_RESOLVED);
    case Melody::Flag: IOC_MELODY(M_FLAG);
    case Melody::Sent: IOC_MELODY(M_SENT);
    case Melody::Saved: IOC_MELODY(M_SAVED);
    case Melody::Ack: IOC_MELODY(M_ACK);
    case Melody::Boot: IOC_MELODY(M_BOOT);
    case Melody::Shutdown: IOC_MELODY(M_SHUTDOWN);
    case Melody::Off: IOC_MELODY(M_OFF);
    case Melody::Error: IOC_MELODY(M_ERROR);
    case Melody::Click: IOC_MELODY(M_CLICK);
  }
#undef IOC_MELODY
  for (uint8_t i = 0; i < n && count_ < CAP; ++i) q_[(head_ + count_++) % CAP] = seq[i];
}

bool ToneQueue::next(uint32_t now, Tone& out) {
  while (count_ > 0 && static_cast<int32_t>(now - freeAt_) >= 0) {
    const Tone t = q_[head_];
    head_ = static_cast<uint8_t>((head_ + 1) % CAP);
    --count_;
    if (t.hz == 0) {  // pausa
      freeAt_ = now + t.ms;
      continue;
    }
    freeAt_ = now + t.ms + NOTE_GAP_MS;
    out = t;
    return true;
  }
  return false;
}

void ledFrame(const LedInputs& in, uint32_t out[2]) {
  const uint32_t t = in.now;
  out[0] = out[1] = 0;
  if (in.scene == LedScene::Shutdown) {  // desligando: os dois em vermelho, respirando
    const uint8_t v = breathe(t - in.sceneStart, 1600, 8, 120);
    out[0] = out[1] = ledRgb(v, 0, 0);
    return;
  }
  if (in.holdPermille) {  // segurando o botão: o vermelho cresce até desligar
    const uint8_t v = static_cast<uint8_t>(16 + (static_cast<uint32_t>(in.holdPermille) * 140u) / 1000u);
    out[0] = ledRgb(v, 0, 0);
    out[1] = in.holdPermille >= 500 ? ledRgb(v, 0, 0) : 0;
    return;
  }
  if (in.scene == LedScene::Boot) {  // ligado: varredura verde
    const uint32_t e = t - in.sceneStart;
    if (e < 400) out[0] = ledRgb(0, 90, 0);
    else if (e < 800) out[1] = ledRgb(0, 90, 0);
    else out[0] = out[1] = ledRgb(40, 60, 40);
    return;
  }
  if (in.unacked && in.level >= IOC_LEVEL_YOU) {  // máxima saliência: alternância vermelha
    const bool phase = ((t / 200u) & 1u) != 0;
    out[0] = phase ? ledRgb(150, 0, 0) : 0;
    out[1] = phase ? 0 : ledRgb(150, 0, 0);
    return;
  }
  if (!in.session) {
    out[0] = (t % 2000u) < 120u ? ledRgb(60, 24, 0) : 0;  // procurando o Node: pulso laranja
  } else if (!in.synced) {
    out[0] = (t % 1000u) < 120u ? ledRgb(0, 0, 60) : 0;  // recebendo o retrato
  } else {
    switch (in.level) {
      case IOC_LEVEL_HANDLING: out[0] = ledRgb(0, 0, breathe(t, 3000, 8, 72)); break;
      case IOC_LEVEL_ATTENTION:
        out[0] = in.unacked ? (((t / 500u) & 1u) ? 0 : ledRgb(110, 70, 0)) : ledRgb(40, 26, 0);
        break;
      case IOC_LEVEL_YOU: out[0] = ledRgb(56, 0, 0); break;
      default: out[0] = (in.usb || t - in.awakeSinceMs < 3000u) ? ledRgb(0, 24, 0) : 0; break;
    }
  }
  if (before(t, in.keyFlashUntil)) out[1] = ledRgb(90, 90, 90);
  else if (before(t, in.savedFlashUntil)) out[1] = ledRgb(0, 110, 0);
  else if (before(t, in.clickFlashUntil)) out[1] = ledRgb(60, 60, 60);
  else if (in.composing) out[1] = ledRgb(0, 48, 48);
  else if (in.markPending) out[1] = ((t / 125u) & 1u) ? ledRgb(0, 60, 60) : 0;
  else if (in.rendering) {
    const uint8_t v = breathe(t, 2400, 4, 44);
    out[1] = ledRgb(v, 0, v);
  }
}
