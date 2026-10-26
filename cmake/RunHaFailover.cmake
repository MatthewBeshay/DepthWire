# The primary dies part-way through the trace. The standby must detect the
# failure, take over from the live feed with gap-fill, and finish with exactly
# the digest a single-process conformance replay produces
# (docs/architecture/failover.md).
include("${CMAKE_CURRENT_LIST_DIR}/HaScriptSupport.cmake")

depthwire_conformance_digest(expected_digest)

math(EXPR primary_feed_port "${DEPTHWIRE_HA_PORT_BASE}")
math(EXPR standby_feed_port "${DEPTHWIRE_HA_PORT_BASE} + 1")
math(EXPR retransmit_port "${DEPTHWIRE_HA_PORT_BASE} + 2")
math(EXPR replication_port "${DEPTHWIRE_HA_PORT_BASE} + 3")
math(EXPR heartbeat_port "${DEPTHWIRE_HA_PORT_BASE} + 4")
set(die_after 40000)

set(feed_report "${DEPTHWIRE_WORK_DIR}/ha_failover_feed.txt")
set(primary_report "${DEPTHWIRE_WORK_DIR}/ha_failover_primary.txt")
set(standby_report "${DEPTHWIRE_WORK_DIR}/ha_failover_standby.txt")
file(REMOVE "${feed_report}" "${primary_report}" "${standby_report}")

execute_process(
    COMMAND "${DEPTHWIRE_HA_EXECUTABLE}" standby
            "--trace=${DEPTHWIRE_TRACE_FILE}"
            "--feed=127.0.0.1:${standby_feed_port}"
            "--retransmit=127.0.0.1:${retransmit_port}"
            "--replication-listen=127.0.0.1:${replication_port}"
            "--heartbeat-listen=127.0.0.1:${heartbeat_port}"
            "--report=${standby_report}"
    COMMAND "${DEPTHWIRE_HA_EXECUTABLE}" feed
            "--trace=${DEPTHWIRE_TRACE_FILE}"
            "--to=127.0.0.1:${primary_feed_port},127.0.0.1:${standby_feed_port}"
            "--retransmit-listen=127.0.0.1:${retransmit_port}"
            --rate=50000 --start-delay-ms=500 --linger-ms=1500
            "--report=${feed_report}"
    COMMAND "${DEPTHWIRE_HA_EXECUTABLE}" primary
            "--trace=${DEPTHWIRE_TRACE_FILE}"
            "--feed=127.0.0.1:${primary_feed_port}"
            "--retransmit=127.0.0.1:${retransmit_port}"
            "--standby=127.0.0.1:${replication_port}"
            "--heartbeat-to=127.0.0.1:${heartbeat_port}"
            "--die-after=${die_after}"
            "--report=${primary_report}"
    RESULTS_VARIABLE results
    TIMEOUT 60
)

depthwire_require_success("${results}" "${feed_report}" "${primary_report}" "${standby_report}")

depthwire_report_value("${primary_report}" "primary-applied-at-death" applied_at_death)
depthwire_report_value("${primary_report}" "primary-death-monotonic-ns" death_ns)
depthwire_report_value("${standby_report}" "promoted" promoted)
depthwire_report_value("${standby_report}" "failure-detected-monotonic-ns" detected_ns)
depthwire_report_value("${standby_report}" "promoted-first-apply-monotonic-ns" first_apply_ns)
depthwire_report_value("${standby_report}" "digest-sha256" standby_digest)

if(NOT applied_at_death EQUAL die_after OR NOT promoted STREQUAL "yes")
    message(FATAL_ERROR "expected the primary to die at ${die_after} and the standby to promote")
endif()

if(NOT standby_digest STREQUAL expected_digest)
    message(FATAL_ERROR "standby digest ${standby_digest} != conformance digest ${expected_digest}")
endif()

math(EXPR detection_ns "${detected_ns} - ${death_ns}")
math(EXPR outage_ns "${first_apply_ns} - ${death_ns}")
message(STATUS "failover reproduced the conformance digest ${expected_digest}; "
               "detection ${detection_ns} ns, outage ${outage_ns} ns")
