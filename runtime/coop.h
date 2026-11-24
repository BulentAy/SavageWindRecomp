// Co-op ghost: off unless SAVAGEWIND_COOP_TEST=local (single-instance diagnostic) or networking is configured.
#pragma once

#include <stdbool.h>

void coop_start(bool net);  // net: runtime/coop_net.c started
