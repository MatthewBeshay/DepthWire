# Superseded failover run (revision 5a2d096)

This is the first full collection by `scripts/ha/run-evidence.sh`, from revision
`5a2d096c0f7b33cb2e72d791913fd8a19e92bac6`. It is kept because it exposed a
defect. It is not evidence for the current design.

## What went wrong

Most failover runs were false failovers:

| Setting | Timeout | False failovers |
|---|---|---|
| default | 20 ms | 16 of 20 |
| moderate | 8 ms | 17 of 20 |
| tight | 3 ms | 20 of 20 |

In each one the standby promoted while the primary was still running. The
primary then fenced itself 2 to 5 records later when its next replication
write failed. Every final digest still matched the conformance digest. That is
the fencing and determinism working as designed. The detector was right to
fire, because the primary really had stopped sending heartbeats.

**Cause.** The primary checked whether a heartbeat was due only between polls.
Draining a backlog of feed datagrams in one poll could take longer than the
standby's timeout, and no heartbeat was sent in that time. Run 01 here
(`failover-default/`) shows it: the standby received 104 heartbeats, then
none for 20 ms, and promoted at record 9420.

**Fix.** The primary now also checks after every record it applies, and it
reports `heartbeat-max-gap-ns` and `replication-max-send-ns`. In six
diagnostic runs at the default setting after the fix, all six were true
failovers. The largest heartbeat gap was 5.9 ms against a 5 ms interval.

**Kept from this run:** the summary, the failure list, the commands, the
environment, and one false-failover run's reports. The other raw reports were
not kept.

**Gap-fill results.** These came from the same binary and are repeated by the
current collection, so the numbers here are superseded too.
