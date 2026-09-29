/* cxxcheck.cpp -- the emulator headers must stay valid C++20 (the JUCE
 * plugin includes them). Compile-only; linked never. */
#include "emu/ssinger_bus.h"

#include <cstddef>

static_assert(sizeof(mc6850_t) > 0, "acia");
static_assert(sizeof(ssinger_state_t) > 0, "translator");
static_assert(sizeof(ssinger_bus_t) > 0, "bus");
static_assert(SG_NVOICES_MAX == 4, "quad");

int ssinger_cxxcheck_anchor;
