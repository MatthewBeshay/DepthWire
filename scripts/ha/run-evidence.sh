#!/usr/bin/env bash
# Collects the failover and gap-fill measurements of
# docs/architecture/failover.md on the namespace topology that
# scripts/ha/setup-netns.sh creates (three namespaces on one machine, one
# kernel). Every run's reports are kept, and a run that fails is recorded in
# failures.txt rather than dropped.
#
#   failover-default  RUNS runs, heartbeat 5 ms, 4 missed (20 ms timeout)
#   failover-moderate RUNS runs, heartbeat 2 ms, 4 missed (8 ms timeout)
#   failover-tight    RUNS runs, heartbeat 1 ms, 3 missed (3 ms timeout)
#   gap-fill          LOSS_RUNS runs at each netem loss rate on the primary's
#                     link, once on feed datagrams only and once on all
#                     traffic (so the TCP gap-fill channel is lossy too)
#
# Usage:
#   scripts/ha/run-evidence.sh [output-directory]
#   scripts/ha/run-evidence.sh finalise <output-directory>
#
# The collect mode writes raw reports, every exact command (commands.log),
# environment-raw.txt and summary-raw.txt. The operator then authors
# environment.md and summary.md and runs the finalise mode, which writes the
# checksum manifest.
set -euo pipefail

cd "$(dirname "$0")/../.."

if [[ "${1:-}" == "finalise" ]]; then
    BUNDLE="${2:?usage: run-evidence.sh finalise <output-directory>}"

    for required in summary.md environment.md; do
        if [[ ! -f "${BUNDLE}/${required}" ]]; then
            echo "error: ${BUNDLE}/${required} is missing" >&2
            exit 1
        fi
    done

    (
        cd "${BUNDLE}"
        find . -type f ! -name sha256-manifest.txt -print0 | sort -z | xargs -0 sha256sum
    ) >"${BUNDLE}/sha256-manifest.txt"

    echo "finalised manifest written to ${BUNDLE}/sha256-manifest.txt"
    exit 0
fi

readonly HA=build/core-release/apps/depthwire-ha
readonly REPLAY=build/core-release/apps/depthwire-replay
readonly TRACE=tests/fixtures/trace/load_uniform_100k_v1.dwtrace
readonly RUNS="${DEPTHWIRE_HA_RUNS:-20}"
readonly LOSS_RUNS="${DEPTHWIRE_HA_LOSS_RUNS:-3}"
readonly RATE=100000
readonly DIE_AFTER=40000
readonly GROUP=239.77.0.1:47001
readonly RETRANSMIT=10.77.0.1:47002
readonly REPLICATION=10.77.0.3:47003
readonly HEARTBEAT=10.77.0.3:47004

OUTPUT_DIRECTORY="${1:-results/failover/raw-$(date +%Y%m%d-%H%M%S)}"

for binary in "${HA}" "${REPLAY}"; do
    if [[ ! -x "${binary}" ]]; then
        echo "error: ${binary} not built (configure and build the core-release preset)" >&2
        exit 1
    fi
done

for namespace in dw-feed dw-primary dw-standby; do
    if ! ip netns list | grep -q "^${namespace}\b"; then
        echo "error: namespace ${namespace} missing; run scripts/ha/setup-netns.sh" >&2
        exit 1
    fi
done

if [[ -e "${OUTPUT_DIRECTORY}" ]]; then
    echo "error: ${OUTPUT_DIRECTORY} already exists" >&2
    exit 1
fi

dirty="$(git status --porcelain --untracked-files=normal)"

if [[ -n "${dirty}" ]]; then
    echo "error: the working tree is not clean; evidence must name an exact revision" >&2
    printf '%s\n' "${dirty}" >&2
    exit 1
fi

HEAD_REVISION="$(git rev-parse --verify HEAD)"
VERSION_LINE="$("${HA}" --version)"

if [[ "${VERSION_LINE}" != *"(revision ${HEAD_REVISION})" ]]; then
    echo "error: ${HA} was not built from ${HEAD_REVISION}: ${VERSION_LINE}" >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIRECTORY}"
