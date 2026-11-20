/* Medição da alimentação: divisor resistivo, INA219, média móvel, extremos, níveis, aviso e texto. */
#include <stdio.h>
#include <string.h>

#include "ioc_power.h"

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static ioc_pw_cfg vin_cfg(void) {
    ioc_pw_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.r1_ohm = 100000;
    cfg.r2_ohm = 100000;
    cfg.warn_mv = 4750;
    cfg.crit_mv = 4500;
    cfg.vin_enabled = 1;
    return cfg;
}

static void fill(ioc_pw *p, uint32_t pin_mv, unsigned n) {
    for (unsigned i = 0; i < n; ++i) ioc_pw_add_pin_mv(p, pin_mv);
}

static void test_the_divider_scales_the_pin_voltage_back_to_the_input(void) {
    CHECK(ioc_pw_divider_mv(2500, 100000, 100000) == 5000);       /* 5 V na entrada, 2,5 V no pino */
    CHECK(ioc_pw_divider_mv(1650, 200000, 100000) == 4950);       /* 3:1 */
    CHECK(ioc_pw_divider_mv(0, 100000, 100000) == 0);
    CHECK(ioc_pw_divider_mv(1000, 100000, 0) == 0);               /* R2 zero: configuração inválida, não divide por zero */
    CHECK(ioc_pw_divider_mv(3300, 1000000, 1000) == 65535);       /* limitado ao que cabe em 16 bits */
    CHECK(ioc_pw_divider_mv(3, 1, 3) == 4);                       /* 3 * 4 / 3 */
}

static void test_ina219_registers(void) {
    CHECK(ioc_pw_ina_shunt_uv(0x0000) == 0);
    CHECK(ioc_pw_ina_shunt_uv(0x0001) == 10);                     /* um passo: 10 uV */
    CHECK(ioc_pw_ina_shunt_uv(0x7FFF) == 327670);                 /* fundo de escala: 327,67 mV */
    CHECK(ioc_pw_ina_shunt_uv(0xFFFF) == -10);                    /* complemento de dois */
    CHECK(ioc_pw_ina_shunt_uv(0x8000) == -327680);
    /* 100 mΩ: 10 uV = 0,1 mA; 14,3 mV = 143 mA. */
    CHECK(ioc_pw_ina_current_ma(14300, 100) == 143);
    CHECK(ioc_pw_ina_current_ma(4, 100) == 0);                    /* abaixo de meio mA arredonda para zero */
    CHECK(ioc_pw_ina_current_ma(5, 100) == 0);                    /* 0,05 mA */
    CHECK(ioc_pw_ina_current_ma(50, 100) == 1);                   /* 0,5 mA arredonda para 1 */
    CHECK(ioc_pw_ina_current_ma(-14300, 100) == -143);            /* corrente invertida: sinal preservado */
    CHECK(ioc_pw_ina_current_ma(-50, 100) == -1);
    CHECK(ioc_pw_ina_current_ma(14300, 0) == 0);                  /* sem resistor configurado */
    CHECK(ioc_pw_ina_current_ma(10000, 10) == 1000);              /* resistor de 10 mΩ: 10 mV = 1 A */
}

static void test_there_is_no_average_before_the_window_is_full(void) {
    ioc_pw p;
    const ioc_pw_cfg cfg = vin_cfg();
    ioc_pw_init(&p, &cfg);
    CHECK(ioc_pw_level_now(&p) == IOC_PW_NO_READING);
    fill(&p, 2500, IOC_PW_WINDOW - 1);
    CHECK(ioc_pw_level_now(&p) == IOC_PW_NO_READING && !p.have_avg);
    char text[96];
    ioc_pw_format(&p, text, sizeof(text));
    CHECK(strcmp(text, " vin=SEM_LEITURA") == 0);
    ioc_pw_add_pin_mv(&p, 2500);
    CHECK(p.have_avg && p.avg_mv == 5000 && p.min_mv == 5000 && p.max_mv == 5000);
    CHECK(ioc_pw_level_now(&p) == IOC_PW_OK);
}

