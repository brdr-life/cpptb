// cosim_glue.cc -- what cpptb supplies in place of upstream's cosim harness
//
// Ibex's own co-simulation flow is dv/verilator/simple_system_cosim. The
// checker there is pure SystemVerilog and is reused here unmodified; what is
// not reusable is simple_system_cosim.cc, because it is a subclass of
// SimpleSystem, the hand-written harness cpptb replaces:
//
//     class SimpleSystemCosim : public SimpleSystem { ... }
//
// It inherits the model pointer, the memory areas and the command-line handling
// from that harness, none of which exist here. So this file provides the two
// DPI entry points the checker calls, and nothing else.
//
//   create_cosim()      constructs the Spike instance, from parameters the
//                       checker reads off the elaborated design
//   get_spike_cosim()   hands back the handle
//
// The third thing upstream's version does is seed Spike's memory:
//
//     void CopyMemAreaToCosim(MemArea *area, uint32_t base_addr) {
//       auto mem_data = area->Read(0, area->GetSizeWords());
//       _cosim->backdoor_write_mem(base_addr, area->GetSizeBytes(), &mem_data[0]);
//     }
//
// That cannot happen at construction here. Upstream loads firmware before the
// simulation starts, so by the time create_cosim runs the memory is already
// populated; this port loads through cpptb's backdoor while the core is held in
// reset, which is later. cosim_seed_memory below is called by the testbench at
// that point instead, so the ordering difference is explicit rather than a
// latent bug.
//
// SPDX-License-Identifier: Apache-2.0

#include <svdpi.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "cosim.h"
#include "spike_cosim.h"

namespace {

// Owned here rather than by a harness object, because there is no harness
// object. The checker's `initial` block creates it once.
std::unique_ptr<SpikeCosim> g_cosim;

// Ibex's reset vector and the mtvec it initialises from boot_addr. Upstream
// passes the same two constants:
//
//     _cosim = std::make_unique<SpikeCosim>(GetIsaString(), 0x100080, 0x100001, ...)
//
// 0x100080 is boot_addr + 0x80, and 0x100001 is boot_addr with mtvec's vectored
// mode bit set, which is what rtl/ibex_cs_registers.sv hardwires.
constexpr uint32_t kStartPc = 0x0010'0080;
constexpr uint32_t kStartMtvec = 0x0010'0001;

// Upstream derives this from the elaborated parameters with GetIsaString(),
// reading them off the Verilated model directly. cpptb has no such pointer, and
// the string has to match the configuration the design was elaborated with or
// Spike and Ibex disagree about which instructions exist. It is passed in from
// the build instead, so the two cannot drift silently: see cosim.toml.
#ifndef CPPTB_COSIM_ISA
#error "CPPTB_COSIM_ISA must name the ISA the design was elaborated for"
#endif

// Passed as a bare token and stringified here, not as a quoted string on the
// command line. The define travels cpptb.toml -> Verilator -> make -> shell,
// and each hop strips a layer of quoting; escaping it through all four is
// possible and unreadable, and gets "error: 'rv32imc' was not declared in this
// scope" when it goes wrong. cpptb's own verilator_timing_main.cpp handles
// CPPTB_VERILATED_TOP the same way.
#define CPPTB_STRINGIFY_IMPL(value) #value
#define CPPTB_STRINGIFY(value) CPPTB_STRINGIFY_IMPL(value)

}  // namespace

extern "C" {

// The checker declares these as `bit [31:0]`, and a packed vector crosses DPI
// as a pointer to svBitVecVal, not as a uint32_t -- even at exactly 32 bits.
// Declaring them uint32_t compiles and links cleanly and then passes the low
// half of a pointer as the value, which surfaces as
//
//     error: bad number of pmp regions: '1373685940' from the dtb
//
// from inside Spike, a long way from the cause. `bit` on its own is the one
// case that really is a scalar (svBit), which is why the first two differ.
void create_cosim(svBit secure_ibex, svBit icache_en,
                  const svBitVecVal *pmp_num_regions,
                  const svBitVecVal *pmp_granularity,
                  const svBitVecVal *mhpm_counter_num,
                  const svBitVecVal *dm_start_addr,
                  const svBitVecVal *dm_end_addr) {
    if (g_cosim) return;  // the checker's initial block runs once, but be safe

    g_cosim = std::make_unique<SpikeCosim>(
        CPPTB_STRINGIFY(CPPTB_COSIM_ISA), kStartPc, kStartMtvec, "cosim.log",
        static_cast<bool>(secure_ibex), static_cast<bool>(icache_en),
        pmp_num_regions[0], pmp_granularity[0], mhpm_counter_num[0],
        dm_start_addr[0], dm_end_addr[0]);

    // The whole address space, sparsely populated, exactly as upstream does.
    // Simple System's peripherals live outside RAM and the programs touch them,
    // so a region covering only RAM would fault on the first character written.
    g_cosim->add_memory(0x0000'0000, 0xFFFF'0000);
}

// The static_cast is load-bearing and must not be simplified away.
//
// SpikeCosim inherits from two bases:
//
//     class SpikeCosim : public simif_t, public Cosim
//
// so the Cosim subobject does not start at the same address as the object. The
// DPI functions in cosim_dpi.cc all take a Cosim*, so handing them the
// unadjusted pointer makes every one of them read a simif_t through a Cosim
// vtable. It compiles, links, and runs -- until the first retired instruction,
// where a call to riscv_cosim_set_mip arrives inside mem_t::load_store with a
// null buffer and segfaults, roughly as far from the cause as a bug can get.
//
// Returning void* is what the DPI signature requires and is exactly why the
// compiler cannot catch this. Upstream writes the same cast for the same
// reason.
void *get_spike_cosim() { return static_cast<Cosim *>(g_cosim.get()); }

}  // extern "C"

// Called by the testbench once the program is in RTL memory, with the same
// bytes it deposited. Not a DPI function: it is ordinary C++ that the cpptb
// testbench links against.
void cosim_seed_memory(uint32_t base_addr, const uint8_t *data, size_t size) {
    if (!g_cosim) {
        std::fprintf(stderr,
                     "cosim_seed_memory: no cosim; the checker's initial block "
                     "should have created one before the first clock edge\n");
        return;
    }
    g_cosim->backdoor_write_mem(base_addr, size, data);
}
