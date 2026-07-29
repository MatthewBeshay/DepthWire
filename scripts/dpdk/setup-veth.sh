#!/usr/bin/env bash

set -Eeuo pipefail

readonly PEER_IF="dw_peer"
readonly ENGINE_IF="dw_eng"
readonly PEER_MAC="02:00:00:00:00:10"
readonly ENGINE_MAC="02:00:00:00:00:20"
readonly OWNER_ALIAS="depthwire-managed-veth-v1"

created_pair=0

interface_exists() {
    ip link show dev "$1" >/dev/null 2>&1
}

interface_is_owned() {
    local interface_name="$1"
    local alias_path="/sys/class/net/${interface_name}/ifalias"

    [ -r "${alias_path}" ] &&
        [ "$(tr -d '\n' < "${alias_path}")" = "${OWNER_ALIAS}" ]
}

rollback_partial_setup() {
    if [ "${created_pair}" -ne 1 ]; then
        return
    fi

    if interface_exists "${PEER_IF}"; then
        echo "Setup failed; removing the partially created DepthWire pair." >&2
        sudo ip link delete dev "${PEER_IF}" >/dev/null 2>&1 || true
    elif interface_exists "${ENGINE_IF}"; then
        echo "Setup failed; removing the partially created DepthWire pair." >&2
        sudo ip link delete dev "${ENGINE_IF}" >/dev/null 2>&1 || true
    fi
}

trap 'status=$?; rollback_partial_setup; exit "${status}"' ERR
trap 'rollback_partial_setup; exit 130' INT
trap 'rollback_partial_setup; exit 143' TERM

for interface_name in "${PEER_IF}" "${ENGINE_IF}"; do
    if interface_exists "${interface_name}" &&
       ! interface_is_owned "${interface_name}"; then
        echo "Refusing to replace existing interface '${interface_name}'." >&2
        echo "It does not carry the DepthWire ownership marker '${OWNER_ALIAS}'." >&2
        exit 2
    fi
done

echo "DepthWire veth setup requires elevated privileges for module and link changes."
sudo -v
sudo modprobe veth 2>/dev/null || true

if interface_exists "${PEER_IF}"; then
    sudo ip link delete dev "${PEER_IF}"
elif interface_exists "${ENGINE_IF}"; then
    sudo ip link delete dev "${ENGINE_IF}"
fi

sudo ip link add name "${PEER_IF}" type veth peer name "${ENGINE_IF}"
created_pair=1

sudo ip link set dev "${PEER_IF}" alias "${OWNER_ALIAS}"
sudo ip link set dev "${ENGINE_IF}" alias "${OWNER_ALIAS}"
sudo ip link set dev "${PEER_IF}" address "${PEER_MAC}"
sudo ip link set dev "${ENGINE_IF}" address "${ENGINE_MAC}"
sudo ip link set dev "${PEER_IF}" up
sudo ip link set dev "${ENGINE_IF}" up

echo "============================================================"
echo "DepthWire private AF_PACKET veth pair created"
echo "============================================================"
echo "Generator side: ${PEER_IF} ${PEER_MAC}"
echo "Engine side:    ${ENGINE_IF} ${ENGINE_MAC}"
echo
ip -br link show dev "${PEER_IF}"
ip -br link show dev "${ENGINE_IF}"

echo
echo "Ownership marker: ${OWNER_ALIAS}"
echo "Only a previously marked DepthWire pair may be replaced."

created_pair=0
trap - ERR INT TERM
