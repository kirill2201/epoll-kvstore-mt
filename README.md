# epoll-kvstore-mt

A multi-threaded version of [epoll-kvstore](https://github.com/kirill2201/epoll-kvstore): the same
event-driven TCP key-value store, scaled across CPU cores using
`SO_REUSEPORT` and per-worker `epoll` loops. Built to measure whether — and
by how much — threading helps an I/O-bound server, not to assume it does.

## Architecture: shared-nothing workers + one shared store

`N = hardware_concurrency()` worker threads are launched, each an independent
copy of the single-threaded server: its own listening socket, its own `epoll`
instance, its own clients and per-connection buffers. Incoming connections are
distributed across workers **by the kernel** via `SO_REUSEPORT` — no
user-space handoff, no shared connection state.

The only shared state is the key-value store:

```cpp
std::unordered_map<std::string, std::string> store;
std::mutex store_mutex;
```

Every access to `store` (`SET`/`GET`/`DEL`) is guarded by a `std::lock_guard`
holding `store_mutex`, and only for the map operation itself — parsing and
buffer handling stay outside the critical section to keep it minimal. Per-
connection `inbuf`/`outbuf` need no locking, since each connection lives
entirely within one worker.

This "shared-nothing except the store" design is the minimal possible surface
for data races — one mutex, one map.

## Correctness: verified under ThreadSanitizer

The concurrent paths were validated with `-fsanitize=thread`: with the mutex
in place, TSan is silent under parallel load; removing a single `lock_guard`
immediately triggers a reported `data race` inside `unordered_map`'s bucket
insertion. The lock is load-bearing, not decorative — and that was confirmed
empirically, not assumed.

```bash
# ASLR can conflict with TSan on recent kernels; run via setarch:
g++ -std=c++20 -Wall -Wextra -g -fsanitize=thread server_mt.cpp -o server_mt
setarch $(uname -m) -R ./server_mt
```

## Benchmark: 2a vs 2b under identical load

Same load generator (8 concurrent clients, synchronous round-trips) against
both servers, `-O2`, no sanitizers, median of several runs:

| Server                  | Aggregate throughput |
|-------------------------|----------------------|
| Single-threaded (2a)    | ~180,000 req/sec     |
| Multi-threaded (2b, 8 workers) | ~288,000 req/sec |

**Speedup ≈ 1.6×.**

## Why 1.6× and not 8×

The gain is real but far below the core count, and that is the expected
result, not a disappointment:

- **Mutex contention.** All workers serialize on a single `store_mutex`. The
  critical section is tiny (a hash-map op), but it is shared, so it caps
  scalability — Amdahl's law: throughput is bounded by the non-parallel
  fraction.
- **I/O-bound workload.** Time is dominated by `read`/`write` syscalls and
  context switches, not computation. Extra cores mostly parallelize *waiting
  on the kernel*, not work.
- **Co-located load generator.** The benchmark client runs 8 threads on the
  same 8-core machine, competing with the server for CPU. A separate client
  machine would raise the ceiling.

A more honest framing than "8× faster" would be: threading this server helps,
but the bottleneck is contention and I/O, not CPU.

## Future work

- **Shard the store.** Split into `S` maps each with its own mutex, keyed by
  `hash(key) % S`. Workers touching different shards no longer contend — this
  is the standard way to scale past single-mutex serialization.
- **`EPOLLOUT` backpressure** (inherited from 2a): re-arm for writability and
  flush the remainder when a `write` returns `EAGAIN`.
- Off-box load generation for a cleaner throughput ceiling.

## Build & run

```bash
# server (production build)
g++ -std=c++20 -O2 server_mt.cpp -o server_mt && ./server_mt

# thread-safety check
g++ -std=c++20 -Wall -Wextra -g -fsanitize=thread server_mt.cpp -o server_mt
setarch $(uname -m) -R ./server_mt

# parallel benchmark (run against 2a and 2b for comparison)
g++ -std=c++20 -O2 bench_mt.cpp -o bench_mt && ./bench_mt
```

## Notes

Same line protocol as 2a (`SET`/`GET`/`DEL`, `\n`-terminated, case-sensitive,
robust to malformed input). Startup failures in a worker call `std::exit` —
acceptable for a fatal bind/listen error, though a production server would
coordinate shutdown across threads more gracefully. In-memory only; the store
is not persisted.
