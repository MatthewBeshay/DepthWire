#!/usr/bin/env bash
# Collects the raw outputs for the Ring PMD and scenario-counter evidence
# bundle (docs/benchmarks/methodology.md) and verifies, at collection time,
# the release version and the embedded revision equal to the current clean git
# HEAD. It gathers, per the methodology, one discarded warm-up process then
# three recorded fresh processes for each benchmark point, retaining every run.
#
# Usage:
#   scripts/ring/run-evidence.sh [output-directory]
#   scripts/ring/run-evidence.sh finalise <output-directory>
#
# The collect mode writes raw outputs, every exact invoked command
# (commands.log and a "command:" header per capture), the effective build
# database (compile_commands.json), and a checksum manifest. The operator then
# authors environment.md and summary.md from those files and runs the finalise
# mode, which re-checksums the bundle including the authored documents.
set -euo pipefail

cd "$(dirname "$0")/../.."

# The version gate compares each binary's reported identity against the version
# this tree declares, read from CMakeLists.txt rather than maintained here a
# second time.
EXPECTED_VERSION="$(sed -nE 's/^[[:space:]]*VERSION ([0-9]+\.[0-9]+\.[0-9]+)[[:space:]]*$/\1/p' CMakeLists.txt | head -1)"

if [[ -z "${EXPECTED_VERSION}" ]]; then
    echo "error: could not read the project VERSION from CMakeLists.txt" >&2
    exit 1
fi

BENCH_DIR=./build/dpdk-release/benchmarks
RING_LATENCY="${BENCH_DIR}/depthwire_ring_latency"
RING_REGIMES="${BENCH_DIR}/depthwire_ring_regimes"
RING_LOAD="${BENCH_DIR}/depthwire_ring_load"
SCENARIO_COUNTERS="${BENCH_DIR}/depthwire_scenario_counters"

# Every attempted burst size is published; the selection rule is in
# docs/benchmarks/methodology.md.
BURST_SIZES=(1 2 4 8 16 32 64 128)
SCENARIOS=(no-signal buy-signal sell-signal invalid-frame duplicate sequence-gap risk-reject outbox-full)
RUNS_PER_POINT=3
SCENARIO_ITERATIONS=50000000
SCENARIO_WARMUP=1000000

# The ring benchmarks poll a virtual Ring PMD and need EAL arguments; the
# scenario counter benchmark is transport-neutral and takes none.
EAL_ARGS=(-l 0 --no-pci --no-huge --no-shconf --no-telemetry
          --mbuf-pool-ops-name=ring_mp_mc)

