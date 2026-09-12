#!/bin/bash
# Twitchy CPU load generator - rapidly alternating high/low utilization

# Configuration
DURATION=${1:-60}  # Total runtime in seconds
BURST_S=${2:-0.2} # Burst duration in milliseconds
PAUSE_MS=${3:-300} # Pause duration in milliseconds
NCPUS=${4:-$(nproc)} # Number of CPUs to stress

echo "Generating twitchy load for ${DURATION}s"
echo "Burst: ${BURST_MS}ms, Pause: ${PAUSE_MS}ms, CPUs: ${NCPUS}"

END=$((SECONDS + DURATION))

while [ $SECONDS -lt $END ]; do
    # High load burst - spawn CPU-intensive background jobs
    for i in $(seq 1 $NCPUS); do
        # CPU-intensive calculation in background
        timeout ${BURST_S}s bash -c 'while :; do :; done' &
    done

    # Wait for burst to complete
    sleep $(echo "scale=3; $BURST_S" | bc)

    # Kill any remaining processes
    pkill -P $$

    # Low load pause
    sleep $(echo "scale=3; $PAUSE_MS / 1000" | bc)
done

# Cleanup
pkill -P $$
echo "Done"
