/**
 * @file ioc_selftest.h
 * @brief Autoteste do Módulo CAN: quadros do laço interno do MCP2515 e leitura dos contadores de erro (sem E/S).
 *
 * Logo depois de gravar uma placa, o que mais se quer saber é se o controlador CAN responde e se o
 * barramento está em ordem. O comando SELFTEST do firmware coloca o MCP2515 em modo de laço interno (o
 * controlador devolve para si o que transmite, sem tocar o barramento), envia alguns quadros e confere
 * o que volta. Isso prova o caminho SPI, o controlador e o pino de interrupção, que são as causas
 * comuns de uma placa que "não conversa" depois de montada; a ligação ao transceptor e ao par trançado
 * não passa pelo laço interno e só a bancada mostra.
 *
 * Esta biblioteca só define os quadros de teste, compara o que voltou e classifica os contadores de
 * erro. Trocar o modo do controlador, enviar e receber é com o firmware (src/autoteste.cpp).
 */
#ifndef IOC_SELFTEST_H
#define IOC_SELFTEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IOC_ST_PASS = 0,
    IOC_ST_WARN = 1,   /* funciona, mas há algo a olhar */
    IOC_ST_FAIL = 2    /* não funciona */
} ioc_st_level;

/* Quadros do laço interno. Os identificadores somam todos os 11 bits (0x555 e 0x2AA são complementares) e
   isolam um bit baixo (0x001) e o mais alto (0x7F0, o de menor prioridade que o projeto deixa sem uso);
   os dados têm 55/AA alternados e um bit andando em cada posição, o que acusa bit preso e linhas em curto. */
#define IOC_ST_FRAMES 4

typedef struct {
    uint32_t id;
    uint8_t  len;
    uint8_t  data[8];
} ioc_st_frame;

/* Quadro n (0 a IOC_ST_FRAMES - 1); devolve 0 se n está fora da faixa. */
int ioc_st_frame_at(uint8_t n, ioc_st_frame *out);

typedef enum {
    IOC_ST_LB_OK = 0,
    IOC_ST_LB_NO_FRAME,   /* nada voltou: pino de interrupção sem sinal, ou o controlador não entrou no modo laço */
    IOC_ST_LB_ID,         /* voltou com outro identificador */
    IOC_ST_LB_LEN,        /* voltou com outro tamanho */
    IOC_ST_LB_DATA        /* voltou com dados diferentes: bit preso ou ruído no SPI */
} ioc_st_lb_result;

/* Compara o que voltou (got) com o que foi enviado; got == NULL significa que nada voltou. */
ioc_st_lb_result ioc_st_loopback_compare(const ioc_st_frame *sent, const ioc_st_frame *got);

/* Texto curto, sem espaços, para a linha de resultado: OK, SEM_QUADRO, ID, TAMANHO, DADOS. */
const char *ioc_st_lb_text(ioc_st_lb_result r);

/* O que conferir na placa quando o resultado não é OK (português, uma linha). */
const char *ioc_st_lb_hint(ioc_st_lb_result r);

/* Contadores do controlador (REC, TEC e o registrador EFLG do MCP2515). Devolve o pior nível encontrado e,
   em *reason, um texto curto sem espaços: OK, ESTOURO_DE_RECEPCAO, AVISO_DE_ERRO, CONTADOR_ALTO,
   ERRO_PASSIVO ou BUS_OFF (o primeiro, em ordem de gravidade). */
ioc_st_level ioc_st_classify_errors(uint8_t rec, uint8_t tec, uint8_t eflg, const char **reason);

/* O que conferir na placa ou no barramento para o motivo devolvido acima (português, uma linha). */
const char *ioc_st_error_hint(const char *reason);

const char *ioc_st_level_name(ioc_st_level level);   /* PASS, WARN, FAIL */
ioc_st_level ioc_st_worst(ioc_st_level a, ioc_st_level b);

#ifdef __cplusplus
}
#endif

#endif