COMMANDS_LOG="${OUTPUT_DIRECTORY}/commands.log"
FAILURES="${OUTPUT_DIRECTORY}/failures.txt"
: >"${COMMANDS_LOG}"
: >"${FAILURES}"
sudo -n true 2>/dev/null || sudo -v

{
    echo "== date";       date -u +"%Y-%m-%dT%H:%M:%SZ"
    echo "== revision";   echo "${HEAD_REVISION}"
    echo "== binary";     echo "${VERSION_LINE}"
    echo "== uname";      uname -a
    echo "== cpu";        lscpu
    echo "== nproc";      nproc
    echo "== memory";     grep -E "MemTotal|SwapTotal" /proc/meminfo
    echo "== gcc";        gcc --version | head -1
    echo "== cmake";      cmake --version | head -1
    echo "== rmem_max";   sysctl net.core.rmem_max
    echo "== tc";         tc -V
    echo "== os-release"; cat /etc/os-release
    echo "== topology"
    ip -br link show dev br-dw

    for namespace in dw-feed dw-primary dw-standby; do
        echo "-- ${namespace}"
        sudo ip -n "${namespace}" -br addr show
    done
} >"${OUTPUT_DIRECTORY}/environment-raw.txt" 2>&1

log_command() {
    printf '%s:' "$1" >>"${COMMANDS_LOG}"
    shift
    printf ' %q' "$@" >>"${COMMANDS_LOG}"
    printf '\n' >>"${COMMANDS_LOG}"
}

# in_namespace <host> <label> <command...>
in_namespace() {
    local host="$1" label="$2"
    shift 2
    log_command "${label}" sudo ip netns exec "dw-${host}" "$@"
    sudo ip netns exec "dw-${host}" "$@"
}

"${REPLAY}" run "${TRACE}" --mode=conformance >"${OUTPUT_DIRECTORY}/conformance.txt"
log_command "conformance" "${REPLAY}" run "${TRACE}" --mode=conformance
EXPECTED_DIGEST="$(sed -n 's/^digest-sha256: //p' "${OUTPUT_DIRECTORY}/conformance.txt")"

run_failover() {
    local label="$1" heartbeat_ms="$2" missed="$3" run="$4"
    local directory="${OUTPUT_DIRECTORY}/${label}"
    local prefix="${directory}/run$(printf '%02d' "${run}")"
    mkdir -p "${directory}"
    echo "== ${label} run ${run}"

    in_namespace standby "${label}-${run}-standby" "${HA}" standby "--trace=${TRACE}" \
        "--feed=${GROUP}" --interface=10.77.0.3 "--retransmit=${RETRANSMIT}" \
        "--replication-listen=${REPLICATION}" "--heartbeat-listen=${HEARTBEAT}" \
        "--heartbeat-ms=${heartbeat_ms}" "--missed=${missed}" "--report=${prefix}-standby.txt" &
    local standby_pid=$!
    in_namespace feed "${label}-${run}-feed" "${HA}" feed "--trace=${TRACE}" \
        "--to=${GROUP}" --interface=10.77.0.1 "--retransmit-listen=${RETRANSMIT}" \
        "--rate=${RATE}" --start-delay-ms=500 --linger-ms=1500 "--report=${prefix}-feed.txt" &
    local feed_pid=$!
    local primary_status=0 standby_status=0 feed_status=0
    in_namespace primary "${label}-${run}-primary" "${HA}" primary "--trace=${TRACE}" \
        "--feed=${GROUP}" --interface=10.77.0.2 "--retransmit=${RETRANSMIT}" \
        "--standby=${REPLICATION}" "--heartbeat-to=${HEARTBEAT}" "--heartbeat-ms=${heartbeat_ms}" \
        "--die-after=${DIE_AFTER}" "--report=${prefix}-primary.txt" || primary_status=$?
    wait "${standby_pid}" || standby_status=$?
    wait "${feed_pid}" || feed_status=$?

    if (( primary_status != 0 || standby_status != 0 || feed_status != 0 )); then
        echo "${label} run ${run}: exit codes primary=${primary_status} standby=${standby_status} feed=${feed_status}" \
            >>"${FAILURES}"
    fi
}

