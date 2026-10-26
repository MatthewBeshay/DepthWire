# The feed drops a seeded 1% of datagrams. The receiver must recover every gap
# by retransmission and finish with exactly the conformance digest
# (docs/architecture/failover.md).
include("${CMAKE_CURRENT_LIST_DIR}/HaScriptSupport.cmake")

depthwire_conformance_digest(expected_digest)

math(EXPR feed_port "${DEPTHWIRE_HA_PORT_BASE}")
math(EXPR retransmit_port "${DEPTHWIRE_HA_PORT_BASE} + 1")

set(feed_report "${DEPTHWIRE_WORK_DIR}/ha_gap_fill_feed.txt")
set(primary_report "${DEPTHWIRE_WORK_DIR}/ha_gap_fill_primary.txt")
file(REMOVE "${feed_report}" "${primary_report}")

execute_process(
    COMMAND "${DEPTHWIRE_HA_EXECUTABLE}" feed
            "--trace=${DEPTHWIRE_TRACE_FILE}"
            "--to=127.0.0.1:${feed_port}"
            "--retransmit-listen=127.0.0.1:${retransmit_port}"
            --rate=50000 --drop-rate=0.01 --seed=7 --start-delay-ms=500 --linger-ms=1500
            "--report=${feed_report}"
    COMMAND "${DEPTHWIRE_HA_EXECUTABLE}" primary
            "--trace=${DEPTHWIRE_TRACE_FILE}"
            "--feed=127.0.0.1:${feed_port}"
            "--retransmit=127.0.0.1:${retransmit_port}"
            "--report=${primary_report}"
    RESULTS_VARIABLE results
    TIMEOUT 60
)

depthwire_require_success("${results}" "${feed_report}" "${primary_report}")

depthwire_report_value("${feed_report}" "feed-dropped" dropped)
depthwire_report_value("${primary_report}" "filled-records" filled)
depthwire_report_value("${primary_report}" "gaps-seen" gaps_seen)
depthwire_report_value("${primary_report}" "gaps-recovered" gaps_recovered)
depthwire_report_value("${primary_report}" "digest-sha256" digest)

# Kernel drops can add gaps beyond the seeded ones, never remove them.
if(dropped EQUAL 0 OR filled LESS dropped OR gaps_seen EQUAL 0 OR NOT gaps_seen EQUAL gaps_recovered)
    message(FATAL_ERROR "gap accounting: dropped ${dropped}, filled ${filled}, "
                        "seen ${gaps_seen}, recovered ${gaps_recovered}")
endif()

if(NOT digest STREQUAL expected_digest)
    message(FATAL_ERROR "gap-fill digest ${digest} != conformance digest ${expected_digest}")
endif()

message(STATUS "gap-fill recovered ${gaps_recovered} gaps (${filled} records) and reproduced ${expected_digest}")
