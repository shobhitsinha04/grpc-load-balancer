#!/usr/bin/env bash
#
# Runs Prometheus + Grafana natively against a load balancer already running on
# this host. No Docker required.
#
# WHY THIS EXISTS ALONGSIDE docker/docker-compose.yml:
# The committed Prometheus and Grafana configs are written for the compose
# stack, so they use container-internal paths and hostnames -- Prometheus
# scrapes `lb:9100` and loads rules from /etc/prometheus/alerts.yml, and
# Grafana reads dashboards from /var/lib/grafana/dashboards. None of that
# resolves on a host running the binaries directly, which is how scripts/demo.sh
# and the README's manual path work. Rather than duplicate every config file
# with host paths, this script generates Grafana's provisioning at run time
# (the paths must be absolute, so they cannot be committed portably) and points
# Prometheus at monitoring/prometheus.local.yml.
#
# The dashboard JSON itself is NOT duplicated: this loads the same
# monitoring/grafana/dashboards/grpc-lb.json the compose stack serves.
#
# Usage:
#   ./build/lb_server --backends=...   # in another terminal, first
#   ./scripts/monitoring.sh
#
#   Grafana     http://localhost:3000  (admin/admin)
#   Prometheus  http://localhost:9090
#
# Requires: brew install prometheus grafana

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN="$ROOT/.monitoring-run"

LB_METRICS_PORT=9100
PROM_PORT=9090
GRAFANA_PORT=3000

