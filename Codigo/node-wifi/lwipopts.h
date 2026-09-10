/**
 * @file lwipopts.h
 * @brief Configuração mínima do lwIP para pico_cyw43_arch_lwip_threadsafe_background,
 * modo NO_SYS=1 (sem RTOS), UDP apenas (discovery + telemetria).
 *
 * Este arquivo estava AUSENTE do projeto original — é obrigatório para
 * qualquer build que linke pico_cyw43_arch_lwip_*; sem ele a build não
 * compila (o SDK inclui lwipopts.h internamente). Os valores abaixo seguem
 * o padrão usado nos exemplos oficiais do pico-examples para
 * threadsafe_background, ajustados para o tamanho de payload usado aqui
 * (telemetria com até ~1472 bytes, ver NET_MAX_SAFE_PAYLOAD_BYTES).
 */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// Sem sistema operacional/RTOS: lwIP roda via callbacks/polling cooperativo.
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// Este projeto só usa UDP (discovery broadcast + telemetria unicast).
#define LWIP_UDP                    1
#define LWIP_TCP                    0
#define LWIP_DHCP                   1 // IP estático (ver edge_net_init)
#define LWIP_DNS                    0
#define LWIP_IGMP                   0

// Memória: dimensionada para caber o maior pacote do protocolo
// (telemetria com bins de FFT, até NET_MAX_SAFE_PAYLOAD_BYTES ~1472B) com
// folga para os pacotes de controle/discovery correndo em paralelo.
#define MEM_ALIGNMENT                4
#define MEM_SIZE                     (8 * 1024)
#define MEMP_NUM_UDP_PCB             4
#define MEMP_NUM_PBUF                16
#define PBUF_POOL_SIZE                8
#define PBUF_POOL_BUFSIZE            1536 // > NET_MAX_SAFE_PAYLOAD_BYTES + cabeçalhos

// Suporte a broadcast (necessário para CMD_BEACON_BROADCAST)
#define IP_SOF_BROADCAST             1
#define IP_SOF_BROADCAST_RECV        1

// Estatísticas/checksums: desliga stats (economiza RAM/flash), mantém
// checksums de hardware/software padrão do lwIP.
#define LWIP_STATS                   0
#define LWIP_CHKSUM_ALGORITHM        3

// Necessário pela arquitetura threadsafe_background do pico_cyw43_arch.
#define LWIP_TIMEVAL_PRIVATE          0

#ifndef NDEBUG
#define LWIP_DEBUG                   0 // ligue para depuração de rede em bancada
#endif

#endif // LWIPOPTS_H
