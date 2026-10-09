/*
 * Tudo o que precisa existir no escopo global antes de o firmware ser
 * incluído dentro do namespace de uma instância: biblioteca padrão,
 * biblioteca compartilhada do projeto (C) e a interface com a bancada.
 * Um cabeçalho de sistema que faltasse aqui seria incluído dentro do
 * namespace e não compilaria.
 */
#ifndef SIM_PRELUDE_H
#define SIM_PRELUDE_H

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <vector>

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ioc_canbits.h"
#include "ioc_failover.h"
#include "ioc_link.h"
#include "ioc_sha256.h"
#include "ioc_wdata.h"

extern "C" {
#include "../../../node-wifi/edge_protocol_definitions.h"
}

#include "sim_api.h"

#endif
