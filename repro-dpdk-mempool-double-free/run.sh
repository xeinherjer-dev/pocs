#!/bin/bash

# Default settings
CORES=${1:-2}
DURATION=${2:-5}
INTERVAL=${3:-200}

# Build the application
echo "Building DPDK application..."
ninja -C build

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

# Execution
echo "----------------------------------------"
echo "Starting dpdk-app"
echo "Cores: $CORES, Duration: ${DURATION}s, Interval: ${INTERVAL}ms"
echo "----------------------------------------"

# EAL arguments for lcores (e.g., -l 0-1 for 2 cores)
LCORES="0-$(($CORES - 1))"

# --no-huge: Run without hugepages (for unprivileged environments)
# -m 128: Limit memory allocation
# --vdev: Create virtual devices (TAP) for environments without physical NICs
./build/dpdk-app -l $LCORES --no-huge -m 128 \
    --vdev=net_tap_vsc0 --vdev=net_tap_vsc1 \
    --vdev=net_tap_vsc2 --vdev=net_tap_vsc3 \
    --vdev=net_tap_vsc4 --vdev=net_tap_vsc5 \
    -- -d $DURATION -i $INTERVAL
