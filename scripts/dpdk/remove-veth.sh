#!/usr/bin/env bash

set -euo pipefail

readonly PEER_IF="dw_peer"
readonly ENGINE_IF="dw_eng"
readonly OWNER_ALIAS="depthwire-managed-veth-v1"

interface_exists() {
    ip link show dev "$1" >/dev/null 2>&1
}

interface_is_owned() {
    local interface_name="$1"
    local alias_path="/sys/class/net/${interface_name}/ifalias"

    [ -r "${alias_path}" ] &&
        [ "$(tr -d '\n' < "${alias_path}")" = "${OWNER_ALIAS}" ]
}

pair_present=0

for interface_name in "${PEER_IF}" "${ENGINE_IF}"; do
    if interface_exists "${interface_name}"; then
        pair_present=1

        if ! interface_is_owned "${interface_name}"; then
            echo "Refusing to remove existing interface '${interface_name}'." >&2
            echo "It does not carry the DepthWire ownership marker '${OWNER_ALIAS}'." >&2
            exit 2
        fi
    fi
done

if [ "${pair_present}" -eq 0 ]; then
    echo "DepthWire veth pair is not present."
    exit 0
fi

echo "DepthWire veth removal requires elevated privileges."
sudo -v

if interface_exists "${PEER_IF}"; then
    sudo ip link delete dev "${PEER_IF}"
else
    sudo ip link delete dev "${ENGINE_IF}"
fi

echo "Removed owned DepthWire test pair: ${PEER_IF} <-> ${ENGINE_IF}"
