from std.memory import MutOpaquePointer


comptime SignalHandle = MutOpaquePointer[MutAnyOrigin]
comptime GetU32Fn = def(SignalHandle) thin abi("C") -> UInt32
comptime PutU32Fn = def(SignalHandle, UInt32) thin abi("C")
comptime ResolveSignalFn = def(UInt32) thin abi("C") -> SignalHandle
comptime RequestDone = UInt32(0)
comptime RequestRisingEdge = UInt32(1)
comptime SignalClk = UInt32(0)
comptime SignalRst = UInt32(1)
comptime SignalEn = UInt32(2)
comptime SignalCount = UInt32(3)


@fieldwise_init
struct Signal(TrivialRegisterPassable):
    var get_u32: GetU32Fn
    var put_u32: PutU32Fn
    var handle: SignalHandle
    var id: UInt32

    def get(self) -> UInt32:
        return self.get_u32(self.handle)

    def set(self, value: UInt32):
        self.put_u32(self.handle, value)


@fieldwise_init
struct RisingEdge(TrivialRegisterPassable):
    var handle: SignalHandle
    var signal_id: UInt32

    def __init__(out self, signal: Signal):
        self.handle = signal.handle
        self.signal_id = signal.id


@fieldwise_init
struct WaitRequest(TrivialRegisterPassable):
    var kind: UInt32
    var signal_id: UInt32
    var next_phase: UInt32

    def encode(self) -> UInt64:
        return (
            (UInt64(self.kind) << 56)
            | (UInt64(self.signal_id) << 32)
            | UInt64(self.next_phase)
        )


@fieldwise_init
struct Scheduler(TrivialRegisterPassable):
    var current_phase: UInt32

    def at(self, phase: UInt32) -> Bool:
        return self.current_phase == phase

    def wait(self, trigger: RisingEdge, next_phase: UInt32) -> UInt64:
        _ = trigger.handle
        return WaitRequest(
            RequestRisingEdge, trigger.signal_id, next_phase
        ).encode()

    def finish(self) -> UInt64:
        return WaitRequest(RequestDone, 0, self.current_phase).encode()
