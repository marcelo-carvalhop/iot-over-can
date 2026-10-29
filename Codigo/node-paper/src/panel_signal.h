#pragma once
/*
 * Canal rápido do painel: os dois LEDs RGB do topo e o alto-falante.
 *
 * O papel leva 15 a 30 s para mostrar uma página; os LEDs e os bipes mostram
 * na hora o que importa. A hierarquia acompanha a relevância da página:
 *
 *   nível                 LED da rede (LED 0)             bipes
 *   Rede normal           verde (só na USB)               -
 *   A rede está tratando  azul respirando                 1
 *   Atenção               amarelo piscando 1 Hz           2, repetidos a cada 5 min
 *   Requer você           vermelho alternando nos dois    3, repetidos a cada 1 min
 *
 * até o operador dar ciência (qualquer botão ou tecla); depois a cor fica
 * fixa e os bipes param. O LED 1 mostra o próprio painel: tecla lida
 * (branco), marca aberta (ciano), marca gravada pelo Node (verde), papel
 * desenhando (magenta respirando).
 *
 * Sem dependência de hardware: o firmware e a bancada virtual usam este
 * mesmo código.
 */
#include <stdint.h>

constexpr uint32_t ledRgb(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

enum class Melody : uint8_t {
  Handling,   /* 1 bipe: a rede está tratando */
  Attention,  /* 2 bipes */
  You,        /* 3 bipes agudos */
  Resolved,   /* voltou ao normal */
  Flag,       /* marca aberta (primeira tecla, A, Enter) */
  Sent,       /* nota enviada */
  Saved,      /* o Node confirmou a marca */
  Ack,        /* ciência do alerta */
  Boot,       /* ligado pelo operador */
  Shutdown,   /* desligando */
  Off,        /* tela final desenhada */
  Error,      /* cancelado ou comando desconhecido */
  Click       /* atualização pedida */
};

/* Frequências dos bipes de alerta (a bancada conta os bipes por elas). */
constexpr uint16_t TONE_ATTENTION_HZ = 2400;
constexpr uint16_t TONE_YOU_HZ = 2900;
constexpr uint16_t TONE_HANDLING_HZ = 1800;

struct Tone {
  uint16_t hz;  /* 0 = pausa */
  uint16_t ms;
};

class ToneQueue {
 public:
  void play(Melody m);
  void clear() { count_ = 0; }
  /* Próxima nota a tocar agora; false enquanto a nota anterior soa ou na pausa. */
  bool next(uint32_t now, Tone& out);
  bool busy(uint32_t now) const { return count_ > 0 || static_cast<int32_t>(now - freeAt_) < 0; }

 private:
  static constexpr uint8_t CAP = 32;
  Tone q_[CAP] = {};
  uint8_t head_ = 0;
  uint8_t count_ = 0;
  uint32_t freeAt_ = 0;
};

enum class LedScene : uint8_t { None, Boot, Shutdown };

struct LedInputs {
  uint32_t now = 0;
  uint8_t level = 0;          /* IOC_LEVEL_* */
  bool synced = false;        /* retrato da rede recebido */
  bool session = false;       /* sessão autenticada com o Node */
  bool usb = false;           /* alimentado: o LED verde fica aceso */
  bool unacked = false;       /* alerta sem ciência do operador */
  uint32_t awakeSinceMs = 0;  /* na bateria, o verde aparece só ao acordar */
  bool composing = false;
  bool markPending = false;
  bool rendering = false;
  uint32_t keyFlashUntil = 0;
  uint32_t savedFlashUntil = 0;
  uint32_t clickFlashUntil = 0;
  uint16_t holdPermille = 0;  /* botão de energia segurado: 0..1000 até desligar */
  LedScene scene = LedScene::None;
  uint32_t sceneStart = 0;
};

/* Cores dos dois LEDs (0xRRGGBB) neste instante. */
void ledFrame(const LedInputs& in, uint32_t out[2]);
