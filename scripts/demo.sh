#!/usr/bin/env bash
#
# End-to-end local demo: round-robin distribution, health-based eviction, and
# recovery -- all observed from the client side.
#
# Every wait here polls for an actual observable condition (a log line, a
# metric value) rather than sleeping a guessed number of seconds. Fixed sleeps
# are how demos become flaky on a slower machine or a cold binary.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
RUN="$ROOT/.demo-run"

LB_PORT=50050
METRICS_PORT=9100
BACKEND_PORTS="50051 50052 50053"
REQUESTS=30

bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
dim()   { printf '\033[2m%s\033[0m\n' "$*"; }

act() {
  echo
  printf '\033[1;36m%s\033[0m\n' "==============================================================="
  printf '\033[1;36m  %s\033[0m\n' "$*"
  printf '\033[1;36m%s\033[0m\n' "==============================================================="
}

cleanup() {
  dim ""
  dim "-- tearing down --"
  [ -n "${LB_PID:-}" ] && kill "$LB_PID" 2>/dev/null
  for p in $BACKEND_PORTS; do
    pid="$(cat "$RUN/be-$p.pid" 2>/dev/null)"
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
  done
  sleep 0.3
  pkill -f "$BUILD/backend_server" 2>/dev/null
  pkill -f "$BUILD/lb_server" 2>/dev/null
  return 0
}
trap cleanup EXIT INT TERM

metric() { # metric <name> -> value, or empty
  curl -s --max-time 1 "localhost:$METRICS_PORT/metrics" 2>/dev/null \
    | awk -v n="^$1" '$0 ~ n {print $2; exit}'
}

wait_for_log() { # wait_for_log <file> <pattern> <label>
  for _ in $(seq 1 80); do
    grep -q "$2" "$1" 2>/dev/null && return 0
    sleep 0.25
  done
  echo "TIMED OUT waiting for $3" >&2
  return 1
}

wait_for_healthy() { # wait_for_healthy <count>
  for _ in $(seq 1 80); do
    [ "$(metric lb_healthy_backends)" = "$1" ] && return 0
    sleep 0.25
  done
  echo "TIMED OUT waiting for $1 healthy backend(s); got $(metric lb_healthy_backends)" >&2
  return 1
}

backend_pid() { cat "$RUN/be-$1.pid" 2>/dev/null; }

start_backend() { # start_backend <port>
  "$BUILD/backend_server" --port="$1" --id="be-$1" > "$RUN/be-$1.log" 2>&1 &
  echo $! > "$RUN/be-$1.pid"
  # Detach from job control so the shell does not print its own "Killed: 9"
  # notice over the demo output when Act 4 kills a backend. We track pids in
  # files, so nothing here depends on the job table.
  disown %% 2>/dev/null
  wait_for_log "$RUN/be-$1.log" "serving on" "backend $1"
}

traffic() { # traffic <label>
  "$BUILD/load_client" --target="127.0.0.1:$LB_PORT" --requests="$REQUESTS" --label="$1"
}

# --------------------------------------------------------------------------
if [ ! -x "$BUILD/lb_server" ]; then
  bold "Building first..."
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release > /dev/null || exit 1
  cmake --build "$BUILD" -j8 > /dev/null || exit 1
fi

rm -rf "$RUN"; mkdir -p "$RUN"
# Only reaps processes this script started (it launches them by absolute path).
# A manual run from the README uses ./build/..., which deliberately does not
# match -- this script has no business killing servers someone else is using.
pkill -f "$BUILD/backend_server" 2>/dev/null
pkill -f "$BUILD/lb_server" 2>/dev/null
sleep 0.3

# ...which means the ports can still be held by a manual run. Say so plainly:
# without this check the readiness wait just times out, and a bind failure
# looks identical to a slow machine.
busy=""
for p in $LB_PORT $METRICS_PORT $BACKEND_PORTS; do
  lsof -ti:"$p" -sTCP:LISTEN >/dev/null 2>&1 && busy="$busy $p"
