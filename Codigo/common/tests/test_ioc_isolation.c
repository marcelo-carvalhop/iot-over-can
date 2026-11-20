/* Isolamento do Node CAN: quando se considera fora do barramento e quando volta. */
#include <stdio.h>

#include "ioc_isolation.h"

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

#define QUIET   6000u
#define RECOVER 3000u

#define TEC_OK      0u
#define TEC_PASSIVE 128u
#define EFLG_NONE   0x00u
#define EFLG_TXEP   0x10u
#define EFLG_TXBO   0x20u

static void test_a_working_bus_never_isolates(void) {
    ioc_iso s;
    ioc_iso_init(&s, 0);
    for (uint32_t t = 0; t < 60000; t += 100) {
        if (t % 1000 == 0) ioc_iso_note_rx(&s, t);   /* o heartbeat do líder, uma vez por segundo */
        CHECK(ioc_iso_poll(&s, t, TEC_OK, EFLG_NONE, QUIET, RECOVER) == IOC_ISO_NONE);
    }
    CHECK(!s.isolated);
}

static void test_silence_alone_is_a_stopped_network_not_isolation(void) {
    ioc_iso s;
    ioc_iso_init(&s, 0);
    for (uint32_t t = 0; t < 120000; t += 100) {
        CHECK(ioc_iso_poll(&s, t, TEC_OK, EFLG_NONE, QUIET, RECOVER) == IOC_ISO_NONE);   /* nada transmitido sem confirmação */
    }
    CHECK(!s.isolated);
}