# ---------------------------------------------------------------------------
# finalise mode: checksum the authored bundle documents alongside the raws.
if [[ "${1:-}" == "finalise" ]]; then
    BUNDLE="${2:?usage: run-evidence.sh finalise <output-directory>}"

    for required in summary.md environment.md; do
        if [[ ! -f "${BUNDLE}/${required}" ]]; then
            echo "error: ${BUNDLE}/${required} is missing; author it per" >&2
            echo "docs/benchmarks/methodology.md before finalising" >&2
            exit 1
        fi
    done

    (
        cd "${BUNDLE}"
        sha256sum ./*.txt ./*.log ./summary.md ./environment.md \
            ./compile_commands.json 2>/dev/null | grep -v "sha256-manifest"
    ) >"${BUNDLE}/sha256-manifest.txt"

    echo "finalised manifest written to ${BUNDLE}/sha256-manifest.txt"
    exit 0
fi

OUTPUT_DIRECTORY="${1:-results/${EXPECTED_VERSION}/fedora44-dpdk24-ring}"

for binary in "${RING_LATENCY}" "${RING_REGIMES}" "${RING_LOAD}" "${SCENARIO_COUNTERS}"; do
    if [[ ! -x "${binary}" ]]; then
        echo "error: ${binary} not built (configure and build the dpdk-release preset)" >&2
        exit 1
    fi
done

run_and_capture() {
    local name="$1"
    shift
    echo "== ${name}"
    printf '%s: %q ' "${name}" "$@" >>"${COMMANDS_LOG}"
    printf '\n' >>"${COMMANDS_LOG}"
    {
        printf 'command:'
        printf ' %q' "$@"
        printf '\n'
        "$@"
    } >"${OUTPUT_DIRECTORY}/${name}.txt" 2>&1 ||
        { echo "error: ${name} failed; see ${OUTPUT_DIRECTORY}/${name}.txt" >&2; exit 1; }
}

capture_field() {
    grep -E "$2" "${OUTPUT_DIRECTORY}/$1.txt" | head -1
}

require_in() {
    if ! grep -qE "$2" "${OUTPUT_DIRECTORY}/$1.txt"; then
        echo "error: $3 (pattern '$2' not found in $1.txt)" >&2
        exit 1
    fi
}

require_measured_zero() {
    # require_measured_zero <capture> <counter-label> <message>
    # The load benchmark prints two identically-labelled counter blocks
    # ("Warmup counters:" then "Measured counters:"). A plain grep would match
    # the warmup block and miss a measured-phase drop, so this scopes the check
    # to the measured block alone.
    local measured
    measured="$(sed -n '/^Measured counters:/,/^Virtual port counters/p' "${OUTPUT_DIRECTORY}/$1.txt")"

    if ! grep -qE "^  $2: +0$" <<<"${measured}"; then
        echo "error: $3 (measured $2 is nonzero in $1.txt)" >&2
        exit 1
    fi
}

require_clean_head() {
    if ! git rev-parse --verify HEAD >/dev/null 2>&1; then
        echo "error: not a git repository; release evidence requires the" >&2
        echo "revision-bearing checkout (see docs/benchmarks/methodology.md)" >&2
        exit 1
    fi

    # The output directory is the run's one expected artefact; everything else
    # must be clean. Prefix-match both ways because git collapses an untracked
    # directory to its topmost unlisted parent.
    local out_rel dirty
    out_rel="$(realpath -m --relative-to=. "${OUTPUT_DIRECTORY}")/"
    dirty="$(git status --porcelain --untracked-files=normal | awk -v out="${out_rel}" '
        {
            path = substr($0, 4)
            if (substr(out, 1, length(path)) == path) next
            if (substr(path, 1, length(out)) == out) next
            print
        }')"

    if [[ -n "${dirty}" ]]; then
        echo "error: the working tree is not clean ($1)" >&2
        printf '%s\n' "${dirty}" >&2
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Provenance gate: the binaries must report exactly the release version and the
# clean revision this tree is at, now and when collection finishes.
require_clean_head "before collection"
HEAD_REVISION="$(git rev-parse --verify HEAD)"

mkdir -p "${OUTPUT_DIRECTORY}"
COMMANDS_LOG="${OUTPUT_DIRECTORY}/commands.log"
: >"${COMMANDS_LOG}"

# The ring benchmarks have no --version subcommand; they print their identity
# only inside a full successful run. A tiny load run is the cheap pre-flight.
run_and_capture "provenance" \
    "${RING_LOAD}" "${EAL_ARGS[@]}" "--file-prefix=dw_ev_ring_pre" \
    -- --warmup=64 --events=256 --burst=32

PROVENANCE_VERSION="$(capture_field provenance '^DepthWire version:')"
PROVENANCE_REVISION="$(capture_field provenance '^Source revision:')"

if [[ "${PROVENANCE_VERSION}" != "DepthWire version:"*"${EXPECTED_VERSION}" ]]; then
    echo "error: benchmark version does not match this checkout:" >&2
    echo "  reported: ${PROVENANCE_VERSION}" >&2
    echo "  expected version: ${EXPECTED_VERSION}" >&2
    exit 1
fi

if [[ "${PROVENANCE_REVISION}" != "Source revision:"*"${HEAD_REVISION}" ]]; then
    echo "error: benchmark revision does not match this clean checkout:" >&2
    echo "  reported: ${PROVENANCE_REVISION}" >&2
    echo "  expected revision: ${HEAD_REVISION}" >&2
    echo "rebuild the dpdk-release preset from this revision" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Environment capture (raw; the operator distills environment.md from this)
# and the effective build database.
{
    echo "== date";        date -u +"%Y-%m-%dT%H:%M:%SZ"
    echo "== revision";    echo "${HEAD_REVISION}"
    echo "== uname";       uname -a
    echo "== cpu";         lscpu
    echo "== microcode";   grep -m1 microcode /proc/cpuinfo || echo "unavailable"
    echo "== nproc";       nproc
    echo "== memory";      grep -E "MemTotal|SwapTotal" /proc/meminfo
    echo "== governor";    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor \
                               2>/dev/null || echo "unavailable"
    echo "== profile";     cat /sys/firmware/acpi/platform_profile 2>/dev/null \
                               || echo "unavailable"
    echo "== perf_paranoid"; cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "unavailable"
    echo "== gcc";         gcc --version 2>/dev/null | head -1 || echo "unavailable"
    echo "== clang";       clang --version 2>/dev/null | head -1 || echo "unavailable"
    echo "== cmake";       cmake --version | head -1
    echo "== ninja";       ninja --version
    echo "== dpdk";        pkg-config --modversion libdpdk 2>/dev/null || echo "unavailable"
    echo "== os-release";  cat /etc/os-release
} >"${OUTPUT_DIRECTORY}/environment-raw.txt" 2>&1

cp build/dpdk-release/compile_commands.json "${OUTPUT_DIRECTORY}/compile_commands.json"

# ---------------------------------------------------------------------------
# Ring service latency: one discarded warm-up process then RUNS_PER_POINT
# recorded fresh processes.
run_and_capture "ring-latency-warmup" \
    "${RING_LATENCY}" "${EAL_ARGS[@]}" "--file-prefix=dw_ev_ring_lat_w"

for run in $(seq 1 "${RUNS_PER_POINT}"); do
    name="ring-latency-run${run}"
    run_and_capture "${name}" \
        "${RING_LATENCY}" "${EAL_ARGS[@]}" "--file-prefix=dw_ev_ring_lat_r${run}"
    require_in "${name}" "^Status: +OK$" "ring latency run did not complete"
    require_in "${name}" "^  Failures: +0$" "ring latency run reported failures"
done

# ---------------------------------------------------------------------------
# Ring regime latency (three distributions per run).
run_and_capture "ring-regimes-warmup" \
    "${RING_REGIMES}" "${EAL_ARGS[@]}" "--file-prefix=dw_ev_ring_reg_w"

for run in $(seq 1 "${RUNS_PER_POINT}"); do
    name="ring-regimes-run${run}"
    run_and_capture "${name}" \
        "${RING_REGIMES}" "${EAL_ARGS[@]}" "--file-prefix=dw_ev_ring_reg_r${run}"
    require_in "${name}" "^Status: +OK$" "ring regime run did not complete"
    require_in "${name}" "^  Total failures: +0$" "ring regime run reported failures"
done

# ---------------------------------------------------------------------------
# Ring burst/load sweep: every attempted burst size, warm-up then recorded
# runs, each asserted lossless (this is closed-loop pressure, not offered load).
for burst in "${BURST_SIZES[@]}"; do
    run_and_capture "load-burst${burst}-warmup" \
        "${RING_LOAD}" "${EAL_ARGS[@]}" "--file-prefix=dw_ev_ring_load_b${burst}_w" \
        -- --warmup=100000 --events=1000000 "--burst=${burst}"

    for run in $(seq 1 "${RUNS_PER_POINT}"); do
        name="load-burst${burst}-run${run}"
        run_and_capture "${name}" \
            "${RING_LOAD}" "${EAL_ARGS[@]}" "--file-prefix=dw_ev_ring_load_b${burst}_r${run}" \
            -- --warmup=100000 --events=1000000 "--burst=${burst}"
        require_in "${name}" "^Status: +OK$" "load burst ${burst} run did not complete"
        require_measured_zero "${name}" "Injection drops" "load burst ${burst} injected drops"
        require_measured_zero "${name}" "Order TX drops" "load burst ${burst} dropped orders"
        require_measured_zero "${name}" "Invalid/unexpected" "load burst ${burst} saw invalid orders"
    done
done

# ---------------------------------------------------------------------------
# Engine scenario counters: per scenario, warm-up then recorded fresh
# processes. Hardware counters are required for evidence; a run without them
# (perf unavailable) fails the counter assertion loudly.
for scenario in "${SCENARIOS[@]}"; do
    run_and_capture "scenario-${scenario}-warmup" \
        "${SCENARIO_COUNTERS}" "--scenario=${scenario}" \
        "--iterations=${SCENARIO_ITERATIONS}" "--warmup=${SCENARIO_WARMUP}"

    for run in $(seq 1 "${RUNS_PER_POINT}"); do
        name="scenario-${scenario}-run${run}"
        run_and_capture "${name}" \
            "${SCENARIO_COUNTERS}" "--scenario=${scenario}" \
            "--iterations=${SCENARIO_ITERATIONS}" "--warmup=${SCENARIO_WARMUP}"
        require_in "${name}" "^Status: +OK$" "scenario ${scenario} did not complete"
        require_in "${name}" "^counter cycles: +[0-9]+$" \
            "scenario ${scenario} produced no hardware counters (perf unavailable?)"
    done
done

echo "ring and scenario evidence collected: latency, regimes, ${#BURST_SIZES[@]} bursts, ${#SCENARIOS[@]} scenarios"

# ---------------------------------------------------------------------------
# The tree must still be clean: nothing was modified during collection.
require_clean_head "after collection"

sha256sum \
    "${OUTPUT_DIRECTORY}"/*.txt "${OUTPUT_DIRECTORY}"/*.log \
    "${OUTPUT_DIRECTORY}/compile_commands.json" \
    >"${OUTPUT_DIRECTORY}/sha256-manifest.txt"

echo "raw evidence written to ${OUTPUT_DIRECTORY}"
echo "next: author environment.md and summary.md per docs/benchmarks/methodology.md,"
echo "then run: scripts/ring/run-evidence.sh finalise ${OUTPUT_DIRECTORY}"
