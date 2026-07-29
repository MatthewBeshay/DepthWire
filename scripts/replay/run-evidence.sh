#!/usr/bin/env bash
# Collects the raw outputs for the release evidence bundle
# (docs/benchmarks/methodology.md, docs/architecture/trace-format.md) and
# verifies, at collection time, everything the
# bundle will later claim: exact release version, embedded revision equal to
# the current clean git HEAD, cross-process conformance digests, lossless
# paced digest equality, sweep knee consistency with the declared recovery
# plan, and overload -> reduced-load recovery with declining backlog.
#
# Usage:
#   scripts/replay/run-evidence.sh [output-directory]
#   scripts/replay/run-evidence.sh finalise <output-directory>
#
# The collect mode writes raw outputs, every exact invoked command
# (commands.log and a "command:" header per capture), the effective build
# database (compile_commands.json), and a checksum manifest. The operator
# then authors environment.md and summary.md from those files and runs the
# finalise mode, which re-checksums the bundle including the authored
# documents.
#
# Warm-up policy: one discarded warm-up run precedes the recorded runs at
# every sweep point and for the recovery plan (retained, labelled "-warmup",
# excluded from summaries); each run additionally samples timer overhead
# before its measurement epoch.
set -euo pipefail

cd "$(dirname "$0")/../.."

# The version gate compares the binary's reported identity against the
# version this tree declares, so it is read from CMakeLists.txt rather
# than maintained here a second time.
EXPECTED_VERSION="$(sed -nE 's/^[[:space:]]*VERSION ([0-9]+\.[0-9]+\.[0-9]+)[[:space:]]*$/\1/p' CMakeLists.txt | head -1)"

if [[ -z "${EXPECTED_VERSION}" ]]; then
    echo "error: could not read the project VERSION from CMakeLists.txt" >&2
    exit 1
fi
REPLAY=./build/dpdk-release/apps/depthwire-replay
CONFORMANCE_TRACE=tests/fixtures/trace/conformance_v1.dwtrace
LOAD_TRACE=tests/fixtures/trace/load_uniform_100k_v1.dwtrace

# Declared sweep (parts-per-million arrival-spacing scales, descending
# spacing = ascending offered rate) and the declared recovery segment plan.
# BELOW_SCALE and OVERLOAD_SCALE must match the recovery plan's segments; the
# sweep results are checked against them (knee validation) before the
# recovery runs are accepted. Every attempted point is published; the
# knee-selection rule is in docs/benchmarks/methodology.md.
SWEEP_SCALES=(1000000 500000 333333 250000 200000 150000 100000 50000 20000)
RUNS_PER_POINT=3
BELOW_SCALE=1000000
OVERLOAD_SCALE=20000
RECOVERY_SEGMENTS="0:${BELOW_SCALE},40001:${OVERLOAD_SCALE},60003:${BELOW_SCALE}"

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

OUTPUT_DIRECTORY="${1:-results/${EXPECTED_VERSION}/raw-$(date +%Y%m%d-%H%M%S)}"

if [[ ! -x "${REPLAY}" ]]; then
    echo "error: ${REPLAY} not built (configure and build the dpdk-release preset)" >&2
    exit 1
fi

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

segment_field() {
    # segment_field <capture> <segment> <field>
    sed -nE "s/^segment $2: .*$3=([0-9]+).*$/\1/p" "${OUTPUT_DIRECTORY}/$1.txt" | head -1
}

