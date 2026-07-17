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
| Prometheus metrics | **Verified.** Endpoint scraped, format and content-type checked, every dashboard/alert metric confirmed to exist. |
| Docker images | **Authored, never built.** Docker is not installed on the development machine. |
| Kubernetes manifests | **Authored, never applied.** No cluster. YAML syntax validated; semantics reviewed, not executed. |
| Terraform GKE module | **Authored, never applied.** `terraform plan` has not run. No GCP project was billed. |
| Grafana dashboard | **Authored, JSON validated, never rendered.** Queries are written against metrics confirmed to exist. |

So: **the load balancer is real and demonstrable. The deployment layer is reviewable configuration, not a running system.** If you want to see it work, run the demo below — it needs nothing but a compiler.

## The demo

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
./scripts/demo.sh
```

Five acts, roughly 20 seconds:

1. **Round-robin** — 30 RPCs across 3 healthy backends → 10/10/10
2. **Backend reports `NOT_SERVING`** *(process stays alive)* → evicted → 15/15
3. **Backend recovers** → re-admitted → 10/10/10
4. **Backend `SIGKILL`ed** → evicted → 15/15
5. **Backend restarted** → re-admitted → 10/10/10

Sample output:

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

### Driving it by hand

Four terminals:

```bash
./build/backend_server --port=50051 --id=be-A
./build/backend_server --port=50052 --id=be-B
./build/backend_server --port=50053 --id=be-C
./build/lb_server --backends=127.0.0.1:50051,127.0.0.1:50052,127.0.0.1:50053
```

Then:

```bash
./build/load_client --requests=30            # expect an even split

# Make one backend report NOT_SERVING without killing it:
kill -USR1 $(lsof -ti:50052 -sTCP:LISTEN)    # note the -sTCP:LISTEN
./build/load_client --requests=30            # expect 15/15, process still alive
kill -USR2 $(lsof -ti:50052 -sTCP:LISTEN)    # recover

kill -9 $(lsof -ti:50053 -sTCP:LISTEN)       # hard kill -> evicted

curl -s localhost:9100/metrics | grep lb_backend_healthy
```

**`-sTCP:LISTEN` is not optional.** Plain `lsof -ti:50052` returns every process *touching* that port — which includes the load balancer, since it holds an established connection to each backend. Without the filter, `kill -USR1` fans out to the LB too. The LB now ignores those signals, but the filter is still what you mean: address the process *listening* on the port, not everyone talking to it. `scripts/demo.sh` sidesteps this entirely by tracking pids in files.

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

## Build requirements

C++17, CMake ≥ 3.16, gRPC + protobuf with `pkg-config` (`grpc++`), and `grpc_cpp_plugin`. Developed against Homebrew gRPC 1.82.1 / protoc 35.1 on macOS arm64.

```bash
brew install grpc protobuf   # macOS
```

## License

MIT
