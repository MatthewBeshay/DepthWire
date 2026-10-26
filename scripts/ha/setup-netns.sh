#!/usr/bin/env bash
# Three network namespaces joined by one bridge stand in for the feed, primary
# and standby hosts of docs/architecture/failover.md. They share one kernel and
# one machine; results gathered on them say so.

set -Eeuo pipefail

readonly BRIDGE="br-dw"
readonly OWNER_ALIAS="depthwire-managed-ha-v1"
readonly HOSTS=(feed primary standby)

interface_exists() {
    ip link show dev "$1" >/dev/null 2>&1
}

if interface_exists "${BRIDGE}"; then
    if [ "$(tr -d '\n' < "/sys/class/net/${BRIDGE}/ifalias" 2>/dev/null)" != "${OWNER_ALIAS}" ]; then
        echo "Refusing to replace existing interface '${BRIDGE}'." >&2
        exit 2
    fi

    "$(dirname "$0")/remove-netns.sh"
fi

echo "DepthWire HA namespaces require elevated privileges for link and namespace changes."
sudo -v
sudo modprobe bridge 2>/dev/null || true
sudo modprobe veth 2>/dev/null || true

sudo ip link add name "${BRIDGE}" type bridge
sudo ip link set dev "${BRIDGE}" alias "${OWNER_ALIAS}"
# Without snooping the bridge floods the multicast feed to every port, which
# is what a group with both engine hosts joined needs.
sudo ip link set dev "${BRIDGE}" type bridge mcast_snooping 0
sudo ip link set dev "${BRIDGE}" up

address=1

for host in "${HOSTS[@]}"; do
    namespace="dw-${host}"
    sudo ip netns add "${namespace}"
    sudo ip link add name "veth-${host}" type veth peer name eth0 netns "${namespace}"
    sudo ip link set dev "veth-${host}" alias "${OWNER_ALIAS}"
    sudo ip link set dev "veth-${host}" master "${BRIDGE}" up
    sudo ip -n "${namespace}" addr add "10.77.0.${address}/24" dev eth0
    sudo ip -n "${namespace}" link set dev eth0 up
    sudo ip -n "${namespace}" link set dev lo up
    sudo ip -n "${namespace}" route add 224.0.0.0/4 dev eth0
    address=$((address + 1))
done

echo "============================================================"
echo "DepthWire HA namespaces created on ${BRIDGE}"
echo "============================================================"

for host in "${HOSTS[@]}"; do
    sudo ip -n "dw-${host}" -br addr show dev eth0
done