require_clean_head() {
    if ! git rev-parse --verify HEAD >/dev/null 2>&1; then
        echo "error: not a git repository; release evidence requires the" >&2
        echo "revision-bearing checkout (see docs/benchmarks/methodology.md)" >&2
        exit 1
    fi

    # The output directory is the run's one expected artefact; everything
    # else must be clean. Prefix-match both ways because git collapses an
    # untracked directory to its topmost unlisted parent.
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
# Provenance gate: the binary must report exactly the release version and the
# clean revision this tree is at, now and when collection finishes.
require_clean_head "before collection"
HEAD_REVISION="$(git rev-parse --verify HEAD)"

mkdir -p "${OUTPUT_DIRECTORY}"
COMMANDS_LOG="${OUTPUT_DIRECTORY}/commands.log"
: >"${COMMANDS_LOG}"

run_and_capture "version" "${REPLAY}" --version

VERSION_LINE="$(capture_field version '^depthwire-replay ')"

if [[ "${VERSION_LINE}" != "depthwire-replay ${EXPECTED_VERSION} (revision ${HEAD_REVISION})" ]]; then
    echo "error: binary identity does not match this clean checkout:" >&2
    echo "  reported: ${VERSION_LINE}" >&2
    echo "  expected: depthwire-replay ${EXPECTED_VERSION} (revision ${HEAD_REVISION})" >&2
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
    echo "== gcc";         gcc --version 2>/dev/null | head -1 || echo "unavailable"
    echo "== clang";       clang --version 2>/dev/null | head -1 || echo "unavailable"
    echo "== cmake";       cmake --version | head -1
    echo "== ninja";       ninja --version
    echo "== dpdk";        pkg-config --modversion libdpdk 2>/dev/null || echo "unavailable"
    echo "== os-release";  cat /etc/os-release
} >"${OUTPUT_DIRECTORY}/environment-raw.txt" 2>&1

cp build/dpdk-release/compile_commands.json "${OUTPUT_DIRECTORY}/compile_commands.json"

# ---------------------------------------------------------------------------
# Trace validation.
run_and_capture "validate-conformance" "${REPLAY}" validate "${CONFORMANCE_TRACE}"
run_and_capture "validate-load" "${REPLAY}" validate "${LOAD_TRACE}"

# ---------------------------------------------------------------------------
# Cross-process determinism: two fresh conformance runs, identical digest.
run_and_capture "conformance-run-1" "${REPLAY}" run "${CONFORMANCE_TRACE}"
run_and_capture "conformance-run-2" "${REPLAY}" run "${CONFORMANCE_TRACE}"

digest_1="$(capture_field conformance-run-1 '^digest-sha256:')"
digest_2="$(capture_field conformance-run-2 '^digest-sha256:')"

if [[ "${digest_1}" != "${digest_2}" || -z "${digest_1}" ]]; then
    echo "error: conformance digests differ or are missing" >&2
    exit 1
fi

echo "conformance digest reproduced: ${digest_1#digest-sha256: }"

# Transport independence: a lossless paced run at recorded spacing and
# matching sequence policy must reproduce the conformance digest.
run_and_capture "paced-lossless" \
    "${REPLAY}" run "${CONFORMANCE_TRACE}" \
    --mode=paced --sequence-policy=contiguous --scale-ppm=1000000 \
    -- "${EAL_ARGS[@]}" "--file-prefix=dw_ev_lossless"

require_in paced-lossless "^counter dropped: +0$" "the lossless paced run dropped events"

if [[ "$(capture_field paced-lossless '^digest-sha256:')" != "${digest_1}" ]]; then
    echo "error: lossless paced digest differs from the conformance digest" >&2
    exit 1
fi

echo "lossless paced digest matches conformance"

# ---------------------------------------------------------------------------
# Declared rate sweep: per point, one discarded warm-up run then
# RUNS_PER_POINT recorded fresh-process runs. Every attempted point is
# retained.
for scale in "${SWEEP_SCALES[@]}"; do
    run_and_capture "sweep-scale${scale}-warmup" \
        "${REPLAY}" run "${LOAD_TRACE}" \
        --mode=paced "--scale-ppm=${scale}" \
        -- "${EAL_ARGS[@]}" "--file-prefix=dw_ev_s${scale}_w"

    for run in $(seq 1 "${RUNS_PER_POINT}"); do
        run_and_capture "sweep-scale${scale}-run${run}" \
            "${REPLAY}" run "${LOAD_TRACE}" \
            --mode=paced "--scale-ppm=${scale}" \
            -- "${EAL_ARGS[@]}" "--file-prefix=dw_ev_s${scale}_r${run}"
    done
done

