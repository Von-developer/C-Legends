#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
#  C-Legends Docker entrypoint
#
#  Uses --daemon mode: no interactive menu, no stdin reads.
#  The process loads the log file, starts the Prometheus endpoint on :9091,
#  starts the live file watcher, then blocks on SIGTERM/SIGINT.
#  Docker stop → SIGTERM → clean shutdown.
# ─────────────────────────────────────────────────────────────────────────────

set -e

LOG_FILE="${LOG_FILE:-Mac.log}"

echo "[entrypoint] Starting C-Legends engine (daemon mode)"
echo "[entrypoint] Log file : $LOG_FILE"
echo "[entrypoint] Metrics  : http://0.0.0.0:9091/metrics"

# exec replaces the shell process so Docker signals go directly to log_analyzer
exec ./log_analyzer --daemon "$LOG_FILE"
