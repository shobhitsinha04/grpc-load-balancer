# grpc-load-balancer

A custom **Layer 7 gRPC load balancer** in C++. It terminates gRPC calls, round-robins them across a pool of backend replicas, and uses the **standard gRPC health checking protocol** (`grpc.health.v1.Health/Check`) to evict failed backends from rotation and re-admit them on recovery. Prometheus metrics and a Grafana dashboard included.

## What is actually verified

Being precise about this, because "I built a load balancer" means little without saying what was proven.

| Component | Status |
|---|---|
| C++ load balancer, backends, client | **Built and run.** Compiles clean under `-Wall -Wextra` against gRPC 1.82. |
| Round-robin distribution | **Measured.** 30 requests → 10/10/10 across 3 backends, tallied client-side. |
| Health-based eviction and recovery | **Measured.** Both a backend reporting `NOT_SERVING` and a `SIGKILL`ed backend. Zero failed requests during failover. |
| Request-level retry | **Measured.** With health checks slowed to 8s, 15 requests hit a dead backend and all 15 were retried onto live ones; 30/30 still succeeded. |
| Prometheus | **Verified end-to-end.** Real Prometheus 3.13 scrapes the LB (target `UP`), `promtool` accepts the config, all 6 alert rules load and evaluate, and it recorded a live eviction and recovery in the timeseries. |
| Grafana dashboard | **Verified end-to-end.** Loaded into real Grafana 13.1. All 10 panels resolve the datasource and return live data through Grafana's own query proxy. |
| Docker images | **Authored, never built.** Docker is not installed on the development machine. |
| Kubernetes manifests | **Authored, never applied.** No cluster. YAML syntax validated; not checked against Kubernetes schemas. |
| Terraform GKE module | **Authored, never applied.** `terraform validate` has not run. No GCP project was billed. |

So: **the load balancer and its observability are real and demonstrable. The deployment layer — Docker, Kubernetes, Terraform — is reviewable configuration, not a running system.** Everything in the "verified" rows can be reproduced on any machine with a compiler; see below.

Evidence for the monitoring rows, reproducible via [`scripts/monitoring.sh`](scripts/monitoring.sh):

```
target UP    prometheus is scraping the load balancer
alert rules  6 loaded, health=ok
dashboard    'gRPC L7 Load Balancer' uid=grpc-lb-overview, 10/10 panels resolve

rate(lb_backend_rpcs_total{result="ok"}[5m])  ->  0.3396, 0.3396, 0.3396   (round-robin)
histogram_quantile(0.99, ...)                 ->  0.00243918

be-50052 health across a live failover:  1 1 1 1 1 1 0 1 1
  eviction (1->0): YES     recovery (0->1): YES
  requests ok: 779         failed: 0
```

## Running it

### Prerequisites

C++17, CMake ≥ 3.16, and gRPC + protobuf discoverable via `pkg-config`. Developed against Homebrew gRPC 1.82.1 / protoc 35.1 on macOS arm64; the Docker build uses Debian's gRPC 1.51.

```bash
brew install grpc protobuf cmake      # macOS
```

Verify the toolchain before building — this catches most setup problems up front:

```bash
pkg-config --modversion grpc++        # expect 1.5x+
which protoc grpc_cpp_plugin          # both must resolve
c++ -std=c++17 -E -x c++ /dev/null    # must not error; a broken libc++ fails here
```

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

Produces three binaries in `build/`:

| Binary | Role |
|---|---|
| `lb_server` | The load balancer. Listens on **:50050**, metrics on **:9100**. |
| `backend_server` | An echo replica. You run several, on different ports. |
| `load_client` | Traffic generator. Sends N requests and tallies who answered. |

---

### Option A — the scripted demo *(start here)*

```bash
./scripts/demo.sh
```

Starts everything, runs five scenarios, tears down after itself. Roughly 20 seconds, no arguments, nothing to clean up.

1. **Round-robin** — 30 RPCs across 3 healthy backends → 10/10/10
2. **Backend reports `NOT_SERVING`** *(process stays alive)* → evicted → 15/15
3. **Backend recovers** → re-admitted → 10/10/10
4. **Backend `SIGKILL`ed** → evicted → 15/15
5. **Backend restarted** → re-admitted → 10/10/10

```
  30 RPCs -> 3 healthy backends
  ----------------------------------------------
  be-50051      10  ########
  be-50052      10  ########
  be-50053      10  ########
  ----------------------------------------------
  30/30 ok across 3 backend(s)
```

