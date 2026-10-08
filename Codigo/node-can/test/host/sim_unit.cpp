/*
 * Compila um arquivo do firmware dentro do namespace de uma instância.
 *   -DSIM_NS=<namespace> -DSIM_SOURCE='"../../src/<arquivo>"' -DIOT_NODE_ID=<n>
 */
#include "sim_prelude.h"

namespace SIM_NS {
#include "sim_stubs.h"
#include SIM_SOURCE
}  // namespace SIM_NS
