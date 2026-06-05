#!/bin/bash

# Default settings
CORES=${1:-7}
DURATION=${2:-5}
INTERVAL_US=${3:-10} # ディレイ（マイクロ秒単位）。mempoolの急激な枯渇を防ぎ競合を維持するため、デフォルトを10に変更
USE_MP=${4:-false} # true に設定すると rte_ring_mp_enqueue_bulk を使用
BURST_SIZE=${5:-64} # コンパイル時のバーストサイズ

if [ "$CORES" -lt 3 ]; then
    echo "Error: This reproduction requires at least 3 cores (1 Consumer, 2 Producers)."
    exit 1
fi

# Build setup if needed
if [ ! -d "build" ]; then
    echo "Setting up meson build directory with use_mp=$USE_MP, burst_size=$BURST_SIZE..."
    meson setup build -Duse_mp=$USE_MP -Dburst_size=$BURST_SIZE
    if [ $? -ne 0 ]; then
        echo "Meson setup failed!"
        exit 1
    fi
else
    echo "Configuring meson build directory with use_mp=$USE_MP, burst_size=$BURST_SIZE..."
    meson configure build -Duse_mp=$USE_MP -Dburst_size=$BURST_SIZE
    if [ $? -ne 0 ]; then
        echo "Meson configure failed!"
        exit 1
    fi
fi

# Build the application
echo "Building DPDK application..."
ninja -C build

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

# Execution
echo "----------------------------------------"
echo "Starting dpdk-app (Ring Race Condition)"
echo "Cores: $CORES, Duration: ${DURATION}s, Producer Interval: ${INTERVAL_US}us"
echo "Build Option (use_mp): $USE_MP"
echo "Build Option (burst_size): $BURST_SIZE"
echo "----------------------------------------"

# EAL arguments for lcores (e.g., -l 0-2 for 3 cores)
LCORES="0-$(($CORES - 1))"

# --no-huge: Run without hugepages
# -m 128: Limit memory allocation
# --vdev: Create virtual devices for environments without physical NICs
./build/dpdk-app -l $LCORES --no-huge -m 1024 \
    --vdev=net_tap_vsc0 --vdev=net_tap_vsc1 \
    --vdev=net_tap_vsc2 --vdev=net_tap_vsc3 \
    --vdev=net_tap_vsc4 --vdev=net_tap_vsc5 \
    -- -d $DURATION -i $INTERVAL_US