**Act 2 is the point of the whole project.** That backend is still running and still accepting TCP connections — it is only *answering* that it cannot serve. An L4 balancer sees a healthy socket and keeps routing to it. This one asks over gRPC, hears the answer, and evicts it.

The demo needs ports 50050, 9100 and 50051–50053. If something already holds them it says so and names the process, rather than failing obscurely.

---

### Option B — drive it yourself

**Four terminals.** Each command runs in the foreground and stays running — that's expected. Ctrl-C to stop.

```bash
# Terminal 1
./build/backend_server --port=50051 --id=be-A

# Terminal 2
./build/backend_server --port=50052 --id=be-B

# Terminal 3
./build/backend_server --port=50053 --id=be-C

# Terminal 4 — the load balancer
./build/lb_server --backends=127.0.0.1:50051,127.0.0.1:50052,127.0.0.1:50053
```

Terminal 4 should show all three entering rotation before you send traffic:

```
[lb] health: be-50051 (127.0.0.1:50051) -> HEALTHY, added to rotation
[lb] health: be-50052 (127.0.0.1:50052) -> HEALTHY, added to rotation
[lb] health: be-50053 (127.0.0.1:50053) -> HEALTHY, added to rotation
[lb] self-health -> SERVING (3 healthy backends)
```

Backends start **unhealthy** and must pass two consecutive health checks before receiving traffic, so this takes about a second. That's deliberate: a backend proves it can serve before anything is routed to it.

**Now, in a fifth terminal, experiment:**

```bash
# Baseline — expect an even split
./build/load_client --requests=30
```

```bash
# The interesting one: make a backend report NOT_SERVING WITHOUT killing it.
kill -USR1 $(lsof -ti:50052 -sTCP:LISTEN)
./build/load_client --requests=30      # → 15/15 across be-A and be-C

# Watch terminal 2: the process is still running. It just says it can't serve.
kill -USR2 $(lsof -ti:50052 -sTCP:LISTEN)
./build/load_client --requests=30      # → back to 10/10/10
```

```bash
# Hard kill — no graceful shutdown, the health check just stops getting answers
kill -9 $(lsof -ti:50053 -sTCP:LISTEN)
./build/load_client --requests=30      # → 15/15, still zero failures
```

> ### ⚠️ `-sTCP:LISTEN` is not optional
>
> Plain `lsof -ti:50052` returns **every process touching that port** — which includes the **load balancer**, because it holds a persistent connection to every backend for health checks and traffic.
>
> So `kill -USR1 $(lsof -ti:50052)` signals the LB too. The LB ignores those signals now, but before that fix it died instantly — and the symptom was a client that couldn't reach the *balancer*, which looks nothing like "I signalled a backend."
>
> `-sTCP:LISTEN` filters to the process **listening** on the port. That's the backend, and it's what you meant.
>
> `scripts/demo.sh` avoids this entirely by tracking pids in files.

### Watching what the balancer thinks

```bash
# Who is in rotation right now?
curl -s localhost:9100/metrics | grep lb_backend_healthy

# Distribution — this is round-robin, observable
curl -s localhost:9100/metrics | grep lb_backend_rpcs_total

# Live view, refreshing every second (macOS has no `watch` by default)
while sleep 1; do clear; curl -s localhost:9100/metrics \
  | grep -E '^lb_healthy_backends|^lb_backend_healthy|^lb_requests_total|^lb_retries_total'; done
```

Leave that running in a spare terminal while you kill and revive backends — `lb_backend_healthy` flips 1 → 0 → 1 in real time as the health checker evicts and re-admits.

### Proving the retry layer

Health checking has a detection gap: a backend can die *between* checks. Widen it to 8 seconds and you can watch retry cover for it:

```bash
# Terminal 4 — restart the LB with slow health checks
./build/lb_server --backends=127.0.0.1:50051,127.0.0.1:50052,127.0.0.1:50053 \
  --health_interval_ms=8000 --healthy_threshold=1

# Then immediately:
kill -9 $(lsof -ti:50052 -sTCP:LISTEN)
./build/load_client --requests=30
curl -s localhost:9100/metrics | grep -E '^lb_healthy_backends|^lb_retries_total'
```

You'll see `lb_healthy_backends 3` — the balancer still **believes** the dead backend is fine — alongside `lb_retries_total 15` and **30/30 ok**. Round-robin routed 15 requests into a corpse and retry rescued every one, before health checking noticed anything.

### Monitoring — Prometheus + Grafana, without Docker

