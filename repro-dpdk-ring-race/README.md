# DPDK Single Producer Ring Race Condition Demonstration

This sample application demonstrates the data race (Race Condition) that occurs when DPDK's Single-Producer (SP) ring enqueue API (`rte_ring_sp_enqueue_bulk()` / `rte_ring_sp_enqueue_burst()`) is concurrently called by multiple threads (workers) without proper serialization.

## Background & Mechanism

DPDK's `rte_ring` supports several modes, including Single-Producer (SP) / Multi-Producer (MP) and Single-Consumer (SC) / Multi-Consumer (MC).

- **Multi-Producer (MP)** mode: Multiple threads can safely enqueue concurrently because DPDK performs synchronization using atomic CAS (Compare-and-Swap) operations.
- **Single-Producer (SP)** mode: Designed under the assumption that "only a single thread will perform enqueuing." It stores pointers into the ring and advances indices (`prod.head` / `prod.tail`) without any locking, achieving high performance.

If multiple threads concurrently call the SP-mode enqueue API, the following race condition occurs:

1. **Index Corruption**: Multiple threads read the same `prod.head` and try to write data to the same slots.
2. **Data Loss & Duplication**: Pointers written by one thread are overwritten by another (leading to memory leaks). Meanwhile, the overwriting thread's pointers are duplicated or indexed incorrectly, leading to multiple reads of the same stale pointer.
3. **Double Free Trigger**: The consumer dequeues the same mbuf address multiple times. Freeing these duplicate mbufs using `rte_pktmbuf_free()` leads to a **Double Free** vulnerability.
4. **Mempool Contamination**: The double-free pollutes the mempool, adding the same object address to the free list multiple times. Subsequent allocations will assign the same address to different, concurrent allocation requests.

## Reproduction Steps

### Requirements

- A Linux environment with DPDK installed and detectable via `pkg-config` (for `libdpdk`).
- At least 3 available CPU cores (1 consumer core, 2 or more producer cores).

### Build & Run

Run the script `run.sh` to automatically set up the build environment using Meson/Ninja and launch the application.

```bash
./run.sh
```

You can customize the execution by passing arguments in the following order:

```bash
./run.sh [CORES] [DURATION] [INTERVAL_US] [USE_MP] [BURST_SIZE]
```

#### Arguments
- `CORES` (default: `7`): Total CPU cores to run (1 consumer, others are producers).
- `DURATION` (default: `5`): Program duration in seconds.
- `INTERVAL_US` (default: `10`): Delay between producer allocations (in microseconds). To prevent mempool starvation due to fast memory leaks, the default is set to 10us.
- `USE_MP` (default: `false`): Set to `true` to switch to `rte_ring_mp_enqueue_bulk()` (Multi-Producer) to resolve the race condition.
- `BURST_SIZE` (default: `64`): Burst size for enqueuing and dequeuing (configured at build-time).

### Examples

#### 1. SP Mode (Reproduce the Issue)
Run with the default SP mode:
```bash
./run.sh 7 5 10 false 64
```
**Expected Output:**
```console
----------------------------------------
Starting dpdk-app (Ring Race Condition)
Cores: 7, Duration: 5s, Producer Interval: 10us
Build Option (use_mp): false
Build Option (burst_size): 64
----------------------------------------
...
--- DPDK Ring Race Reproduction ---
Mode: Single-Producer Ring Enqueue (rte_ring_sp_enqueue_bulk) [WARNING: Race condition expected]
...
[Consumer] ★★★ DUPLICATE ADDRESS IN SAME BURST: 0x13fd3f040 ... ★★★
[Consumer] ★★★ DUPLICATE SEQUENCE DETECTED (STALE DEQUEUE): ... ★★★
...
[Final Check] avail=1, size=8192, total_duplicates=267245
  Verdict: ★ DUPLICATE DETECTED (Confirmed via Ring Race Condition) ★
```
Thousands of duplicate packet references will be detected due to the race condition on the single-producer enqueue pointers.

#### 2. MP Mode (Resolve the Issue)
Run with the safe Multi-Producer mode:
```bash
./run.sh 7 5 10 true 64
```
**Expected Output:**
```console
----------------------------------------
Starting dpdk-app (Ring Race Condition)
Cores: 7, Duration: 5s, Producer Interval: 10us
Build Option (use_mp): true
Build Option (burst_size): 64
----------------------------------------
...
--- DPDK Ring Race Reproduction ---
Mode: Multi-Producer Ring Enqueue (rte_ring_mp_enqueue_bulk)
...
[Final Check] avail=0, size=8192, total_duplicates=0
  Verdict: ✓ OK or minor leakage (No race detected)
```
No duplicate addresses are detected because the CAS synchronization in the MP enqueue API prevents the race condition.
