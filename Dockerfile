FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    libcurl4-openssl-dev \
    libevent-dev \
    libpq-dev \
    libssl-dev \
    pkg-config \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
RUN cmake --build build --target cloud-server -j 2

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libcurl4 \
    libevent-2.1-7 \
    libpq5 \
    libssl3 \
    zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/cloud-server /usr/local/bin/cloud-server
EXPOSE 8080
ENTRYPOINT ["cloud-server"]
CMD ["--migrate-and-serve"]