```bash
brew install prometheus grafana

# with the LB already running in another terminal:
./scripts/monitoring.sh
```

Then **Grafana at http://localhost:3000** (admin/admin) → dashboard *"gRPC L7 Load Balancer"*, and **Prometheus at http://localhost:9090**.

Drive it while watching the dashboard:

```bash
./build/load_client --requests=600 --delay_ms=50     # panels start moving
kill -USR1 $(lsof -ti:50052 -sTCP:LISTEN)            # watch it drop out of rotation
kill -USR2 $(lsof -ti:50052 -sTCP:LISTEN)            # watch it come back
```

The **Backend health state** panel shows the eviction as a red band; **RPCs routed per backend** shows the survivors absorbing its share; **Request outcomes** stays flat at zero failures throughout. That contrast — visible failover, invisible to clients — is the story worth screenshotting.

Why a script rather than the committed configs directly: `monitoring/prometheus.yml` and `monitoring/grafana/provisioning/` target the docker-compose stack, so they use container paths (`/etc/prometheus/alerts.yml`, `/var/lib/grafana/dashboards`) and the compose hostname `lb:9100`. None of that resolves on a host running the binaries natively. The script generates Grafana's provisioning at run time with absolute host paths and points Prometheus at [`monitoring/prometheus.local.yml`](monitoring/prometheus.local.yml). **The dashboard JSON is not duplicated** — the script loads the same file the compose stack serves, which is what makes running it a real test of the committed artifact.

You can also check the configs without running anything:

```bash
promtool check rules monitoring/alerts.yml            # 6 rules
cd monitoring && promtool check config prometheus.local.yml
```

`promtool check config monitoring/prometheus.yml` **fails on purpose** — it points at `/etc/prometheus/alerts.yml`, which only exists inside the container. That's the compose config, and promtool can't see a Docker path from the host.

### Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `Connection refused ... 127.0.0.1:50050` | The **load balancer** isn't running (:50050 is the LB, not a backend) | Check terminal 4. Restart it. |
| `ERROR: ports already in use` | A previous run or a manual session still holds them | Ctrl-C those terminals; the message names the pids |
| `no healthy backend available` | No backend has passed a health check | Are the backends running? Check the LB log for eviction lines |
| Killing a backend also kills the LB | `lsof -ti:PORT` without `-sTCP:LISTEN` | Add the filter (see the warning above) |
| Code changes have no effect | The running process is the **old binary** | Rebuild, then Ctrl-C and restart that process |
| First run of a fresh binary is slow | macOS validates newly linked binaries | Wait for the `serving on` line; never assume a fixed sleep |
| `monitoring.sh`: nothing serving metrics on :9100 | The LB isn't running | Start `lb_server` first; the script scrapes it, it doesn't start it |
| Grafana panels say "Datasource not found" | Datasource `uid` isn't `prometheus` | The dashboard references that uid explicitly; don't rename it |

## Why L7

gRPC runs over HTTP/2, which **multiplexes many RPCs over one long-lived TCP connection**. An L4 balancer chooses a backend *per connection*, so a client opens one connection, gets pinned to one backend, and every RPC it ever sends goes to that same backend. Balancing accomplishes nothing.

This balancer is itself a gRPC server. It decodes each call down to the RPC and its protobuf message, picks a backend, and issues a **new** gRPC call on a **separate** connection. Because it understands the RPC boundary, it can balance individual calls — and because it parses the request, it can also see the client's deadline and honour whichever of the two budgets is tighter.

## Architecture

```
   client ──gRPC/HTTP2──▶  ┌─────────────────────────┐
                           │   LOAD BALANCER          │
                           │  • gRPC server (L7)      │  ──▶ /metrics :9100
                           │  • round-robin picker    │
                           │  • per-backend health    │
                           │    checker threads       │
                           │  • retry on failure      │
                           └─────────────────────────┘
                              │         │         │      new gRPC calls,
                              ▼         ▼         ▼      separate connections
                          backend   backend   backend
                          EchoService + grpc.health.v1.Health
```

