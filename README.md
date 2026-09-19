# gcra_ipc

Interprocess tick-less GCRA rate shaper in POSIX shared memory.

The whole queue schedule lives in a single shared word — the virtual queue
position, in nanoseconds. There are no timers, threads, locks or syscalls on
the hot path: `Reserve()` performs exactly one CAS over one 64-byte cache line.

Semantics is that of a **shaper**, not a policer: a charge always succeeds and
returns the **absolute deadline** (nanoseconds of `CLOCK_MONOTONIC`) until
which the caller must not perform the limited action. There is no rejection
and no way to fail. Since the deadline is absolute, the remaining wait is
always `deadline - now`; interruptions and re-entries in the caller's wait do
not accumulate drift. How to wait — spinning, sleeping, an event loop — is up
to the caller.

Several instances of the shaper, possibly in different processes, attached to
the same shm object share one queue. Slot order equals the order of winning
CAS operations; the queue is implicitly unbounded (everyone eventually gets a
slot). The state is crash-safe: `kill -9` of any participant neither corrupts
it nor requires recovery.

## Layout

The shared object is 128 bytes, pinned by the binary contract of the
`(magic, version)` pair (fourcc `"GCR1"`, version `1`):

```cpp
struct alignas(64) gcra_ipc_state {
    std::atomic<uint64_t> position_ns;       // virtual queue position, ns
    std::atomic<uint64_t> rate_per_second;   // stored rate, units/s; 0 = disabled
    std::atomic<uint64_t> burst_window_ns;   // burst credit window, ns; 0 = strict spacer
};

struct gcra_ipc_shm {
    std::atomic<uint32_t> magic;
    uint32_t version;
    alignas(64) gcra_ipc_state state;        // the single cache line mutated by Reserve()
};
```

The rate and the burst window are stored as given (full `uint64` range) with
no re-encoding: `rate_per_second == 0` disables the limit (fail-open),
`burst_window_ns == 0` is a strict spacer without burst credit. An
incompatible layout change must rotate both the magic and the shm object name.

## Algorithm

For a request with cost `units` at time `now`:

- slot (returned deadline) = `max(position_ns − burst_window_ns, now)`,
  guarded against underflow;
- increment = `ceil(units × 1e9 / rate_per_second)` — the only rounding of a
  request, always up, over-charge below one nanosecond at any rate;
- new position = `max(position_ns, now) + increment` — re-anchored at
  `max(position, now)`, so idle time never accumulates credit beyond the
  burst window.

After an idle period, a simultaneous series of requests gets immediate slots
while the total cost of the preceding requests is within the burst budget
`B = floor(burst_window_ns × rate_per_second / 1e9)`; with unit cost that is
exactly `B + 1` immediate requests.

The library is usable as a network throttler up to 100 Gbit/s and beyond:
the increment is a single integer division on the hot path, precise to one
nanosecond per request at any rate.

## Example

```cpp
#include <gcra_ipc/gcra_ipc.h>

// Attaches to an existing or creates a new shm object: 10 units/s, 200 ms burst.
auto shaper = gcra_ipc::shaper::Create("/gcra_ipc_example",
    /*ratePerSecond*/ 10, /*burstWindowNs*/ 200'000'000);

// Charge one unit; the returned value is the absolute moment (CLOCK_MONOTONIC ns)
// before which the limited action must not be performed.
uint64_t now = clock_monotonic_ns();
uint64_t deadline = shaper->Reserve(now, /*units*/ 1);
if (deadline > now) {
    // sleep / spin / schedule until `deadline`; the choice is yours
}
```

Multiple attachments to the same name share one queue:

```cpp
auto b = gcra_ipc::shaper::Open("/gcra_ipc_example");
```

`Create()`/`CreateOrThrow()` on an existing object reconfigure it in place:
position is preserved and previously issued deadlines stay valid. Repeated
`Reconfigure()` calls atomically overwrite the parameters.

## Public API

- `shaper::Open(shmName)` — attaches to an existing shm object; returns null
  with `errno` set (`ENOENT`, `EINVAL` for foreign magic, or a system error).
- `shaper::Create(shmName, ratePerSecond, burstWindowNs)` — attaches or
  creates and configures; returns null with `errno` set on failure.
- `shaper::CreateOrThrow(...)` — same, but throws `gcra_ipc_error` carrying
  the errno value.
- `shaper::Reconfigure(ratePerSecond, burstWindowNs)` — live reconfiguration,
  throws `gcra_ipc_error` (EINVAL) on a detached shaper.
- `shaper::Reserve(nowNs, units)` — always succeeds, returns the absolute
  deadline; `units == 0` is a no-op. The recommended working bound for
  `units` is `2^24` (kept as `gcra_ipc::detail::units_max`); the bound is a
  usage contract and is not checked.

Usage contract: callers must not perform the limited action before their
deadline — violating callers are not limited by the library.

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires a C++17 compiler and POSIX shared memory (`/dev/shm`). GoogleTest is
fetched automatically for the tests (pass `-DGCRA_IPC_BUILD_TESTS=OFF` to skip).

## Documentation

The normative specification — formulas, the golden schedule table, the
requirements/axioms/tests/justifications ledger — lives in
[docs/SPEC.md](docs/SPEC.md).

## License

[MIT](LICENSE)
