from mojotb.runtime import (
    GetU32Fn,
    PutU32Fn,
    RisingEdge,
    ResolveSignalFn,
    Scheduler,
    Signal,
    SignalClk,
    SignalCount,
    SignalEn,
    SignalRst,
)


comptime ProcessResetDriver = UInt32(0)
comptime ProcessEnableDriver = UInt32(1)
comptime ProcessCountMonitor = UInt32(2)
comptime ProcessCount = UInt32(3)

comptime PhaseRun = UInt32(0)


@fieldwise_init
struct CounterDut(TrivialRegisterPassable):
    var clk: Signal
    var rst: Signal
    var en: Signal
    var count: Signal


def counter_dut(
    get_u32: GetU32Fn,
    put_u32: PutU32Fn,
    resolve_signal: ResolveSignalFn,
) -> CounterDut:
    return CounterDut(
        Signal(get_u32, put_u32, resolve_signal(SignalClk), SignalClk),
        Signal(get_u32, put_u32, resolve_signal(SignalRst), SignalRst),
        Signal(get_u32, put_u32, resolve_signal(SignalEn), SignalEn),
        Signal(get_u32, put_u32, resolve_signal(SignalCount), SignalCount),
    )


@fieldwise_init
struct ResetDriver:
    var dut: CounterDut
    var scheduler: Scheduler

    def start(self) -> UInt64:
        self.dut.rst.set(1)
        print("reset: rst=1, wait one clock")
        return self.scheduler.wait(RisingEdge(self.dut.clk), PhaseRun)

    def resume(self, time: UInt64) -> UInt64:
        self.dut.rst.set(0)
        print("reset: time=", time, " rst=0, done")
        return self.scheduler.finish()


@fieldwise_init
struct EnableDriver:
    var dut: CounterDut
    var scheduler: Scheduler

    def start(self) -> UInt64:
        self.dut.en.set(0)
        print("enable: en=0, waiting for reset release")
        return self.scheduler.wait(RisingEdge(self.dut.clk), PhaseRun)

    def resume(self, time: UInt64) -> UInt64:
        var rst_value = self.dut.rst.get()
        var count_value = self.dut.count.get()

        if rst_value != 0:
            self.dut.en.set(0)
            print("enable: time=", time, " rst=1, keep en=0")
            return self.scheduler.wait(RisingEdge(self.dut.clk), PhaseRun)

        if count_value < 5:
            self.dut.en.set(1)
            print("enable: time=", time, " count=", count_value, " drive en=1")
            return self.scheduler.wait(RisingEdge(self.dut.clk), PhaseRun)

        self.dut.en.set(0)
        print(
            "enable: time=", time, " count=", count_value, " drive en=0, done"
        )
        return self.scheduler.finish()


@fieldwise_init
struct CountMonitor:
    var dut: CounterDut
    var scheduler: Scheduler

    def start(self) -> UInt64:
        print("monitor: waiting for count activity")
        return self.scheduler.wait(RisingEdge(self.dut.clk), PhaseRun)

    def resume(self, time: UInt64) -> UInt64:
        var count_value = self.dut.count.get()
        print("monitor: time=", time, " count=", count_value)

        if count_value >= 5:
            print("monitor: observed target count, done")
            return self.scheduler.finish()

        return self.scheduler.wait(RisingEdge(self.dut.clk), PhaseRun)


@fieldwise_init
struct CounterTestbench:
    var dut: CounterDut

    def finish(self) -> Int32:
        var final_count = self.dut.count.get()
        if final_count == 5:
            print("tb: PASS final_count=", final_count)
            return 0

        print("tb: FAIL final_count=", final_count)
        return 1


@export
def mojotb_process_count() abi("C") -> UInt32:
    return ProcessCount


@export
def mojotb_on_setup(
    get_u32: GetU32Fn,
    put_u32: PutU32Fn,
    resolve_signal: ResolveSignalFn,
) abi("C"):
    var dut = counter_dut(get_u32, put_u32, resolve_signal)
    dut.clk.set(0)
    dut.rst.set(1)
    dut.en.set(0)
    print("tb: setup clk=0 rst=1 en=0")


@export
def mojotb_on_process_start(
    get_u32: GetU32Fn,
    put_u32: PutU32Fn,
    resolve_signal: ResolveSignalFn,
    process_id: UInt32,
) abi("C") -> UInt64:
    var dut = counter_dut(get_u32, put_u32, resolve_signal)
    var scheduler = Scheduler(PhaseRun)

    if process_id == ProcessResetDriver:
        return ResetDriver(dut, scheduler).start()

    if process_id == ProcessEnableDriver:
        return EnableDriver(dut, scheduler).start()

    if process_id == ProcessCountMonitor:
        return CountMonitor(dut, scheduler).start()

    print("tb: unknown process id=", process_id)
    return scheduler.finish()


@export
def mojotb_on_process_resume(
    get_u32: GetU32Fn,
    put_u32: PutU32Fn,
    resolve_signal: ResolveSignalFn,
    process_id: UInt32,
    time: UInt64,
    phase: UInt32,
) abi("C") -> UInt64:
    var dut = counter_dut(get_u32, put_u32, resolve_signal)
    var scheduler = Scheduler(phase)

    if process_id == ProcessResetDriver:
        return ResetDriver(dut, scheduler).resume(time)

    if process_id == ProcessEnableDriver:
        return EnableDriver(dut, scheduler).resume(time)

    if process_id == ProcessCountMonitor:
        return CountMonitor(dut, scheduler).resume(time)

    print("tb: unknown process id=", process_id)
    return scheduler.finish()


@export
def mojotb_on_done(
    get_u32: GetU32Fn,
    put_u32: PutU32Fn,
    resolve_signal: ResolveSignalFn,
) abi("C") -> Int32:
    var tb = CounterTestbench(counter_dut(get_u32, put_u32, resolve_signal))
    return tb.finish()