| Path | What lives there |
|---|---|
| [`proto/`](proto/) | `echo.proto`; `health.proto` vendored from gRPC ([why](proto/health.proto)) |
| [`src/lb/`](src/lb/) | [`lb_server.cc`](src/lb/lb_server.cc) forwarding path · [`backend_pool.cc`](src/lb/backend_pool.cc) round-robin + health · [`metrics_server.cc`](src/lb/metrics_server.cc) Prometheus endpoint |
| [`src/backend/`](src/backend/) | Echo backend with gRPC's built-in health service |
| [`src/client/`](src/client/) | Traffic generator that tallies `served_by` |
| [`docker/`](docker/) | Multi-stage Dockerfile + compose stack with Prometheus/Grafana |
| [`k8s/`](k8s/) | Namespace, backend StatefulSet + headless Service, LB Deployment + Service, ServiceMonitor |
| [`terraform/`](terraform/) | GKE module: VPC-native private cluster, node pool, Artifact Registry |
| [`monitoring/`](monitoring/) | Prometheus scrape config, alert rules, Grafana dashboard |

## Design decisions worth knowing

**Health checks ride the same channel as traffic.** Both stubs share one `grpc::Channel`, so a check traverses the connection real requests use. A check on a separate connection can pass while the connection carrying traffic is broken.

**Asymmetric thresholds — evict after 1 failure, re-admit after 2 successes.** The costs aren't symmetric: a bad backend in rotation causes user-visible errors immediately, while a shaky backend re-admitted too eagerly flaps. Fail fast, recover slow. `lb_backend_transitions_total` catches it if this is tuned wrong.

**Jitter on every check round, not just at startup.** Fixed intervals re-synchronise over time; at scale, every LB replica probing every backend on the same tick is a self-inflicted DDoS.

**One checker thread per backend.** A shared loop lets one hung backend's timeout delay every other backend's check — head-of-line blocking. This trades threads for isolation and stops scaling in the thousands.

**Two layers of defence.** Active health checking is proactive but has a detection gap (up to `interval + timeout`). Request-level retry covers that gap instantly. Health checking without retry drops requests in the gap; retry without health checking wastes an attempt on every request, forever.

**The balancer serves the health protocol it consumes.** It reports `NOT_SERVING` when its pool is empty, so Kubernetes' native `readinessProbe: grpc:` routes around a balancer that can't help.

**Liveness probes deliberately do not reuse the health check.** `NOT_SERVING` is a legitimate state ("up, but can't serve"). Restarting a pod for saying so destroys the signal the balancer depends on. Liveness asks a different question — is the process wedged — and a TCP accept answers it.

## Metrics

`http://localhost:9100/metrics`

| Metric | Type | Meaning |
|---|---|---|
| `lb_requests_total{result}` | counter | RPCs by outcome |
| `lb_retries_total` | counter | RPCs re-sent to another backend |
| `lb_healthy_backends` | gauge | Backends in rotation |
| `lb_backend_healthy{backend}` | gauge | Per-backend: 1 in rotation, 0 evicted |
| `lb_backend_rpcs_total{backend,result}` | counter | Distribution — this is round-robin, observable |
| `lb_health_checks_total{backend,result}` | counter | Active check outcomes |
| `lb_backend_transitions_total{backend}` | counter | Health flips; flapping detector |
| `lb_request_duration_seconds` | histogram | End-to-end latency |

Histogram buckets start at 50µs because a loopback hop through the balancer measures ~0.25ms — the bounds were calibrated against a measurement, after a first attempt put every request in one bucket.

## Configuration

```
lb_server
  --port=50050               --metrics_port=9100
  --backends=host:port,...   --timeout_ms=500       --max_attempts=3
  --health_interval_ms=300   --health_timeout_ms=200
  --unhealthy_threshold=1    --healthy_threshold=2  --health_jitter_ms=100

backend_server
  --port=50051  --id=be-1     # SIGUSR1 → report NOT_SERVING, SIGUSR2 → SERVING

load_client
  --target=127.0.0.1:50050  --requests=30  --delay_ms=0
```

## Known limitations

Stated plainly, because each is a reasonable interview question:

- **Round-robin ignores load.** A backend busy with a slow request gets the same share as an idle one. Least-connections or EWMA-latency would fix it. Round-robin is the right starting point — stateless, no coordination, provably fair for uniform requests, which Echo's are.
- **Backends are a static list.** No dynamic discovery. This is why k8s deployment uses a StatefulSet + headless Service for stable DNS rather than a Deployment. Watching the EndpointSlice API is the real fix.
- **`PickExcluding` only excludes the last backend tried**, not every one tried. Bounded and harmless at 3 backends; wrong at scale.
- **No connection draining.** An evicted backend's in-flight requests are lost.
- **Thread-per-backend doesn't scale to thousands.** Async completion queues would.
- **Insecure channels only.** No TLS, no mTLS.
- **The deployment layer is unproven** — see the table at the top.

## License

MIT
