# Scheduling

cpptb maps coroutine suspension points to simulator events. Ordinary C++ runs
inside the current DPI callback until the coroutine reaches `co_await`.

## Time and edges

- `co_await RisingEdge{signal}` resumes on a low-to-high transition.
- `co_await FallingEdge{signal}` resumes on a high-to-low transition.
- `co_await Edge{signal}` resumes on either transition.
- `co_await clock_cycles(clock, count)` counts rising edges of the selected
  clock. It is not tied to a primary or generated clock.
- `co_await Delay{10_ns}` resumes after an absolute simulator-time delay and
  works in designs with no clocks.

An edge wait resumes in the simulator callback associated with that edge. Use
an explicit delay such as `Delay{1_ps}` when the testbench intends to observe
logic after a later simulator time slot. Signal writes and backdoor operations
never add that delay automatically.

## Composition

- `Join{a(), b()}` waits until every child task completes.
- `First{RisingEdge{irq}, Delay{100_ns}}` returns the winning trigger index.
- `with_timeout(operation(), 100_ns)` returns a checked timeout result and
  cancels the losing task recursively.
- `Event`, `Channel<T>`, and `Process` provide notification, communication, and
  process lifecycle control without exposing scheduler stepping calls.

Waiters sharing an edge or deadline resume in registration order. Equal-time
events from different simulator processes retain simulator process ordering;
testbenches should not infer hardware priority from that ordering.