# set_loss <scope> <percent>: netem on the bridge side of the primary's link,
# which is the primary's ingress
set_loss() {
    local scope="$1" percent="$2"
    sudo tc qdisc del dev veth-primary root 2>/dev/null || true

    if [[ "${scope}" == "all" ]]; then
        log_command "netem" sudo tc qdisc add dev veth-primary root netem loss "${percent}%"
        sudo tc qdisc add dev veth-primary root netem loss "${percent}%"
    else
        log_command "netem" sudo tc qdisc add dev veth-primary root handle 1: prio
        sudo tc qdisc add dev veth-primary root handle 1: prio
        log_command "netem" sudo tc qdisc add dev veth-primary parent 1:3 handle 30: netem loss "${percent}%"
        sudo tc qdisc add dev veth-primary parent 1:3 handle 30: netem loss "${percent}%"
        log_command "netem" sudo tc filter add dev veth-primary protocol ip parent 1:0 prio 3 u32 \
            match ip protocol 17 0xff flowid 1:3
        sudo tc filter add dev veth-primary protocol ip parent 1:0 prio 3 u32 \
            match ip protocol 17 0xff flowid 1:3
    fi
}

run_gap_fill() {
    local scope="$1" percent="$2" run="$3"
    local label="gap-fill-${scope}-${percent}"
    local directory="${OUTPUT_DIRECTORY}/${label}"
    local prefix="${directory}/run$(printf '%02d' "${run}")"
    mkdir -p "${directory}"
    echo "== ${label} run ${run}"

    set_loss "${scope}" "${percent}"
    in_namespace feed "${label}-${run}-feed" "${HA}" feed "--trace=${TRACE}" \
        "--to=${GROUP}" --interface=10.77.0.1 "--retransmit-listen=${RETRANSMIT}" \
        "--rate=${RATE}" --start-delay-ms=500 --linger-ms=1500 "--report=${prefix}-feed.txt" &
    local feed_pid=$!
    local primary_status=0 feed_status=0
    in_namespace primary "${label}-${run}-primary" "${HA}" primary "--trace=${TRACE}" \
        "--feed=${GROUP}" --interface=10.77.0.2 "--retransmit=${RETRANSMIT}" \
        "--report=${prefix}-primary.txt" || primary_status=$?
    wait "${feed_pid}" || feed_status=$?
    sudo tc -s qdisc show dev veth-primary >"${prefix}-netem.txt"
    sudo tc qdisc del dev veth-primary root

    if (( primary_status != 0 || feed_status != 0 )); then
        echo "${label} run ${run}: exit codes primary=${primary_status} feed=${feed_status}" >>"${FAILURES}"
    fi
}

for run in $(seq 1 "${RUNS}"); do
    run_failover failover-default 5 4 "${run}"
done

for run in $(seq 1 "${RUNS}"); do
    run_failover failover-moderate 2 4 "${run}"
done

for run in $(seq 1 "${RUNS}"); do
    run_failover failover-tight 1 3 "${run}"
done

for scope in feed all; do
    for percent in 0.1 1 5; do
        for run in $(seq 1 "${LOSS_RUNS}"); do
            run_gap_fill "${scope}" "${percent}" "${run}"
        done
    done
done

sudo chown -R "$(id -u):$(id -g)" "${OUTPUT_DIRECTORY}"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "${SCRATCH}"' EXIT

# Nearest-rank percentiles, ceil(q * N), over one number per line on stdin.
percentiles() {
    sort -n | awk '{ v[NR] = $1 } END {
        if (NR == 0) { print "n=0"; exit }
        split("0.50 0.99 1.00", q, " ")
        out = "n=" NR
        for (i = 1; i <= 3; ++i) { r = int(q[i] * NR); if (r < q[i] * NR) ++r; if (r < 1) r = 1; out = out " " (i == 1 ? "p50" : i == 2 ? "p99" : "max") "=" v[r] }
        print out
    }'
}

field() {
    sed -n "s/^$2: //p" "$1" | head -1
}

