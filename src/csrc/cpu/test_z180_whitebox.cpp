// test_z180_whitebox.cpp -- tests of the MAME core that cpu.h cannot drive, with the core's state set directly.
//
// It includes z180_mame.cpp (so it is built alone, not linked with it).  Kept apart from test_z180_contract.c,
// which uses cpu.h only.
//   dma1_level   MAME's e0deaf3898b, the DMA1 hunk: /DREQ1's edge or level sense is DCNTL.DMS1 (the reverted hunk
//                read DIM1, the transfer direction).  Level sense with DIM1 = 1: a held /DREQ1 keeps transferring;
//                edge sense with DIM1 = 0: one transfer, then /DREQ1 is consumed.
#include <cstdio>
#include <cstring>
#include "z180_mame.cpp"

namespace {

uint8_t mem[65536];
uint8_t rd(void *, uint32_t a) { return mem[a & 0xffff]; }
void wr(void *, uint32_t a, uint8_t v) { mem[a & 0xffff] = v; }
uint8_t in(void *, uint16_t) { return 0x5a; }
void out(void *, uint16_t, uint8_t) {}

int failures;

void report(const char *name, bool ok, const char *detail)
{
    std::printf("%-4s %-13s %s\n", ok ? "ok" : "FAIL", name, detail);
    failures += !ok;
}

// DMA1 with 8 bytes to go, /DREQ1 asserted and held, over 4 steps of NOPs; returns the bytes transferred
int dma1_transfers(uint8_t dcntl)
{
    std::memset(mem, 0, sizeof mem);            // NOPs
    cpu_bus bus = {};
    bus.read = rd;
    bus.write = wr;
    bus.in = in;
    bus.out = out;
    z180 *c = z180_create(&bus, 6144000.0);
    z180_device &d = *c->dev;
    d.m_dcntl = dcntl;                          // no wait states; DMS1 / DIM1 as given
    d.m_dma_mar1.d = 0x1000;
    d.m_dma_iar1.d = 0x0080;                    // an external port
    d.m_dma_bcr[1].w = 8;
    d.m_dstat = Z180_DSTAT_DE1 | Z180_DSTAT_DME;
    d.execute_set_input(Z180_INPUT_LINE_DREQ1, ASSERT_LINE);
    for (int i = 0; i < 4; i++)
        z180_step(c);
    int n = 8 - d.m_dma_bcr[1].w;
    z180_destroy(c);
    return n;
}

}  // namespace

int main()
{
    char dsc[200];
    int level = dma1_transfers(Z180_DCNTL_DIM1);          // level sense, I/O -> memory
    int edge = dma1_transfers(Z180_DCNTL_DMS1);           // edge sense, memory -> I/O
    std::snprintf(dsc, sizeof dsc, "level sense (DIM1 = 1): %d transfers in 4 steps (want 4); edge sense (DIM1 = 0): "
                  "%d (want 1)", level, edge);
    report("dma1_level", level == 4 && edge == 1, dsc);
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
