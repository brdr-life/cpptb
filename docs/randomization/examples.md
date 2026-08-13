# Randomization examples

These examples compare the authored randomization portion of cpptb, Cocotb,
UVM, and pure-SystemVerilog testbenches. The repository's cpptb and pure-SV
benchmark versions use the same `xoshiro256ss-v1` stream, consume random words
in the same order, drive the same DUT transaction, and check the same response
and final checksum.

The Cocotb tabs are runnable authoring equivalents using Python's `random`
module. Cocotb 2.0 seeds that module per test from `COCOTB_RANDOM_SEED`, but its
generator is not the exact xoshiro performance peer. See Cocotb's official
[2.0 release notes](https://docs.cocotb.org/en/stable/release_notes.html) and
[runner reference](https://docs.cocotb.org/en/stable/library_reference.html#cocotb_tools.runner.Runner.test)
for current seed configuration.

UVM tabs appear on the constrained examples. The constraints themselves are
SystemVerilog; UVM adds a standard sequence-item, sequencer, and driver
lifecycle around them. These compact references follow the explicit
`start_item()`, `randomize()`, `finish_item()` flow described by the
[Accellera UVM 1.2 User's Guide](https://www.accellera.org/images/downloads/standards/uvm/uvm_users_guide_1.2.pdf).
They are not another transport or performance result. Verilator 5.050 continues
to describe class support as limited and may warn when a constraint form is
ignored, so the exact repository gate remains the cpptb/pure-SV pair; see the
[Verilator language guide](https://verilator.org/guide/latest/languages.html)
and [`CONSTRAINTIGN`](https://verilator.org/guide/latest/warnings.html#cmdoption-arg-CONSTRAINTIGN).

Common RNG initialization, clocking, reset, `transact(...)`, and result
reporting are omitted from both tabs. The commands below run the complete
sources, not the excerpts.

## Direct mixed stimulus

This workload creates a 32-bit payload from a full-width value, a weighted
mask, a 65-bit packed value, and a shuffled lane order. It is a good fit for
direct generation because the values do not have cross-field legality.

<div class="cpptb-code-tabs" data-tabs="3" data-tab-group="randomization-comparison" data-tab-label="Direct random stimulus implementation"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

```cpp
Task<void> random_sequence(Context& context, TestContext& test) {
    auto& random = test.random();
    constexpr std::array masks{
        weighted(0x0000'0000u, 1),
        weighted(0x0101'0101u, 2),
        weighted(0x1357'9bdfu, 3),
        weighted(0xa5a5'5a5au, 4),
    };

    for (uint32_t iteration = 0; iteration < kIterations; ++iteration) {
        uint32_t payload = random.randint<uint32_t>(
            0, std::numeric_limits<uint32_t>::max());
        payload ^= random.weighted_choice(masks);

        const Bits<65> wide = random.randbits<65>();
        payload ^= wide.word(0) ^ wide.word(1);
        if (wide.word(2) != 0) payload ^= 0x8000'0000u;

        std::array<uint32_t, 4> order{0, 1, 2, 3};
        random.shuffle(order);
        payload ^= order[0] | (order[1] << 4) |
                   (order[2] << 8) | (order[3] << 12);

        co_await transact(context, iteration, payload);
    }
}
```

<div class="cpptb-code-tab-label">Cocotb</div>

```python
import random

async def random_sequence(dut):
    masks = [0x0000_0000, 0x0101_0101, 0x1357_9BDF, 0xA5A5_5A5A]

    for iteration in range(ITERATIONS):
        payload = random.getrandbits(32)
        payload ^= random.choices(masks, weights=[1, 2, 3, 4], k=1)[0]

        wide = random.getrandbits(65)
        payload ^= wide & 0xFFFF_FFFF
        payload ^= (wide >> 32) & 0xFFFF_FFFF
        if (wide >> 64) & 1:
            payload ^= 0x8000_0000

        order = [0, 1, 2, 3]
        random.shuffle(order)
        payload ^= order[0] | (order[1] << 4)
        payload ^= (order[2] << 8) | (order[3] << 12)

        await transact(dut, iteration, payload)
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
function automatic logic [31:0] random_payload();
  logic [31:0] payload;
  logic [63:0] wide;
  logic [63:0] top;
  int unsigned order [0:3] = '{0, 1, 2, 3};

  payload = random_next_u64()[31:0];
  case (random_below(10))
    0:       payload ^= 32'h0000_0000;
    1, 2:    payload ^= 32'h0101_0101;
    3, 4, 5: payload ^= 32'h1357_9bdf;
    default: payload ^= 32'ha5a5_5a5a;
  endcase

  wide = random_next_u64();
  top = random_next_u64();
  payload ^= wide[31:0] ^ wide[63:32];
  if (top[0]) payload ^= 32'h8000_0000;

  for (int unsigned remaining = 4; remaining > 1; remaining--) begin
    int unsigned selected = random_below(remaining);
    int unsigned temporary = order[remaining - 1];
    order[remaining - 1] = order[selected];
    order[selected] = temporary;
  end
  return payload ^ order[0] ^ (order[1] << 4) ^
         (order[2] << 8) ^ (order[3] << 12);
endfunction

task automatic random_sequence();
  for (int unsigned iteration = 0; iteration < kIterations; iteration++)
    transact(iteration, random_payload(), 1'b0);
endtask
```

Run the exact pair:

```sh
make feature-test FEATURE=random_stimulus
make feature-benchmark FEATURE=random_stimulus
```

## Related packet fields

The packet has range, alignment, modulo, and cross-field rules. cpptb keeps
those rules with the transaction. The exact pure-SV peer performs the same
candidate generation and rejection explicitly because that is the portable
Verilator implementation being measured.

<div class="cpptb-code-tabs" data-tabs="4" data-tab-group="randomization-constrained-comparison" data-tab-label="Constrained packet implementation"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

```cpp
class Packet final : public Randomized {
  public:
    Rand<uint8_t> opcode{*this, "opcode"};
    Rand<uint16_t> length{*this, "length"};
    Rand<uint16_t> address{*this, "address"};
    Rand<uint8_t> tag{*this, "tag"};

    Packet() {
        constraint("supported opcode", opcode <= uint8_t{6});
        constraint("packet length",
                   length >= uint16_t{64} && length <= uint16_t{1500});
        constraint("address window",
                   address >= uint16_t{0x1000} &&
                   address <= uint16_t{0x1fff});
        constraint("word-sized packet", length % uint16_t{4} == 0);
        constraint("aligned address", address % uint16_t{4} == 0);
        constraint("short control packet",
                   opcode != uint8_t{6} || length <= uint16_t{256});
    }

    uint32_t payload() const {
        return (uint32_t{opcode.get()} << 29) ^
               (uint32_t{length.get()} << 16) ^
               (uint32_t{address.get()} << 1) ^ tag.get();
    }
};

Task<void> packet_sequence(Context& context, TestContext& test) {
    Packet packet;
    for (uint32_t iteration = 0; iteration < kIterations; ++iteration) {
        test.randomize(packet);
        co_await transact(context, iteration, packet.payload());
    }
}
```

<div class="cpptb-code-tab-label">Cocotb</div>

```python
import random
from dataclasses import dataclass

@dataclass(frozen=True)
class Packet:
    opcode: int
    length: int
    address: int
    tag: int

    def payload(self):
        return ((self.opcode << 29) ^ (self.length << 16) ^
                (self.address << 1) ^ self.tag) & 0xFFFF_FFFF

def random_packet():
    while True:
        packet = Packet(
            opcode=random.randrange(7),
            length=random.randrange(64, 1501),
            address=random.randrange(0x1000, 0x2000),
            tag=random.randrange(256),
        )
        if packet.length % 4 != 0 or packet.address % 4 != 0:
            continue
        if packet.opcode == 6 and packet.length > 256:
            continue
        return packet

async def packet_sequence(dut):
    for iteration in range(ITERATIONS):
        await transact(dut, iteration, random_packet().payload())
```

<div class="cpptb-code-tab-label">UVM</div>

```systemverilog
class packet_item extends uvm_sequence_item;
  `uvm_object_utils(packet_item)

  rand bit [2:0]  opcode;
  rand bit [15:0] length;
  rand bit [15:0] address;
  rand bit [7:0]  tag;

  constraint legal {
    opcode <= 6;
    length inside {[64:1500]};
    address inside {[16'h1000:16'h1fff]};
    length % 4 == 0;
    address % 4 == 0;
    opcode == 6 -> length <= 256;
  }

  function new(string name = "packet_item");
    super.new(name);
  endfunction
endclass

class packet_sequence extends uvm_sequence #(packet_item);
  `uvm_object_utils(packet_sequence)

  function new(string name = "packet_sequence");
    super.new(name);
  endfunction

  task body();
    repeat (ITERATIONS) begin
      packet_item item = packet_item::type_id::create("item");
      start_item(item);
      if (!item.randomize())
        `uvm_fatal("RAND", "packet_item randomization failed")
      finish_item(item); // the driver translates fields to DUT pins
    end
  endtask
endclass
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
function automatic logic [31:0] constrained_packet_payload();
  logic [7:0] opcode;
  logic [15:0] length;
  logic [15:0] address;
  logic [7:0] tag;

  forever begin
    opcode = random_below(7);
    length = 16'd64 + random_below(1437);
    address = 16'h1000 + random_below(4096);
    tag = random_below(256);

    if ((length % 4) != 0) continue;
    if ((address % 4) != 0) continue;
    if ((opcode == 6) && (length > 256)) continue;

    return ({24'b0, opcode} << 29) ^
           ({16'b0, length} << 16) ^
           ({16'b0, address} << 1) ^ {24'b0, tag};
  end
endfunction

task automatic packet_sequence();
  for (int unsigned iteration = 0; iteration < kIterations; iteration++)
    transact(iteration, constrained_packet_payload(), 1'b0);
endtask
```

Run the exact pair:

```sh
make feature-test FEATURE=constrained_packet
make feature-benchmark FEATURE=constrained_packet
```

## Selection policies and composite fields

This transaction combines an `inside()` set, weighted value/range policy, soft
default, disabled mode, nested object, fixed array, and 65-bit value. The tabs
show why the transaction model becomes more useful as policy and structure
accumulate.

<div class="cpptb-code-tabs" data-tabs="4" data-tab-group="randomization-constrained-comparison" data-tab-label="Constraint extension implementation"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

```cpp
class Header final : public Randomized {
  public:
    Rand<uint8_t> route{*this, "route"};

    explicit Header(Randomized& parent) : Randomized(parent, "header") {
        soft_constraint("default route", route == uint8_t{2});
    }
};

class ExtendedPacket final : public Randomized {
  public:
    Rand<uint8_t> opcode{*this, "opcode"};
    Rand<uint16_t> length{*this, "length"};
    Header header{*this};
    RandArray<uint8_t, 2> bytes{*this, "bytes"};
    RandBits<65> token{*this, "token"};

    ExtendedPacket() {
        constraint("selected opcode", inside(opcode, {1, 3, 5}));
        distribution(
            "packet length mix",
            dist(length, weighted(uint16_t{64}, 1),
                 weighted(range(uint16_t{128}, uint16_t{131}), 3)));
        constraint("distinct prefix", bytes[0] != bytes[1]);
        constraint("high token bit", token.word(2) == uint32_t{1});

        auto legacy = constraint("legacy opcode", opcode == uint8_t{7});
        legacy.disable();
    }
};

ExtendedPacket packet;
test.randomize(packet);
```

<div class="cpptb-code-tab-label">Cocotb</div>

```python
import random
from dataclasses import dataclass

@dataclass(frozen=True)
class ExtendedPacket:
    opcode: int
    length: int
    route: int
    bytes: tuple[int, int]
    token: int

def random_extended_packet():
    while True:
        byte0 = random.randrange(256)
        byte1 = random.randrange(256)
        if byte0 == byte1:
            continue

        return ExtendedPacket(
            opcode=random.choice([1, 3, 5]),
            length=random.choices([64, 128, 129, 130, 131],
                                  weights=[4, 3, 3, 3, 3], k=1)[0],
            route=2,
            bytes=(byte0, byte1),
            token=(1 << 64) | random.getrandbits(64),
        )

async def extended_packet_sequence(dut):
    for iteration in range(ITERATIONS):
        packet = random_extended_packet()
        await transact(dut, iteration, encode(packet))
```

<div class="cpptb-code-tab-label">UVM</div>

```systemverilog
class extended_packet_item extends uvm_sequence_item;
  `uvm_object_utils(extended_packet_item)

  rand bit [2:0]  opcode;
  rand bit [15:0] length;
  rand bit [7:0]  route;
  rand bit [7:0]  bytes[2];
  rand bit [64:0] token;

  constraint selected_opcode { opcode inside {1, 3, 5}; }
  constraint length_mix {
    length dist {16'd64 :/ 1, [16'd128:16'd131] :/ 3};
  }
  constraint default_route { soft route == 2; }
  constraint distinct_prefix { bytes[0] != bytes[1]; }
  constraint high_token_bit { token[64] == 1; }
  constraint legacy_opcode { opcode == 7; }

  function new(string name = "extended_packet_item");
    super.new(name);
    legacy_opcode.constraint_mode(0);
  endfunction
endclass

class extended_packet_sequence extends uvm_sequence #(extended_packet_item);
  `uvm_object_utils(extended_packet_sequence)

  function new(string name = "extended_packet_sequence");
    super.new(name);
  endfunction

  task body();
    extended_packet_item item =
        extended_packet_item::type_id::create("item");
    start_item(item);
    if (!item.randomize() with { route == 7; })
      `uvm_fatal("RAND", "extended packet randomization failed")
    finish_item(item);
  endtask
endclass
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
function automatic logic [31:0] extended_packet_payload();
  logic [7:0] opcode;
  logic [15:0] length;
  logic [7:0] route;
  logic [7:0] byte0;
  logic [7:0] byte1;
  logic [31:0] token0;
  logic [31:0] token1;
  logic token2;

  forever begin
    case (random_below(3))
      0: opcode = 1;
      1: opcode = 3;
      default: opcode = 5;
    endcase

    if (random_below(4) == 0)
      length = 16'd64;
    else
      length = 16'd128 + random_below(4);

    // Bound-one draws preserve the exact field-generation word stream while
    // the accepted soft or hard policy fixes the resulting value.
    route = 2 + random_below(1);
    byte0 = random_below(256);
    byte1 = random_below(256);
    token0 = random_below(64'h0000_0001_0000_0000);
    token1 = random_below(64'h0000_0001_0000_0000);
    token2 = 1 + random_below(1);
    if (byte0 == byte1) continue;

    return ({24'b0, opcode} << 29) ^
           ({16'b0, length} << 16) ^
           ({24'b0, route} << 24) ^
           ({24'b0, byte0} << 8) ^ {24'b0, byte1} ^
           token0 ^ token1 ^ ({31'b0, token2} << 31);
  end
endfunction

task automatic extended_packet_sequence();
  for (int unsigned iteration = 0; iteration < kIterations; iteration++)
    transact(iteration, extended_packet_payload(), 1'b0);
endtask
```

Run the exact pair:

```sh
make feature-test FEATURE=constraint_extensions
make feature-benchmark FEATURE=constraint_extensions
```

The complete implementations are in
`benchmarks/authoring_core/testbenches/cpp_dpi/testbench.cpp` and
`benchmarks/authoring_core/testbenches/systemverilog/authoring_core_sv_tb.sv`.
See [Performance](../performance.md#deterministic-random-stimulus) for measured
ratios and environment qualification.

## Related pages

- [Random value generation](value-generation.md) documents the direct
  `test.random()` API used in the mixed-stimulus example.
- [Constrained transactions](constrained-transactions.md) explains the
  `Randomized` class, fields, and constraints in the packet example.
- [Policies and composite fields](policies-and-composition.md) covers the
  membership, distribution, soft-default, handle, nested, array, and packed
  features in the extension example.
- [Solvers and diagnostics](solvers-and-diagnostics.md) describes the
  backends that solve these models and their failure diagnostics.
- [Randomization library reference](../library/randomization.md) lists the
  exact signatures used across every tab.
