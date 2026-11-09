#pragma once

typedef enum { GPIO_NUM_1 = 1, GPIO_NUM_9 = 9, GPIO_NUM_10 = 10 } gpio_num_t;
typedef enum { GPIO_INTR_LOW_LEVEL = 4 } gpio_int_type_t;

inline int gpio_wakeup_enable(gpio_num_t, gpio_int_type_t) { return 0; }
inline int gpio_wakeup_disable(gpio_num_t) { return 0; }