# Knee validation: the declared recovery plan must be consistent with the
# measured sweep: the below-saturation scale lost nothing in any recorded
# run, and the overload scale showed loss in every recorded run. Adjust the
# declared constants and re-collect if the knee moved.
for run in $(seq 1 "${RUNS_PER_POINT}"); do
    require_in "sweep-scale${BELOW_SCALE}-run${run}" "^counter dropped: +0$" \
        "the declared below-saturation scale ${BELOW_SCALE} dropped events; the knee moved"

    if grep -qE "^counter dropped: +0$" \
        "${OUTPUT_DIRECTORY}/sweep-scale${OVERLOAD_SCALE}-run${run}.txt"; then
        echo "error: the declared overload scale ${OVERLOAD_SCALE} showed no loss" >&2
        echo "in run ${run}; the knee moved; adjust the declared scales" >&2
        exit 1
    fi
done

echo "declared recovery scales are consistent with the measured sweep"

# ---------------------------------------------------------------------------
# Declared overload-recovery segment run (one warm-up, then recorded runs),
# each verified: loss confined to the overload segment, backlog built there,
# backlog declined in the reduced-load segment, recovery took measurable
# time, and the run drained to zero (the exit status already enforces the
# conservation and restoration invariants).
run_and_capture "recovery-segments-warmup" \
    "${REPLAY}" run "${LOAD_TRACE}" \
    --mode=paced "--segments=${RECOVERY_SEGMENTS}" \
    -- "${EAL_ARGS[@]}" "--file-prefix=dw_ev_rec_w"

for run in $(seq 1 "${RUNS_PER_POINT}"); do
    name="recovery-segments-run${run}"
    run_and_capture "${name}" \
        "${REPLAY}" run "${LOAD_TRACE}" \
        --mode=paced "--segments=${RECOVERY_SEGMENTS}" \
        -- "${EAL_ARGS[@]}" "--file-prefix=dw_ev_rec_r${run}"

    require_in "${name}" "^counter backlog-final: +0$" "recovery run left backlog"

    overload_dropped="$(segment_field "${name}" 1 dropped)"
    overload_backlog="$(segment_field "${name}" 1 backlog-at-end)"
    final_backlog="$(segment_field "${name}" 2 backlog-at-end)"
    recovery_ns="$(sed -nE 's/^counter recovery-duration-ns: +([0-9]+)$/\1/p' \
        "${OUTPUT_DIRECTORY}/${name}.txt")"

    if [[ -z "${overload_dropped}" || "${overload_dropped}" -eq 0 ]]; then
        echo "error: ${name}: no loss in the overload segment" >&2
        exit 1
    fi

    if [[ -z "${overload_backlog}" || "${overload_backlog}" -eq 0 ]]; then
        echo "error: ${name}: no backlog built during overload" >&2
        exit 1
    fi

    if [[ -z "${final_backlog}" || "${final_backlog}" -ge "${overload_backlog}" ]]; then
        echo "error: ${name}: backlog did not decline after overload" >&2
        exit 1
    fi

    if [[ -z "${recovery_ns}" || "${recovery_ns}" -eq 0 ]]; then
        echo "error: ${name}: recovery duration missing or zero" >&2
        exit 1
    fi
done

echo "overload -> reduced-load recovery verified in every recorded run"

# ---------------------------------------------------------------------------
# The tree must still be clean: nothing was modified during collection.
require_clean_head "after collection"

# Fixture identity and raw-output checksums (finalise re-runs this with the
# authored summary.md and environment.md included).
sha256sum "${CONFORMANCE_TRACE}" "${LOAD_TRACE}" \
    "${OUTPUT_DIRECTORY}"/*.txt "${OUTPUT_DIRECTORY}"/*.log \
    "${OUTPUT_DIRECTORY}/compile_commands.json" \
    >"${OUTPUT_DIRECTORY}/sha256-manifest.txt"

echo "raw evidence written to ${OUTPUT_DIRECTORY}"
echo "next: author environment.md and summary.md per docs/benchmarks/methodology.md,"
echo "then run: scripts/replay/run-evidence.sh finalise ${OUTPUT_DIRECTORY}"
