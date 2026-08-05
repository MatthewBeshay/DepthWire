# Benchmark results

Benchmark evidence, grouped as `<version>/<environment>/`, each collected
from a tagged release revision.

- [`1.0.0/fedora44-dpdk24/`](1.0.0/fedora44-dpdk24/): a nine-point replay
  rate sweep, reproduced conformance digests, and three overload-recovery
  runs, collected by `scripts/replay/run-evidence.sh`.
- [`1.1.0/fedora44-dpdk24-ring/`](1.1.0/fedora44-dpdk24-ring/): Ring PMD
  service and regime latency, the closed-loop burst sweep over sizes 1
  through 128, and the engine scenario counters, collected by
  `scripts/ring/run-evidence.sh`.

The required provenance fields, warm-up policy, and retention rules are
defined in [the benchmark methodology](../docs/benchmarks/methodology.md).
Measurements taken before a release tag lacked a verifiable source
revision and were deliberately not retained.
