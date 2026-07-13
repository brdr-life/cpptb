#include <cstdint>

#include "Vcounter.h"
#include "verilated.h"

double sc_time_stamp() { return 0.0; }

struct CounterSim {
    VerilatedContext* context;
    Vcounter* top;
};

extern "C" CounterSim* counter_create() {
    auto* context = new VerilatedContext;
    auto* top = new Vcounter{context};
    top->clk = 0;
    top->rst = 0;
    top->en = 0;
    top->eval();
    return new CounterSim{context, top};
}

extern "C" void counter_destroy(CounterSim* sim) {
    if (!sim) return;
    sim->top->final();
    delete sim->top;
    delete sim->context;
    delete sim;
}

extern "C" void counter_reset(CounterSim* sim) {
    sim->top->rst = 1;
    sim->top->en = 0;
    sim->top->clk = 0;
    sim->top->eval();
    sim->top->clk = 1;
    sim->top->eval();
    sim->top->clk = 0;
    sim->top->eval();
    sim->top->rst = 0;
}

extern "C" void counter_tick(CounterSim* sim, uint8_t en) {
    sim->top->en = en;
    sim->top->clk = 0;
    sim->top->eval();
    sim->top->clk = 1;
    sim->top->eval();
    sim->top->clk = 0;
    sim->top->eval();
    sim->context->timeInc(1);
}

extern "C" uint8_t counter_value(CounterSim* sim) {
    return sim->top->count;
}
