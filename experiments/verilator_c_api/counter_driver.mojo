from std.ffi import OwnedDLHandle
from std.memory import MutOpaquePointer


comptime CounterHandle = MutOpaquePointer[MutAnyOrigin]


def main() raises:
    var lib = OwnedDLHandle("./build/libcounter.dylib")

    var create = lib.get_function[def() thin abi("C") -> CounterHandle](
        "counter_create"
    )
    var destroy = lib.get_function[def(CounterHandle) thin abi("C")](
        "counter_destroy"
    )
    var reset = lib.get_function[def(CounterHandle) thin abi("C")]("counter_reset")
    var tick = lib.get_function[def(CounterHandle, UInt8) thin abi("C")](
        "counter_tick"
    )
    var value = lib.get_function[def(CounterHandle) thin abi("C") -> UInt8](
        "counter_value"
    )

    var sim = create()
    reset(sim)
    print("after reset:", value(sim))

    for _ in range(5):
        tick(sim, 1)

    print("after 5 enabled ticks:", value(sim))

    tick(sim, 0)
    print("after disabled tick:", value(sim))
    destroy(sim)
