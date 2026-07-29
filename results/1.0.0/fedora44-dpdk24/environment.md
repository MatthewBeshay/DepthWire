# DepthWire 1.0.0 evidence environment

Collected 2026-07-29T22:59:56Z from source revision
`2f6e29a8f513e35e037723da15a9261936ff32bb` (tag `v1.0.0`, clean working
tree, gated before and after collection). Raw capture: `environment-raw.txt`.

## Host

| Item | Value |
|---|---|
| CPU | AMD Ryzen AI 9 365 w/ Radeon 880M (10 cores, 20 threads, SMT on) |
| Topology | 1 socket, 1 NUMA node, CPUs 0-19 |
| Frequency | max 5090 MHz, boost enabled, invariant TSC at 1,996,200,000 Hz |
| Microcode | 0xb20401b |
| Memory | 28,499,536 kB total, 8,388,604 kB swap |
| Governor | `performance` |
| Platform profile | `performance` |
| OS | Fedora Linux 44 (Workstation Edition) |
| Kernel | 7.1.5-200.fc44.x86_64 |

## Toolchain

| Item | Value |
|---|---|
| Compiler | GCC 16.1.1 20260515 (Red Hat 16.1.1-2) |
| Build | `dpdk-release` preset (Release); effective commands in `compile_commands.json` |
| CMake / Ninja | 4.3.0 / 1.13.2 |
| DPDK | 24.11.4 (distribution packages, shared linkage) |

## Run configuration and controls

- All runs on lcore 0 via `-l 0`; virtual Ring PMD with `--no-pci
  --no-huge --no-shconf --no-telemetry --mbuf-pool-ops-name=ring_mp_mc`.
- CPU affinity only: no core isolation, no IRQ steering, SMT left
  enabled. Temperature and background load were **uncontrolled** (an
  ordinary desktop session on a workstation laptop).
- Each run samples and reports its own timer-read-overhead distribution
  before the measurement epoch; representative values are in
  `summary.md` and every raw capture.