static void test_noise_in_single_samples_does_not_become_a_false_minimum(void) {
    ioc_pw p;
    const ioc_pw_cfg cfg = vin_cfg();
    ioc_pw_init(&p, &cfg);
    fill(&p, 2500, IOC_PW_WINDOW);                                 /* 5000 mV estáveis */
    ioc_pw_add_pin_mv(&p, 2300);                                   /* uma amostra com 400 mV a menos na entrada */
    /* A média cai 400/16 = 25 mV, não 400. */
    CHECK(p.avg_mv == 4975 && p.min_mv == 4975);
    fill(&p, 2500, IOC_PW_WINDOW);
    CHECK(p.avg_mv == 5000 && p.min_mv == 4975 && p.max_mv == 5000);   /* o mínimo fica registrado */
}

static void test_a_sustained_drop_is_seen_in_full_and_classified(void) {
    ioc_pw p;
    const ioc_pw_cfg cfg = vin_cfg();
    ioc_pw_init(&p, &cfg);
    fill(&p, 2500, IOC_PW_WINDOW);                                 /* 5,00 V */
    CHECK(ioc_pw_level_now(&p) == IOC_PW_OK);
    fill(&p, 2350, IOC_PW_WINDOW);                                 /* 4,70 V: abaixo do aviso */
    CHECK(p.avg_mv == 4700 && ioc_pw_level_now(&p) == IOC_PW_LOW);
    fill(&p, 2200, IOC_PW_WINDOW);                                 /* 4,40 V: abaixo do crítico */
    CHECK(p.avg_mv == 4400 && ioc_pw_level_now(&p) == IOC_PW_CRITICAL);
    CHECK(p.min_mv == 4400 && p.max_mv == 5000);
    /* Fronteiras exatas: o limite pertence ao nível de cima. */
    fill(&p, 2375, IOC_PW_WINDOW);                                 /* 4750 = aviso */
    CHECK(p.avg_mv == 4750 && ioc_pw_level_now(&p) == IOC_PW_OK);
    fill(&p, 2250, IOC_PW_WINDOW);                                 /* 4500 = crítico */
    CHECK(p.avg_mv == 4500 && ioc_pw_level_now(&p) == IOC_PW_LOW);
}

static void test_the_sampling_clock_is_every_10_ms_even_across_the_counter_wrap(void) {
    ioc_pw p;
    const ioc_pw_cfg cfg = vin_cfg();
    ioc_pw_init(&p, &cfg);
    CHECK(ioc_pw_due(&p, 1000) == 1);                              /* a primeira chamada colhe */
    CHECK(ioc_pw_due(&p, 1009) == 0);
    CHECK(ioc_pw_due(&p, 1010) == 1);
    CHECK(ioc_pw_due(&p, 1019) == 0 && ioc_pw_due(&p, 1020) == 1);
    ioc_pw_init(&p, &cfg);
    CHECK(ioc_pw_due(&p, 0xFFFFFFF9u) == 1);
    CHECK(ioc_pw_due(&p, 0xFFFFFFFFu) == 0);                       /* 6 ms depois */
    CHECK(ioc_pw_due(&p, 4) == 1);                                 /* 11 ms depois, já com o contador virado */
}

static void test_the_low_voltage_warning_is_rate_limited(void) {
    ioc_pw p;
    const ioc_pw_cfg cfg = vin_cfg();
    ioc_pw_init(&p, &cfg);
    fill(&p, 2500, IOC_PW_WINDOW);
    CHECK(ioc_pw_warning_due(&p, 1000) == 0);                      /* tensão boa: nada a avisar */
    fill(&p, 2300, IOC_PW_WINDOW);                                 /* 4,60 V */
    CHECK(ioc_pw_warning_due(&p, 2000) == 1);
    CHECK(ioc_pw_warning_due(&p, 2001) == 0 && ioc_pw_warning_due(&p, 11999) == 0);
    CHECK(ioc_pw_warning_due(&p, 12000) == 1);                     /* 10 s depois */
    fill(&p, 2500, IOC_PW_WINDOW);                                 /* a tensão voltou */
    CHECK(ioc_pw_warning_due(&p, 30000) == 0);
}

