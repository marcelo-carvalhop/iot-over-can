/**
 * @file ioc_isolation.h
 * @brief Um Node CAN percebe que perdeu o barramento (sem E/S).
 *
 * Com o cabo CAN solto, o módulo segue ligado: o ponto de acesso Wi-Fi continua no ar, o sensor continua
 * com a sessão aberta e os dados dele vão para um barramento que ninguém ouve. Os outros módulos, que não
 * o ouvem mais, passam a considerá-lo fora do ar e o líder reassocia o sensor a outro módulo, mas o sensor
 * só aceita uma nova oferta depois de largar a sessão que tem, e o módulo isolado nunca a larga.
 *
 * O módulo se considera isolado quando as duas coisas valem ao mesmo tempo:
 *
 *   1. nenhum quadro chegou do barramento há quiet_ms (o líder e os outros módulos falam pelo menos uma
 *      vez por segundo);
 *   2. o controlador CAN diz que as transmissões dele não são reconhecidas: erro passivo de transmissão
 *      (TEC >= 128 ou o sinal TXEP) ou bus-off (TXBO). Sem outro nó ligado ao fio, ninguém confirma o
 *      quadro e o contador sobe até 128 e fica lá.
 *
 * Só o silêncio não basta: um barramento calado com os quadros do módulo sendo confirmados é uma rede
 * parada (por exemplo, sem líder), não um módulo desligado dela. Quem pede o silêncio é a eleição, não
 * este módulo.
 *
 * A volta exige quadros chegando sem interrupção por recover_ms: um quadro solto de um contato intermitente
 * não religa o ponto de acesso.
 */
#ifndef IOC_ISOLATION_H
#define IOC_ISOLATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Um quadro recebido há menos que isto conta como "chegando" para a volta. */
#define IOC_ISO_RX_RECENT_MS  1500u

typedef struct {
    uint32_t last_rx_ms;
    uint32_t recover_since_ms;
    uint8_t  recovering;
    uint8_t  isolated;
} ioc_iso;

typedef enum {
    IOC_ISO_NONE = 0,
    IOC_ISO_ENTERED,   /* acabou de se isolar */
    IOC_ISO_EXITED     /* o barramento voltou */
} ioc_iso_event;

void ioc_iso_init(ioc_iso *s, uint32_t now_ms);

/* Chamada a cada quadro recebido do barramento. */
void ioc_iso_note_rx(ioc_iso *s, uint32_t now_ms);

/* 1 se o controlador diz que as transmissões não estão sendo confirmadas (TEC, EFLG do MCP2515). */
int ioc_iso_tx_failing(uint8_t tec, uint8_t eflg);

/* Chamada periódica. quiet_ms == 0 desliga a detecção. */
ioc_iso_event ioc_iso_poll(ioc_iso *s, uint32_t now_ms, uint8_t tec, uint8_t eflg,
                           uint32_t quiet_ms, uint32_t recover_ms);

#ifdef __cplusplus
}
#endif

#endif
