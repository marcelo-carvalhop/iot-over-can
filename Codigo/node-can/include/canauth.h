#ifndef CANAUTH_H
#define CANAUTH_H

#include <ACAN2515.h>
#include <stdint.h>

/*
 * Autenticação de origem dos comandos da Probe 00 no barramento CAN (ioc_canauth.h). Desligada por padrão:
 * compile com IOT_CAN_AUTH=1 em todos os módulos E na Probe (scripts: --auth-can). Ligada,
 *
 *   - a Probe transmite, logo depois de cada comando de sensor sem fio (0x304) e de cada comando de ensaio
 *     (0x305), um quadro de etiqueta (0x307) com a prova de que o comando saiu de quem tem a chave;
 *   - os módulos guardam o comando e só o executam quando a etiqueta chega e confere (ou descartam, e contam).
 *
 * Quem apenas liga um dispositivo ao cabo deixa de conseguir comandar os módulos. Quem tem a chave de
 * enlace de um módulo (a mesma de todos) ainda consegue. O que fica de fora: o controle 0x080, a eleição, o
 * líder, o sinal de presença e os dados, que têm vários emissores ou nenhum comando a proteger; ver
 * «Autenticação de origem dos comandos» em Documentacao/seguranca.md.
 *
 * A Probe passa a precisar da chave de enlace (IOT_LINK_MASTER_KEY_HEX); sem ela a compilação falha.
 *
 * Comandos seriais:
 *   CANAUTH            estado e contadores
 *   CANAUTH SYNC <n>   (só nos módulos) acerta a última atualidade aceita; use depois de trocar a Probe
 *                      (SYNC 0) ou de gravar um módulo novo com a Probe muito usada (o valor que a Probe mostra)
 */

#ifndef IOT_CAN_AUTH
#define IOT_CAN_AUTH 0
#endif

enum CanAuthVerdict : uint8_t {
  CANAUTH_PASS,       // não é da autenticação: siga o fluxo normal com o quadro recebido
  CANAUTH_CONSUMED,   // guardado à espera da etiqueta, ou descartado: não faça mais nada com ele
  CANAUTH_RELEASE     // a etiqueta confere: processe o quadro devolvido em `released`
};

#if IOT_CAN_AUTH

void canauthInit();
void canauthPoll();   // descarta o comando que ficou sem etiqueta

// Probe: monta a etiqueta do comando que acabou de ser transmitido. false se o quadro não é protegido.
bool canauthTagFor(const CANMessage& sent, CANMessage& tag);
void canauthTagNotSent();   // a etiqueta não coube na fila do controlador

// Módulos: cada quadro recebido passa por aqui antes de ser tratado.
CanAuthVerdict canauthOnReceive(const CANMessage& rx, CANMessage& released);

bool canauthHandleSerialCommand(const char* command);

#else

inline void canauthInit() {}
inline void canauthPoll() {}
inline bool canauthTagFor(const CANMessage&, CANMessage&) { return false; }
inline void canauthTagNotSent() {}
inline CanAuthVerdict canauthOnReceive(const CANMessage&, CANMessage&) { return CANAUTH_PASS; }
inline bool canauthHandleSerialCommand(const char*) { return false; }

#endif

#endif
