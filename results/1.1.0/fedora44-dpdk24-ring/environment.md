# DepthWire 1.1.0 ring evidence environment

Collected 2026-08-05T07:27:09Z from source revision
`9997170584b2a13c36a2c4a3851692931be3488d` (tag `v1.1.0`, clean working
tree, gated before and after collection). Raw capture: `environment-raw.txt`.

## Host

| Item | Value |
|---|---|
| CPU | AMD Ryzen AI 9 365 w/ Radeon 880M (10 cores, 20 threads, SMT on) |
| Topology | 1 socket, 2 threads per core |
| Frequency | invariant TSC at 1,996,200,000 Hz |
| Microcode | 0xb20401b |
| Memory | 28,499,544 kB total |
| Governor | `performance` |
| Platform profile | `performance` |
| perf_event_paranoid | 2 (per-process hardware counters permitted without privilege) |
| OS | Fedora Linux 44 (Workstation Edition) |
| Kernel | 7.1.5-200.fc44.x86_64 |

## Toolchain

| Item | Value |
|---|---|
| Compiler | GCC 16.1.1 20260515 (Red Hat 16.1.1-2) |
| Build | `dpdk-release` preset (Release); effective commands in `compile_commands.json` |
| CMake / Ninja | 4.3.0 / 1.13.2 |
| DPDK | 24.11.4 (distribution packages) |

## Run configuration and controls

- Ring benchmarks run on lcore 0 via `-l 0`, over the virtual Ring PMD with
  `--no-pci --no-huge --no-shconf --no-telemetry --mbuf-pool-ops-name=ring_mp_mc`
  and a unique `--file-prefix` per capture. The scenario counter benchmark is
  transport-neutral and takes no EAL arguments.
- CPU affinity only: no core isolation, no IRQ steering, SMT left enabled.
  Temperature and background load were **uncontrolled** (an ordinary desktop
  session on a workstation laptop).
- Latency and regime benchmarks report a timer-read-overhead distribution before
  the measured epoch, included in reported intervals and never subtracted. The
  scenario benchmark reads hardware counters through `perf_event_open` scaled by
  enabled and running time (a `multiplexed` flag reports PMU time-sharing) and
  the operating-system counters from a `getrusage` delta.
- Each published point discards one warm-up process and records three fresh
  processes; every recorded run is retained.
