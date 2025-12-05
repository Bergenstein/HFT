# =============================================================================
# Dockerfile - HFT Crypto Trading System
# =============================================================================
# Multi-stage build for minimal production image
# Supports both development and production modes
# =============================================================================

# Stage 1: Build Stage
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libssl-dev \
    libboost-all-dev \
    libsqlite3-dev \
    libzmq3-dev \
    libprotobuf-dev \
    protobuf-compiler \
    nlohmann-json3-dev \
    wget \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Install cppzmq (header-only)
RUN cd /tmp && \
    git clone https://github.com/zeromq/cppzmq.git && \
    cd cppzmq && \
    mkdir build && cd build && \
    cmake .. && \
    make install

# Install abseil (required by newer protobuf)
RUN cd /tmp && \
    git clone https://github.com/abseil/abseil-cpp.git && \
    cd abseil-cpp && \
    mkdir build && cd build && \
    cmake -DCMAKE_CXX_STANDARD=20 -DABSL_BUILD_TESTING=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON .. && \
    make -j$(nproc) && \
    make install

# Install websocketpp (header-only)
RUN cd /tmp && \
    git clone https://github.com/zaphoyd/websocketpp.git && \
    cd websocketpp && \
    mkdir build && cd build && \
    cmake .. && \
    make install

# Set working directory
WORKDIR /app

# Copy source code
COPY . .

# Create build directory
RUN mkdir -p build

# Build with Linux-specific flags
RUN g++ -std=c++20 -Wall -Wextra -O3 -pthread \
    -I. \
    -I/usr/include \
    -I/usr/local/include \
    -DLINUX_BUILD \
    -c discovery/discovery.cpp -o build/discovery.o

# Build main executables
RUN g++ -std=c++20 -Wall -Wextra -O3 -pthread \
    -I. -I/usr/include -I/usr/local/include -DLINUX_BUILD \
    -c run/stream_and_record.cpp -o build/stream_and_record.o && \
    g++ -std=c++20 -O3 -pthread -o build/stream_and_record \
    build/discovery.o build/stream_and_record.o \
    -lssl -lcrypto -lpthread -lsqlite3

RUN g++ -std=c++20 -Wall -Wextra -O3 -pthread \
    -I. -I/usr/include -I/usr/local/include -DLINUX_BUILD \
    -c run/backtest_strategy.cpp -o build/backtest_strategy.o && \
    g++ -std=c++20 -O3 -pthread -o build/backtest_strategy \
    build/discovery.o build/backtest_strategy.o \
    -lssl -lcrypto -lpthread -lsqlite3

RUN g++ -std=c++20 -Wall -Wextra -O3 -pthread \
    -I. -I/usr/include -I/usr/local/include -DLINUX_BUILD \
    -c run/scan_recording.cpp -o build/scan_recording.o && \
    g++ -std=c++20 -O3 -pthread -o build/scan_recording \
    build/scan_recording.o

RUN g++ -std=c++20 -Wall -Wextra -O3 -pthread \
    -I. -I/usr/include -I/usr/local/include -DLINUX_BUILD \
    -c run/replay_and_book.cpp -o build/replay_and_book.o && \
    g++ -std=c++20 -O3 -pthread -o build/replay_and_book \
    build/discovery.o build/replay_and_book.o \
    -lssl -lcrypto -lpthread

# Build lock-free queue tests
RUN g++ -std=c++17 -O3 -march=x86-64 -pthread \
    -o build/test_lockfree_queues run/test_lockfree_queues.cpp

# Build arbitrage demo
RUN g++ -std=c++17 -O3 -march=x86-64 \
    -I. -I/usr/include -I/usr/local/include \
    -o build/test_arbitrage_demo run/test_arbitrage_demo.cpp

# Build matching engine test
RUN g++ -std=c++20 -O2 \
    -I. -I/usr/include -I/usr/local/include \
    -o build/test_matching_engine_simple run/test_matching_engine_simple.cpp

# Build SQLite backtester
RUN g++ -std=c++20 -Wall -Wextra -O3 -pthread \
    -I. -I/usr/include -I/usr/local/include -DLINUX_BUILD \
    -c run/backtest_from_sqlite.cpp -o build/backtest_from_sqlite.o && \
    g++ -std=c++20 -O3 -pthread -o build/backtest_from_sqlite \
    build/discovery.o build/backtest_from_sqlite.o \
    -lssl -lcrypto -lpthread -lsqlite3

# =============================================================================
# Stage 2: Production Runtime
# =============================================================================
FROM ubuntu:22.04 AS production

ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies only
RUN apt-get update && apt-get install -y \
    libssl3 \
    libboost-system1.74.0 \
    libsqlite3-0 \
    libzmq5 \
    libprotobuf23 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user for security
RUN useradd -m -s /bin/bash hft

# Set working directory
WORKDIR /app

# Copy binaries from builder
COPY --from=builder /app/build/stream_and_record /app/bin/
COPY --from=builder /app/build/backtest_strategy /app/bin/
COPY --from=builder /app/build/scan_recording /app/bin/
COPY --from=builder /app/build/replay_and_book /app/bin/
COPY --from=builder /app/build/test_lockfree_queues /app/bin/
COPY --from=builder /app/build/test_arbitrage_demo /app/bin/
COPY --from=builder /app/build/test_matching_engine_simple /app/bin/
COPY --from=builder /app/build/backtest_from_sqlite /app/bin/

# Copy configuration
COPY config/ /app/config/
COPY storage/sqlite/schema.sql /app/storage/sqlite/

# Create data directories
RUN mkdir -p /app/data /app/logs && \
    chown -R hft:hft /app

# Switch to non-root user
USER hft

# Add binaries to PATH
ENV PATH="/app/bin:${PATH}"

# Default command
CMD ["echo", "HFT System Ready. Use: docker run <image> <binary_name> [args]"]

# =============================================================================
# Stage 3: Development (includes all tools)
# =============================================================================
FROM builder AS development

# Keep all build tools and sources for development
WORKDIR /app

# Install additional dev tools
RUN apt-get update && apt-get install -y \
    gdb \
    valgrind \
    htop \
    vim \
    && rm -rf /var/lib/apt/lists/*

CMD ["/bin/bash"]
