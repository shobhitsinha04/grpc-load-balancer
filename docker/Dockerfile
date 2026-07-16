# Multi-stage build producing two images from one compile.
#
# Build with --target to select which binary you want:
#   docker build -f docker/Dockerfile --target lb      -t grpc-lb:dev .
#   docker build -f docker/Dockerfile --target backend -t grpc-backend:dev .
#
# NOTE ON VERIFICATION: this file has not been built. Docker is not installed
# on the machine this project was developed on, so it is authored from the
# known-good local build (Homebrew gRPC 1.82) translated to Debian's packaged
# gRPC. It is reviewable, not proven. See README.md.

# ---------------------------------------------------------------------------
# Build stage
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS build

# protobuf-compiler-grpc supplies grpc_cpp_plugin, which CMakeLists.txt locates
# via find_program. libgrpc++-dev brings the headers and the .pc files that
# pkg_check_modules(grpc++) resolves against.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      pkg-config \
      protobuf-compiler \
      protobuf-compiler-grpc \
      libgrpc++-dev \
      libprotobuf-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY proto ./proto
COPY src ./src

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)"

# ---------------------------------------------------------------------------
# Runtime base
#
# Runtime library names are pinned to what Debian bookworm ships (gRPC 1.51).
# If the base image moves to a new Debian release these sonames change and must
# be updated -- an apt error at build time will say so plainly.
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime-base

RUN apt-get update && apt-get install -y --no-install-recommends \
      libgrpc++1.51 \
      libgrpc29 \
      libprotobuf32 \
    && rm -rf /var/lib/apt/lists/*

# Unprivileged: neither binary needs root, and neither binds a port below 1024.
RUN useradd --system --uid 10001 --no-create-home appuser
USER 10001

# ---------------------------------------------------------------------------
# Final images
# ---------------------------------------------------------------------------
FROM runtime-base AS backend
COPY --from=build /src/build/backend_server /usr/local/bin/backend_server
EXPOSE 50051
# Overridden by the k8s manifests; these defaults make `docker run` useful.
ENTRYPOINT ["/usr/local/bin/backend_server"]
CMD ["--port=50051"]

FROM runtime-base AS lb
COPY --from=build /src/build/lb_server /usr/local/bin/lb_server
EXPOSE 50050 9100
ENTRYPOINT ["/usr/local/bin/lb_server"]
CMD ["--port=50050", "--metrics_port=9100"]

# Traffic generator, useful inside compose/k8s for a quick distribution check.
FROM runtime-base AS client
COPY --from=build /src/build/load_client /usr/local/bin/load_client
ENTRYPOINT ["/usr/local/bin/load_client"]