static void test_the_text_lists_only_what_is_enabled(void) {
    ioc_pw p;
    char text[160];
    ioc_pw_cfg cfg = vin_cfg();
    ioc_pw_init(&p, &cfg);
    fill(&p, 2500, IOC_PW_WINDOW);
    fill(&p, 2300, 1);
    ioc_pw_format(&p, text, sizeof(text));
    CHECK(strcmp(text, " vin_mv=4975 vin_min_mv=4975 vin_max_mv=5000 vin=OK") == 0);

    /* Só a corrente. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.shunt_mohm = 100;
    ioc_pw_init(&p, &cfg);
    ioc_pw_format(&p, text, sizeof(text));
    CHECK(strcmp(text, " corrente=SEM_LEITURA") == 0);
    ioc_pw_add_shunt_reg(&p, 1430);                                /* 14,3 mV */
    ioc_pw_add_shunt_reg(&p, 4120);                                /* 41,2 mV: o pico */
    ioc_pw_add_shunt_reg(&p, 900);
    ioc_pw_format(&p, text, sizeof(text));
    CHECK(strcmp(text, " corrente_ma=90 corrente_max_ma=412") == 0);
    ioc_pw_current_failed(&p);                                     /* o INA219 parou de responder */
    ioc_pw_format(&p, text, sizeof(text));
    CHECK(strcmp(text, " corrente=SEM_RESPOSTA") == 0);

    /* Nada ligado: texto vazio, e as leituras que chegam são ignoradas. */
    memset(&cfg, 0, sizeof(cfg));
    ioc_pw_init(&p, &cfg);
    ioc_pw_add_pin_mv(&p, 2500);
    ioc_pw_add_shunt_reg(&p, 1430);
    CHECK(ioc_pw_format(&p, text, sizeof(text)) == 0 && text[0] == '\0' && p.samples == 0);

    /* Os dois ligados. */
    cfg = vin_cfg();
    cfg.shunt_mohm = 100;
    ioc_pw_init(&p, &cfg);
    fill(&p, 2500, IOC_PW_WINDOW);
    ioc_pw_add_shunt_reg(&p, 1430);
    ioc_pw_format(&p, text, sizeof(text));
    CHECK(strcmp(text, " vin_mv=5000 vin_min_mv=5000 vin_max_mv=5000 vin=OK corrente_ma=143 corrente_max_ma=143") == 0);
}

static void test_the_text_never_overflows_a_small_buffer(void) {
    ioc_pw p;
    ioc_pw_cfg cfg = vin_cfg();
    cfg.shunt_mohm = 100;
    ioc_pw_init(&p, &cfg);
    fill(&p, 2500, IOC_PW_WINDOW);
    ioc_pw_add_shunt_reg(&p, 1430);
    for (size_t cap = 1; cap < 100; ++cap) {
        char text[100];
        memset(text, 'x', sizeof(text));
        const size_t len = ioc_pw_format(&p, text, cap);
        CHECK(len < cap && text[len] == '\0');
    }
    CHECK(ioc_pw_format(&p, NULL, 0) == 0);
}

static void test_clearing_keeps_the_configuration_and_drops_the_history(void) {
    ioc_pw p;
    ioc_pw_cfg cfg = vin_cfg();
    cfg.shunt_mohm = 100;
    ioc_pw_init(&p, &cfg);
    fill(&p, 2200, IOC_PW_WINDOW);
    ioc_pw_add_shunt_reg(&p, 4120);
    ioc_pw_clear(&p);
    CHECK(p.cfg.r1_ohm == 100000 && p.cfg.shunt_mohm == 100 && p.cfg.vin_enabled == 1);
    CHECK(!p.have_avg && p.samples == 0 && p.current_max_ma == 0 && !p.current_seen);
    fill(&p, 2500, IOC_PW_WINDOW);
    CHECK(p.min_mv == 5000);                                       /* o mínimo de 4,40 V ficou para trás */
}

int main(void) {
    test_the_divider_scales_the_pin_voltage_back_to_the_input();
    test_ina219_registers();
    test_there_is_no_average_before_the_window_is_full();
    test_noise_in_single_samples_does_not_become_a_false_minimum();
    test_a_sustained_drop_is_seen_in_full_and_classified();
    test_the_sampling_clock_is_every_10_ms_even_across_the_counter_wrap();
    test_the_low_voltage_warning_is_rate_limited();
    test_the_text_lists_only_what_is_enabled();
    test_the_text_never_overflows_a_small_buffer();
    test_clearing_keeps_the_configuration_and_drops_the_history();
    if (failures) {
        printf("test_ioc_power: %d falha(s)\n", failures);
        return 1;
    }
    puts("test_ioc_power: ok");
    return 0;
}