static void test_failing_transmissions_alone_do_not_isolate_while_frames_arrive(void) {
    ioc_iso s;
    ioc_iso_init(&s, 0);
    for (uint32_t t = 0; t < 30000; t += 100) {
        if (t % 500 == 0) ioc_iso_note_rx(&s, t);
        CHECK(ioc_iso_poll(&s, t, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_NONE);
    }
    CHECK(!s.isolated);
}

static void test_silence_plus_unacknowledged_transmissions_isolates_at_the_boundary(void) {
    ioc_iso s;
    ioc_iso_init(&s, 1000);
    CHECK(ioc_iso_poll(&s, 1000 + QUIET - 1, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_NONE);
    CHECK(ioc_iso_poll(&s, 1000 + QUIET, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_ENTERED);
    CHECK(s.isolated);
    /* O evento sai uma vez só. */
    CHECK(ioc_iso_poll(&s, 1000 + QUIET + 100, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_NONE);
}

static void test_each_failure_signal_counts(void) {
    CHECK(ioc_iso_tx_failing(128, 0x00) == 1);
    CHECK(ioc_iso_tx_failing(255, 0x00) == 1);
    CHECK(ioc_iso_tx_failing(127, 0x00) == 0);
    CHECK(ioc_iso_tx_failing(0, EFLG_TXEP) == 1);
    CHECK(ioc_iso_tx_failing(0, EFLG_TXBO) == 1);
    CHECK(ioc_iso_tx_failing(0, 0x01 | 0x02 | 0x04 | 0x08 | 0x40 | 0x80) == 0);   /* avisos e estouro de recepção não */
    ioc_iso s;
    ioc_iso_init(&s, 0);
    CHECK(ioc_iso_poll(&s, QUIET, 0, EFLG_TXBO, QUIET, RECOVER) == IOC_ISO_ENTERED);   /* bus-off */
}

static void test_the_bus_must_stay_back_for_the_whole_recovery_time(void) {
    ioc_iso s;
    ioc_iso_init(&s, 0);
    CHECK(ioc_iso_poll(&s, QUIET, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_ENTERED);

    /* Um quadro solto de contato intermitente não basta, e a contagem recomeça depois da lacuna. */
    uint32_t t = QUIET + 1000;
    ioc_iso_note_rx(&s, t);
    CHECK(ioc_iso_poll(&s, t, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_NONE);
    for (t += 100; t < QUIET + 1000 + 2000; t += 100) {    /* nenhum quadro mais: a lacuna passa de 1,5 s */
        CHECK(ioc_iso_poll(&s, t, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_NONE);
    }
    CHECK(s.isolated && !s.recovering);

    /* Agora com quadros a cada segundo: volta depois de RECOVER ms, nem um milissegundo antes. */
    const uint32_t start = t;
    uint32_t returned_at = 0;
    for (; t < start + 10000; t += 100) {
        if ((t - start) % 1000 == 0) ioc_iso_note_rx(&s, t);
        if (ioc_iso_poll(&s, t, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_EXITED) {
            returned_at = t;
            break;
        }
    }
    CHECK(returned_at == start + RECOVER);
    CHECK(!s.isolated);
}

static void test_it_can_isolate_again_after_returning(void) {
    ioc_iso s;
    ioc_iso_init(&s, 0);
    uint32_t t = QUIET;
    CHECK(ioc_iso_poll(&s, t, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_ENTERED);
    for (; t < QUIET + 8000; t += 100) {
        ioc_iso_note_rx(&s, t);
        if (ioc_iso_poll(&s, t, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_EXITED) break;
    }
    CHECK(!s.isolated);
    t += 100;
    ioc_iso_note_rx(&s, t);
    CHECK(ioc_iso_poll(&s, t + QUIET, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_ENTERED);
}

static void test_the_detection_can_be_turned_off(void) {
    ioc_iso s;
    ioc_iso_init(&s, 0);
    CHECK(ioc_iso_poll(&s, 100000, TEC_PASSIVE, EFLG_TXBO, 0, RECOVER) == IOC_ISO_NONE);
    CHECK(!s.isolated);
    /* Desligada com o módulo já isolado, ele não fica preso no estado. */
    CHECK(ioc_iso_poll(&s, 100100, TEC_PASSIVE, EFLG_TXBO, QUIET, RECOVER) == IOC_ISO_ENTERED);
    CHECK(ioc_iso_poll(&s, 100200, TEC_PASSIVE, EFLG_TXBO, 0, RECOVER) == IOC_ISO_EXITED);
    CHECK(!s.isolated);
}

static void test_the_millisecond_counter_wrapping_around(void) {
    ioc_iso s;
    const uint32_t start = 0xFFFFFFFFu - 2000u;   /* o contador de 32 bits vira durante o isolamento */
    ioc_iso_init(&s, start);
    CHECK(ioc_iso_poll(&s, start + QUIET - 1, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_NONE);
    CHECK(ioc_iso_poll(&s, start + QUIET, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_ENTERED);
    uint32_t t = start + QUIET;
    uint32_t first = t;
    uint32_t returned_at = 0;
    for (; t < first + 10000; t += 100) {
        if ((t - first) % 500 == 0) ioc_iso_note_rx(&s, t);
        if (ioc_iso_poll(&s, t, TEC_PASSIVE, EFLG_TXEP, QUIET, RECOVER) == IOC_ISO_EXITED) {
            returned_at = t;
            break;
        }
    }
    CHECK(returned_at == first + RECOVER);
}

int main(void) {
    test_a_working_bus_never_isolates();
    test_silence_alone_is_a_stopped_network_not_isolation();
    test_failing_transmissions_alone_do_not_isolate_while_frames_arrive();
    test_silence_plus_unacknowledged_transmissions_isolates_at_the_boundary();
    test_each_failure_signal_counts();
    test_the_bus_must_stay_back_for_the_whole_recovery_time();
    test_it_can_isolate_again_after_returning();
    test_the_detection_can_be_turned_off();
    test_the_millisecond_counter_wrapping_around();
    if (failures) {
        printf("test_ioc_isolation: %d falha(s)\n", failures);
        return 1;
    }
    puts("test_ioc_isolation: ok");
    return 0;
}
