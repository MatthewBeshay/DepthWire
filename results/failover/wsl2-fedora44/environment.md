# Failover and gap-fill evidence environment

Collected 2026-10-26T12:16:13Z from source revision
`e15cd4579f8f6dcdd5214f8f55a446e5d774c91f` on branch `ha/failover`. The
working tree was clean, and the binary reported that revision before
collection started. This is a branch revision, not a tagged release.
Raw capture: `environment-raw.txt`.

## Host

| Item | Value |
|---|---|
| CPU | AMD Ryzen 9 5950X (16 cores, 32 threads, SMT on) |
| Memory | 16,332,512 kB visible to the VM |
| OS | Fedora Linux 44 under WSL2 on Windows 11 |
| Kernel | 6.18.33.1-microsoft-standard-WSL2 |
| `net.core.rmem_max` | 4,194,304 bytes, so the 8 MiB socket buffer request is capped at 4 MiB |

## Toolchain

| Item | Value |
|---|---|
| Compiler | GCC 16.1.1 20260515 (Red Hat 16.1.1-2) |
| Build | `core-release` preset; `depthwire-ha` is built with `-O3` |
| CMake | 4.3.0 |
| Traffic control | iproute2 6.17.0 `tc` with `netem` |

## Topology

`scripts/ha/setup-netns.sh` creates three network namespaces, `dw-feed`
(10.77.0.1), `dw-primary` (10.77.0.2) and `dw-standby` (10.77.0.3). Each is
joined by a veth pair to the bridge `br-dw`, with multicast snooping off.

**The three "hosts" are namespaces on one machine, sharing one kernel and one
CPU package.** Nothing here measures a physical network, NIC or switch, or
clock skew between machines. Every timestamp comes from the same
`CLOCK_MONOTONIC`, so cross-process differences such as detection latency need
no clock synchronisation. That would not be true across real hosts.

## Workload and controls

- **Trace:** `tests/fixtures/trace/load_uniform_100k_v1.dwtrace`, 100,000
  records. Its single-process conformance digest is `8211716b...21f17cb`
  (`conformance.txt`).
- **Feed:** paced at 100,000 records/s, multicast to 239.77.0.1:47001, after a
  500 ms start delay. The retransmit service stays up 1.5 s after the last
  datagram.
- **Failover runs:** the primary applies record 40,000, prints its
  `CLOCK_MONOTONIC` time, and exits with `_Exit`. That models a process crash,
  so the kernel still delivers what the process had already written.
- **Gap-fill runs:** `netem loss` on the bridge side of the primary's veth,
  which is the primary's ingress, applied in one of two ways:
  - `feed`: UDP only, through a `prio` qdisc and a `u32` filter on IP
    protocol 17;
  - `all`: every packet, including the TCP gap-fill responses.
- **Controls:** none of the following were controlled: CPU affinity, core
  isolation, IRQ steering, frequency governor, background load. This was an
  ordinary desktop session, and the Windows host scheduled the WSL2 VM.
- **Exact commands:** every command appears in `commands.log`.
