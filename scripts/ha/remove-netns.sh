#!/usr/bin/env bash

set -Eeuo pipefail

readonly BRIDGE="br-dw"
readonly HOSTS=(feed primary standby)

sudo -v

for host in "${HOSTS[@]}"; do
    sudo ip netns delete "dw-${host}" 2>/dev/null || true
    sudo ip link delete dev "veth-${host}" 2>/dev/null || true
done

sudo ip link delete dev "${BRIDGE}" 2>/dev/null || true
echo "DepthWire HA namespaces removed."