{
    echo "conformance digest: ${EXPECTED_DIGEST}"
    echo

    for label in failover-default failover-moderate failover-tight; do
        echo "## ${label}"
        printf 'run\tpromoted\tapplied-at-promotion\tdetection-ns\toutage-ns\tcatch-up-ns\tdigest-equal\n'
        : >"${SCRATCH}/detection" ; : >"${SCRATCH}/outage" ; : >"${SCRATCH}/catchup"

        for standby in "${OUTPUT_DIRECTORY}/${label}"/run*-standby.txt; do
            run="$(basename "${standby}" -standby.txt)"
            primary="${standby%-standby.txt}-primary.txt"
            death="$(field "${primary}" primary-death-monotonic-ns)"
            promoted="$(field "${standby}" promoted)"
            detected="$(field "${standby}" failure-detected-monotonic-ns)"
            first_apply="$(field "${standby}" promoted-first-apply-monotonic-ns)"
            catch_up="$(sed -n 's/^gap first=[0-9]* count=[0-9]* recovery-ns=//p' "${standby}" | head -1)"
            digest="$(field "${standby}" digest-sha256)"

            # A standby that promoted while the primary was alive: the primary
            # fences itself when replication breaks, and every record past
            # the standby's promotion point that the primary also applied is
            # split-brain exposure.
            fenced="$(sed -n 's/^self-fence: replication write failed after \([0-9]*\) records$/\1/p' "${primary}")"

            if [[ -n "${fenced}" && -n "${detected}" ]]; then
                applied="$(field "${standby}" applied-at-promotion)"
                printf '%s\tFALSE-FAILOVER\t%s\tprimary fenced after %s records (overlap %s)\t\t\t%s\n' "${run}" \
                    "${applied}" "${fenced}" "$((fenced - applied))" \
                    "$([[ "${digest}" == "${EXPECTED_DIGEST}" ]] && echo yes || echo NO)"
                continue
            fi

            if [[ -z "${death}" || -z "${detected}" || -z "${first_apply}" ]]; then
                printf '%s\tINCOMPLETE\n' "${run}"
                continue
            fi

            detection=$((detected - death))
            outage=$((first_apply - death))
            echo "${detection}" >>"${SCRATCH}/detection"
            echo "${outage}" >>"${SCRATCH}/outage"
            echo "${catch_up:-0}" >>"${SCRATCH}/catchup"
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${run}" "${promoted}" \
                "$(field "${standby}" applied-at-promotion)" "${detection}" "${outage}" "${catch_up}" \
                "$([[ "${digest}" == "${EXPECTED_DIGEST}" ]] && echo yes || echo NO)"
        done

        echo "detection-ns $(percentiles <"${SCRATCH}"/detection)"
        echo "outage-ns $(percentiles <"${SCRATCH}"/outage)"
        echo "catch-up-ns $(percentiles <"${SCRATCH}"/catchup)"
        echo
    done

    for directory in "${OUTPUT_DIRECTORY}"/gap-fill-*; do
        echo "## $(basename "${directory}")"
        printf 'run\tfeed-sent\tlive-received\tfilled\tgaps-seen\tgaps-recovered\tp50-ns\tp99-ns\tmax-ns\tdigest-equal\n'
        : >"${SCRATCH}/gaps"

        for primary in "${directory}"/run*-primary.txt; do
            run="$(basename "${primary}" -primary.txt)"
            feed="${primary%-primary.txt}-feed.txt"
            digest="$(field "${primary}" digest-sha256)"
            sed -n 's/^gap first=[0-9]* count=[0-9]* recovery-ns=//p' "${primary}" >>"${SCRATCH}/gaps"
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "${run}" "$(field "${feed}" feed-sent)" \
                "$(field "${primary}" live-received)" "$(field "${primary}" filled-records)" \
                "$(field "${primary}" gaps-seen)" "$(field "${primary}" gaps-recovered)" \
                "$(field "${primary}" gap-recovery-ns-p50)" "$(field "${primary}" gap-recovery-ns-p99)" \
                "$(field "${primary}" gap-recovery-ns-max)" \
                "$([[ -n "${digest}" && "${digest}" == "${EXPECTED_DIGEST}" ]] && echo yes || echo NO)"
        done

        echo "all-gaps recovery-ns $(percentiles <"${SCRATCH}"/gaps)"
        echo
    done

    echo "## failures"
    cat "${FAILURES}"
} >"${OUTPUT_DIRECTORY}/summary-raw.txt"

cat "${OUTPUT_DIRECTORY}/summary-raw.txt"