green() { printf '\033[32m%s\033[0m\n' "$*"; }
dim()   { printf '\033[2m%s\033[0m\n' "$*"; }
die()   { printf '\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

cleanup() {
  echo
  dim "-- stopping monitoring stack --"
  [ -n "${PROM_PID:-}" ] && kill "$PROM_PID" 2>/dev/null
  [ -n "${GF_PID:-}" ] && kill "$GF_PID" 2>/dev/null
  return 0
}
trap cleanup EXIT INT TERM

# --- preconditions -----------------------------------------------------------
command -v prometheus >/dev/null || die "prometheus not found. Run: brew install prometheus"
command -v grafana    >/dev/null || die "grafana not found. Run: brew install grafana"

# Grafana needs its homepath (static assets + default conf) passed explicitly
# when it is not started from its install directory.
GRAFANA_HOME=""
for candidate in "$(brew --prefix grafana 2>/dev/null)/share/grafana" "/opt/homebrew/share/grafana" "/usr/local/share/grafana"; do
  [ -d "$candidate/public" ] && { GRAFANA_HOME="$candidate"; break; }
done
[ -n "$GRAFANA_HOME" ] || die "could not locate grafana homepath (expected .../share/grafana)"

curl -s --max-time 2 "localhost:$LB_METRICS_PORT/metrics" >/dev/null 2>&1 \
  || die "nothing serving metrics on :$LB_METRICS_PORT.
  Start the load balancer first, in another terminal:
    ./build/lb_server --backends=127.0.0.1:50051,127.0.0.1:50052,127.0.0.1:50053"

for p in $PROM_PORT $GRAFANA_PORT; do
  if lsof -ti:"$p" -sTCP:LISTEN >/dev/null 2>&1; then
    die "port $p already in use by pid $(lsof -ti:"$p" -sTCP:LISTEN | head -1)"
  fi
done

rm -rf "$RUN"; mkdir -p "$RUN/gf-prov/datasources" "$RUN/gf-prov/dashboards" \
                        "$RUN/gf-data" "$RUN/gf-logs" "$RUN/tsdb"

# --- prometheus --------------------------------------------------------------
# Run from monitoring/ so the relative rule_files path in prometheus.local.yml
# resolves; Prometheus resolves those against the process working directory.
( cd "$ROOT/monitoring" && exec prometheus \
    --config.file=prometheus.local.yml \
    --storage.tsdb.path="$RUN/tsdb" \
    --web.listen-address=":$PROM_PORT" ) > "$RUN/prometheus.log" 2>&1 &
PROM_PID=$!

for _ in $(seq 1 60); do
  curl -s --max-time 1 "localhost:$PROM_PORT/-/ready" 2>/dev/null | grep -q "Ready" && break
  sleep 0.5
done
curl -s --max-time 1 "localhost:$PROM_PORT/-/ready" 2>/dev/null | grep -q "Ready" \
  || { cat "$RUN/prometheus.log"; die "prometheus did not become ready"; }
green "  prometheus  ready on :$PROM_PORT  (scraping localhost:$LB_METRICS_PORT)"

# --- grafana -----------------------------------------------------------------
# Mirrors monitoring/grafana/provisioning/, with host paths substituted for the
# container paths the compose stack uses. uid MUST stay "prometheus": the
# dashboard's panels reference the datasource by that uid, and a mismatch
# renders every panel as "Datasource not found".
cat > "$RUN/gf-prov/datasources/prometheus.yml" <<EOF
apiVersion: 1
datasources:
  - name: Prometheus
    type: prometheus
    uid: prometheus
    access: proxy
    url: http://localhost:$PROM_PORT
    isDefault: true
    editable: false
    jsonData:
      timeInterval: 5s
EOF

cat > "$RUN/gf-prov/dashboards/dashboards.yml" <<EOF
apiVersion: 1
providers:
  - name: grpc-lb
    orgId: 1
    folder: ""
    type: file
    updateIntervalSeconds: 10
    allowUiUpdates: true
    options:
      path: $ROOT/monitoring/grafana/dashboards
      foldersFromFilesStructure: false
EOF

grafana server --homepath "$GRAFANA_HOME" \
  cfg:default.paths.data="$RUN/gf-data" \
  cfg:default.paths.logs="$RUN/gf-logs" \
  cfg:default.paths.provisioning="$RUN/gf-prov" \
  cfg:server.http_port="$GRAFANA_PORT" > "$RUN/grafana.log" 2>&1 &
GF_PID=$!

for _ in $(seq 1 90); do
  curl -s --max-time 1 "localhost:$GRAFANA_PORT/api/health" 2>/dev/null | grep -q '"database"' && break
  sleep 0.5
done
curl -s --max-time 1 "localhost:$GRAFANA_PORT/api/health" 2>/dev/null | grep -q '"database"' \
  || { tail -20 "$RUN/grafana.log"; die "grafana did not become ready"; }
green "  grafana     ready on :$GRAFANA_PORT"

# --- verify the target is actually being scraped -----------------------------
dim "  waiting for the first scrape..."
for _ in $(seq 1 30); do
  health=$(curl -s "localhost:$PROM_PORT/api/v1/targets" 2>/dev/null \
    | sed -n 's/.*"health":"\([a-z]*\)".*/\1/p' | head -1)
  [ "$health" = "up" ] && break
  sleep 1
done
if [ "${health:-}" = "up" ]; then
  green "  target UP   prometheus is scraping the load balancer"
else
  printf '\033[33m  target health=%s -- check %s\033[0m\n' "${health:-unknown}" "$RUN/prometheus.log"
fi

echo
green "Monitoring stack running."
echo "  Grafana     http://localhost:$GRAFANA_PORT  (admin/admin)"
echo "              dashboard: 'gRPC L7 Load Balancer'"
echo "  Prometheus  http://localhost:$PROM_PORT"
echo "              try:  lb_healthy_backends"
echo "                    rate(lb_backend_rpcs_total{result=\"ok\"}[1m])"
echo
dim "Send traffic in another terminal to make the panels move:"
dim "  ./build/load_client --requests=600 --delay_ms=50"
dim "Then evict a backend and watch it drop out:"
dim "  kill -USR1 \$(lsof -ti:50052 -sTCP:LISTEN)"
echo
dim "Logs in $RUN/  --  Ctrl-C to stop."
echo

# Hold the terminal; the trap stops both on Ctrl-C.
while kill -0 "$PROM_PID" 2>/dev/null && kill -0 "$GF_PID" 2>/dev/null; do
  sleep 1
done
