# DPDK Double-Free Reproduction and Detection

This project demonstrates the consequences of **mbuf double-free** vulnerabilities in DPDK applications and provides a method for detecting mempool contamination.

## Overview

This program simulates a Producer/Consumer multi-core scenario:
1. **Producer Core**: Periodically allocates mbufs and intentionally performs a **double-free** on a "victim" buffer.
2. **Detection (Alloc)**: Detects if the same memory address is assigned to multiple mbufs during a single `alloc_bulk` call.
3. **Consumer Core**: Receives mbufs via a ring, detects duplicate addresses in the batch, and frees them.
4. **Pool Monitoring**: Uses `rte_mempool_avail_count()` to monitor if the pool contains more mbufs than its original size, indicating contamination.

## Prerequisites

- DPDK 20.11 or later
- Meson 0.49 or later
- Ninja
- GCC or Clang
- `libnuma-dev`, `pkg-config`

## Build Instructions

```bash
# Setup build directory
meson setup build

# Compile
ninja -C build
```

## Running the Application

Use the provided `run.sh` script to run the application in a virtualized/unprivileged environment (no hugepages, virtual TAP devices).

```bash
# Usage: ./run.sh [CORES] [DURATION_SEC] [INTERVAL_MS]
./run.sh 2 10 100
```

### Key EAL Parameters Used in `run.sh`
- `--no-huge`: Run without hugepages (using standard memory).
- `-m 128`: Limit memory allocation to 128MB.
- `--vdev=net_tap_vscX`: Create virtual TAP devices to allow the EAL to initialize without physical NICs.

## Mechanism of Detection

### 1. Duplicate Address Detection
When a double-free occurs, the same pointer is added to the mempool's stack twice. Consequently, subsequent `rte_pktmbuf_alloc_bulk()` calls may return the same address for multiple mbuf pointers.
```c
if (bufs[i] == bufs[j]) {
    printf("Duplicate address detected: %p\n", bufs[i]);
}
```

### 2. Mempool Contamination Monitoring
`rte_mempool_avail_count()` returns the number of available entries in the pool (including caches). If a double-free occurs, the `avail_count` will exceed the total `size` of the pool.
```c
unsigned int avail = rte_mempool_avail_count(mp);
if (avail > pool_size) {
    printf("!!! POOL CONTAMINATED !!!\n");
}
```

## Disclaimer
This is a debugging and educational tool. **Never** use `MEMPOOL_CACHE_SIZE=0` in a production environment as it severely degrades performance.

## License
[BSD-3-Clause](https://opensource.org/licenses/BSD-3-Clause)
