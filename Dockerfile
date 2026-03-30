FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc libc6-dev make && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY include/ include/
COPY src/ src/
COPY poc/ poc/

RUN cd poc && make

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    netcat-openbsd iproute2 procps && rm -rf /var/lib/apt/lists/*

COPY --from=builder /app/poc/raft_poc /usr/local/bin/raft_poc

ENTRYPOINT ["raft_poc"]