done
if [ -n "$busy" ]; then
  echo >&2
  echo "ERROR: ports already in use:$busy" >&2
  echo >&2
  for p in $busy; do
    pid="$(lsof -ti:"$p" -sTCP:LISTEN 2>/dev/null | head -1)"
    echo "  :$p  held by pid $pid  ($(ps -p "$pid" -o args= 2>/dev/null | cut -c1-60))" >&2
  done
  echo >&2
  echo "This demo needs those ports. Stop the processes above (Ctrl-C in their" >&2
  echo "terminals) and re-run. If they are a manual session from the README," >&2
  echo "that is expected -- the two cannot run at the same time." >&2
  exit 1
fi

act "SETUP: 3 backend replicas + 1 L7 load balancer"
for p in $BACKEND_PORTS; do
  start_backend "$p" || exit 1
  echo "  backend up: 127.0.0.1:$p (pid $(backend_pid "$p"))"
done

TARGETS="$(echo $BACKEND_PORTS | tr ' ' '\n' | sed 's/^/127.0.0.1:/' | paste -sd, -)"
"$BUILD/lb_server" --port="$LB_PORT" --backends="$TARGETS" --metrics_port="$METRICS_PORT" \
  > "$RUN/lb.log" 2>&1 &
LB_PID=$!
wait_for_log "$RUN/lb.log" "listening on" "load balancer" || exit 1
echo "  load balancer up: 127.0.0.1:$LB_PORT (pid $LB_PID)"
wait_for_healthy 3 || exit 1
green "  all 3 backends passed active health checks and entered rotation"

act "ACT 1: round-robin across 3 healthy backends"
dim "  Each reply carries served_by, stamped by the backend that handled it."
dim "  The tally below is measured by the client, not reported by the balancer."
traffic "$REQUESTS RPCs -> 3 healthy backends"

act "ACT 2: backend reports UNHEALTHY (process stays alive)"
BE=50052
PID="$(backend_pid $BE)"
dim "  SIGUSR1 -> be-$BE flips its health status to NOT_SERVING."
dim "  Note it keeps running and keeps accepting TCP connections. An L4"
dim "  balancer would happily keep routing to it. Ours asks over gRPC."
kill -USR1 "$PID"
wait_for_healthy 2 || exit 1
ps -p "$PID" > /dev/null && green "  be-$BE process is STILL ALIVE (pid $PID) -- only its health answer changed"
green "  load balancer evicted be-$BE from rotation"
traffic "$REQUESTS RPCs -> be-$BE evicted"

act "ACT 3: backend recovers"
dim "  SIGUSR2 -> be-$BE reports SERVING again."
kill -USR2 "$PID"
wait_for_healthy 3 || exit 1
green "  load balancer re-added be-$BE to rotation"
traffic "$REQUESTS RPCs -> be-$BE recovered"

act "ACT 4: backend dies hard (SIGKILL, no graceful shutdown)"
BE=50053
PID="$(backend_pid $BE)"
dim "  No goodbye, no draining. The health check simply stops getting answers."
kill -9 "$PID"
wait_for_healthy 2 || exit 1
green "  load balancer evicted the dead be-$BE"
traffic "$REQUESTS RPCs -> be-$BE killed"

act "ACT 5: dead backend is restarted"
start_backend "$BE" || exit 1
wait_for_healthy 3 || exit 1
green "  restarted be-$BE passed health checks and was re-added"
traffic "$REQUESTS RPCs -> be-$BE restarted"

act "SCOREBOARD"
echo
bold "  Health-check outcomes per backend:"
curl -s "localhost:$METRICS_PORT/metrics" | grep '^lb_health_checks_total' | sed 's/^/    /'
echo
bold "  Health state flips (evictions + re-admissions):"
curl -s "localhost:$METRICS_PORT/metrics" | grep '^lb_backend_transitions_total' | sed 's/^/    /'
echo
bold "  Request outcomes through the balancer:"
curl -s "localhost:$METRICS_PORT/metrics" | grep -E '^lb_requests_total|^lb_retries_total' | sed 's/^/    /'
echo
bold "  Load balancer eviction/recovery log:"
grep 'health:' "$RUN/lb.log" | sed 's/^/    /'
echo
green "Demo complete. Metrics endpoint: http://localhost:$METRICS_PORT/metrics"
dim "Logs kept in $RUN/"
echo
