/* cxxcheck.cpp -- the emulator headers must stay valid C++20 (the JUCE
 * plugin includes them). Compile-only; linked never. */
#include "emu/robovox_bus.h"

#include <cstddef>

static_assert(sizeof(mc6850_t) > 0, "acia");
static_assert(sizeof(robovox_state_t) > 0, "translator");
static_assert(sizeof(robovox_bus_t) > 0, "bus");
static_assert(RV_NVOICES_MAX == 4, "quad");

int robovox_cxxcheck_anchor;
