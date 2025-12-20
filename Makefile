SHELL := /bin/bash
CXX       ?= c++
CXXFLAGS  := -std=c++20 -Wall -Wextra -O2 -pthread

# Define library prefixes with fallback paths
OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null || echo /opt/homebrew/opt/openssl@3)
BOOST_PREFIX   := $(shell brew --prefix boost 2>/dev/null || echo /opt/homebrew/opt/boost)
JSON_PREFIX    := $(shell brew --prefix nlohmann-json 2>/dev/null || echo /opt/homebrew/opt/nlohmann-json)
ZMQ_PREFIX     := $(shell brew --prefix zeromq 2>/dev/null || echo /opt/homebrew/opt/zeromq)
CPPZMQ_PREFIX  := $(shell brew --prefix cppzmq 2>/dev/null || echo /opt/homebrew/opt/cppzmq)
PROTOBUF_PREFIX := $(shell brew --prefix protobuf 2>/dev/null || echo /opt/homebrew/opt/protobuf)
ABSEIL_PREFIX  := $(shell brew --prefix abseil 2>/dev/null || echo /opt/homebrew/opt/abseil)
WEBSOCKETPP_PREFIX := $(shell brew --prefix websocketpp 2>/dev/null || echo /opt/homebrew/opt/websocketpp)

# Check if libraries exist
ifeq (,$(wildcard $(OPENSSL_PREFIX)/lib/libssl.a))
    $(error OpenSSL not found at $(OPENSSL_PREFIX))
endif
ifeq (,$(wildcard $(BOOST_PREFIX)/include/boost))
    $(error Boost not found at $(BOOST_PREFIX))
endif
ifeq (,$(wildcard $(JSON_PREFIX)/include/nlohmann))
    $(error nlohmann-json not found at $(JSON_PREFIX))
endif

INCLUDES := -I. -I$(BOOST_PREFIX)/include -I$(OPENSSL_PREFIX)/include -I$(JSON_PREFIX)/include \
            -I$(ZMQ_PREFIX)/include -I$(CPPZMQ_PREFIX)/include -I$(PROTOBUF_PREFIX)/include \
            -I$(ABSEIL_PREFIX)/include -I$(WEBSOCKETPP_PREFIX)/include

# Abseil libraries (required by protobuf)
ABSEIL_LIBS := -labsl_log_internal_check_op -labsl_log_internal_conditions \
               -labsl_log_internal_format -labsl_log_internal_globals \
               -labsl_log_internal_log_sink_set -labsl_log_internal_message \
               -labsl_log_severity -labsl_log_entry -labsl_log_globals \
               -labsl_hash -labsl_city -labsl_raw_hash_set -labsl_hashtablez_sampler \
               -labsl_exponential_biased \
               -labsl_strings -labsl_strings_internal -labsl_str_format_internal \
               -labsl_cord -labsl_cordz_info -labsl_cord_internal \
               -labsl_cordz_functions -labsl_cordz_handle \
               -labsl_crc_cord_state -labsl_crc32c -labsl_crc_internal -labsl_crc_cpu_detect \
               -labsl_synchronization -labsl_graphcycles_internal -labsl_kernel_timeout_internal \
               -labsl_time -labsl_civil_time -labsl_time_zone \
               -labsl_malloc_internal -labsl_base -labsl_spinlock_wait \
               -labsl_int128 -labsl_throw_delegate -labsl_stacktrace -labsl_symbolize \
               -labsl_debugging_internal -labsl_demangle_internal -labsl_raw_logging_internal

LIBS     := -L$(OPENSSL_PREFIX)/lib -L$(ZMQ_PREFIX)/lib -L$(PROTOBUF_PREFIX)/lib -L$(ABSEIL_PREFIX)/lib \
            -lssl -lcrypto -lpthread -lzmq -lprotobuf -lcurl $(ABSEIL_LIBS)

BUILDDIR := build
SRCDIRS  := discovery md net io core bt

COMMON_SRCS := $(foreach d,$(SRCDIRS),$(wildcard $(d)/*.cpp))
COMMON_OBJS := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(COMMON_SRCS))

# Main programs
STREAM_MAIN    := run/stream_and_record.cpp
REPLAY_MAIN    := run/replay_and_book.cpp
BACKTEST_MAIN  := run/backtest_strategy.cpp
SCAN_MAIN      := run/scan_recording.cpp
STREAM_LAT_MAIN := run/stream_with_latency.cpp
# TODO Item #5 - New executables
MULTI_EXCHANGE_MAIN := run/complete_multi_exchange_pipeline.cpp
LIVE_STRATEGY_MAIN  := run/live_strategy_runner.cpp
TEST_MULTI_EXCHANGE_SIMPLE_MAIN := run/test_multi_exchange_simple.cpp
# Production HFT System
PRODUCTION_HFT_MAIN := run/production_hft_system.cpp
# Hot/Cold Integration Test
TEST_HOT_COLD_MAIN := run/test_hot_cold_integration.cpp
# Full System Integration (Multi-Exchange)
FULL_SYSTEM_MAIN := run/full_system_integration.cpp

STREAM_OBJ     := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(STREAM_MAIN))
REPLAY_OBJ     := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(REPLAY_MAIN))
BACKTEST_OBJ   := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(BACKTEST_MAIN))
SCAN_OBJ       := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(SCAN_MAIN))
STREAM_LAT_OBJ := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(STREAM_LAT_MAIN))
# TODO Item #5 - Object files
MULTI_EXCHANGE_OBJ := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(MULTI_EXCHANGE_MAIN))
LIVE_STRATEGY_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(LIVE_STRATEGY_MAIN))
TEST_MULTI_EXCHANGE_SIMPLE_OBJ := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(TEST_MULTI_EXCHANGE_SIMPLE_MAIN))
# Production HFT
PRODUCTION_HFT_OBJ := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(PRODUCTION_HFT_MAIN))
# Integration Test
TEST_HOT_COLD_OBJ := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(TEST_HOT_COLD_MAIN))
# Full System Integration
FULL_SYSTEM_OBJ := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(FULL_SYSTEM_MAIN))
# Test Coinbase to Queue
TEST_COINBASE_QUEUE_MAIN := run/test_coinbase_to_queue.cpp
TEST_COINBASE_QUEUE_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(TEST_COINBASE_QUEUE_MAIN))
TEST_COINBASE_QUEUE_BIN  := $(BUILDDIR)/test_coinbase_to_queue

STREAM_BIN     := $(BUILDDIR)/stream_and_record
REPLAY_BIN     := $(BUILDDIR)/replay_and_book
BACKTEST_BIN   := $(BUILDDIR)/backtest_strategy
SCAN_BIN       := $(BUILDDIR)/scan_recording
STREAM_LAT_BIN := $(BUILDDIR)/stream_with_latency
# TODO Item #5 - Binaries
MULTI_EXCHANGE_BIN := $(BUILDDIR)/complete_multi_exchange_pipeline
LIVE_STRATEGY_BIN  := $(BUILDDIR)/live_strategy_runner
# Production HFT System
PRODUCTION_HFT_BIN := $(BUILDDIR)/production_hft_system
# Integration Test
TEST_HOT_COLD_BIN := $(BUILDDIR)/test_hot_cold_integration
# Full System Integration
FULL_SYSTEM_BIN := $(BUILDDIR)/full_system_integration

# Results directory
RESULTS_DIR    := test_results_$(shell date +%Y%m%d_%H%M%S)

.PHONY: all clean
all: $(STREAM_BIN) $(STREAM_LAT_BIN) $(REPLAY_BIN) $(BACKTEST_BIN) $(SCAN_BIN) $(MULTI_EXCHANGE_BIN) $(LIVE_STRATEGY_BIN) $(TEST_HOT_COLD_BIN) $(FULL_SYSTEM_BIN) $(BACKTEST_TO_JSON_BIN)

$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Compile proto files
$(BUILDDIR)/proto/%.pb.o: proto/%.pb.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Compile test_scripts files
$(BUILDDIR)/test_scripts/%.o: test_scripts/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(STREAM_BIN): $(COMMON_OBJS) $(STREAM_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(STREAM_LAT_BIN): $(COMMON_OBJS) $(BUILDDIR)/run/stream_with_latency.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(REPLAY_BIN): $(COMMON_OBJS) $(REPLAY_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(BACKTEST_BIN): $(COMMON_OBJS) $(BACKTEST_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# SQLite Backtest (uses REAL historical data)
BACKTEST_SQLITE_MAIN := run/backtest_from_sqlite.cpp
BACKTEST_SQLITE_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(BACKTEST_SQLITE_MAIN))
BACKTEST_SQLITE_BIN  := $(BUILDDIR)/backtest_from_sqlite

$(BACKTEST_SQLITE_BIN): $(COMMON_OBJS) $(BACKTEST_SQLITE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3

# Funding Rate Arbitrage Runner
FUNDING_ARB_MAIN := run/funding_rate_arb_runner.cpp
FUNDING_ARB_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(FUNDING_ARB_MAIN))
FUNDING_ARB_BIN  := $(BUILDDIR)/funding_rate_arb_runner

$(FUNDING_ARB_BIN): $(COMMON_OBJS) $(FUNDING_ARB_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3
	@echo "✓ Built funding_rate_arb_runner"

$(SCAN_BIN): $(SCAN_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

# TODO Item #5 - Build targets
$(MULTI_EXCHANGE_BIN): $(COMMON_OBJS) $(MULTI_EXCHANGE_OBJ) $(BUILDDIR)/proto/messages.pb.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3
	@echo "✓ Built complete_multi_exchange_pipeline"

$(LIVE_STRATEGY_BIN): $(LIVE_STRATEGY_OBJ) $(BUILDDIR)/proto/messages.pb.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)
	@echo "✓ Built live_strategy_runner"

# Production HFT System
$(PRODUCTION_HFT_BIN): $(COMMON_OBJS) $(PRODUCTION_HFT_OBJ) $(BUILDDIR)/proto/messages.pb.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3
	@echo "✓ Built production_hft_system"

# Integration Test
$(TEST_HOT_COLD_BIN): $(COMMON_OBJS) $(TEST_HOT_COLD_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3
	@echo "✓ Built test_hot_cold_integration"

# Full System Integration
$(FULL_SYSTEM_BIN): $(COMMON_OBJS) $(FULL_SYSTEM_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3
	@echo "✓ Built full_system_integration"

# Real Funding Rate Arbitrage (NO SIMULATION - uses REAL exchange APIs)
REAL_FUNDING_ARB_MAIN := run/real_funding_rate_arb.cpp
REAL_FUNDING_ARB_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(REAL_FUNDING_ARB_MAIN))
REAL_FUNDING_ARB_BIN  := $(BUILDDIR)/real_funding_rate_arb

$(REAL_FUNDING_ARB_BIN): $(COMMON_OBJS) $(REAL_FUNDING_ARB_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3
	@echo "✓ Built real_funding_rate_arb (REAL exchange API data)"

# Backtest with REAL historical funding rates
BACKTEST_REAL_FUNDING_MAIN := run/backtest_real_funding.cpp
BACKTEST_REAL_FUNDING_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(BACKTEST_REAL_FUNDING_MAIN))
BACKTEST_REAL_FUNDING_BIN  := $(BUILDDIR)/backtest_real_funding

$(BACKTEST_REAL_FUNDING_BIN): $(COMMON_OBJS) $(BACKTEST_REAL_FUNDING_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3
	@echo "✓ Built backtest_real_funding (REAL historical data)"

# Dashboard Server (ZMQ metrics publisher for cold path)
DASHBOARD_SERVER_MAIN := run/dashboard_server.cpp
DASHBOARD_SERVER_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(DASHBOARD_SERVER_MAIN))
DASHBOARD_SERVER_BIN  := $(BUILDDIR)/dashboard_server

$(DASHBOARD_SERVER_BIN): $(COMMON_OBJS) $(DASHBOARD_SERVER_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3
	@echo "✓ Built dashboard_server (ZMQ cold path metrics)"

# REAL Dashboard Server (NO SIMULATION - uses actual exchange data)
REAL_DASHBOARD_SERVER_MAIN := run/real_dashboard_server.cpp
REAL_DASHBOARD_SERVER_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(REAL_DASHBOARD_SERVER_MAIN))
REAL_DASHBOARD_SERVER_BIN  := $(BUILDDIR)/real_dashboard_server

$(REAL_DASHBOARD_SERVER_BIN): $(COMMON_OBJS) $(REAL_DASHBOARD_SERVER_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3
	@echo "✓ Built real_dashboard_server (REAL exchange data, NO SIMULATION)"

# Backtest to JSON (PRODUCTION - runs ONCE, outputs JSON for static dashboard)
BACKTEST_TO_JSON_MAIN := run/backtest_to_json.cpp
BACKTEST_TO_JSON_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(BACKTEST_TO_JSON_MAIN))
BACKTEST_TO_JSON_BIN  := $(BUILDDIR)/backtest_to_json

$(BACKTEST_TO_JSON_BIN): $(COMMON_OBJS) $(BACKTEST_TO_JSON_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3
	@echo "✓ Built backtest_to_json (PRODUCTION - outputs JSON)"

# Multi-Exchange Hot/Cold Test
MULTI_HOT_COLD_MAIN := run/test_multi_exchange_hot_cold.cpp
MULTI_HOT_COLD_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(MULTI_HOT_COLD_MAIN))
MULTI_HOT_COLD_BIN  := $(BUILDDIR)/test_multi_exchange_hot_cold

$(MULTI_HOT_COLD_BIN): $(COMMON_OBJS) $(MULTI_HOT_COLD_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3
	@echo "✓ Built test_multi_exchange_hot_cold"

# Test Coinbase to Queue
$(TEST_COINBASE_QUEUE_BIN): $(TEST_COINBASE_QUEUE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)
	@echo "✓ Built test_coinbase_to_queue"

# Individual build targets
.PHONY: stream replay backtest scan multi_exchange live_strategy production test_integration full_system
stream:   $(STREAM_BIN)
replay:   $(REPLAY_BIN)
backtest: $(BACKTEST_BIN)
scan:     $(SCAN_BIN)
# TODO Item #5 targets
multi_exchange: $(MULTI_EXCHANGE_BIN)
live_strategy:  $(LIVE_STRATEGY_BIN)
# Production target
production: $(PRODUCTION_HFT_BIN)
# Integration test target
test_integration: $(TEST_HOT_COLD_BIN)
# Full system target
full_system: $(FULL_SYSTEM_BIN)

# ==========================================
# AUTOMATED STRATEGY TESTING
# ==========================================

# Find latest data file and most active product
LATEST_DATA = $(shell ls -t data/*.ndjson 2>/dev/null | head -n1)
ACTIVE_PROD = $(shell if [ -f "$(LATEST_DATA)" ]; then ./$(SCAN_BIN) "$(LATEST_DATA)" 2>/dev/null | grep -A1 "Top products" | tail -1 | awk '{print $$1}'; fi)

# Test parameters
QTY := 1.0
CAP := 50000

# Helper function to run backtest
define run_backtest
	@echo "Testing $(1) with params: $(2), $(3)"
	@mkdir -p $(RESULTS_DIR)
	@$(BACKTEST_BIN) "$(LATEST_DATA)" "$(1)" "$(ACTIVE_PROD)" "$(QTY)" "$(2)" "$(3)" "$(CAP)" > $(RESULTS_DIR)/$(1)_p1_$(2)_p2_$(3).txt 2>&1 || true
	@grep -A5 "=== Results" $(RESULTS_DIR)/$(1)_p1_$(2)_p2_$(3).txt | head -10 || echo "  No results"
endef

# Test individual strategies
.PHONY: test_imbalance test_ofi test_microprice test_quote_intensity test_spread_reversion test_vpin test_vw_spread

test_imbalance: $(BACKTEST_BIN)
	@echo "========================================="
	@echo "Testing Imbalance Strategy"
	@echo "========================================="
	$(call run_backtest,imbalance,0.3,5.0)
	$(call run_backtest,imbalance,0.5,5.0)
	$(call run_backtest,imbalance,0.7,5.0)
	$(call run_backtest,imbalance,0.9,5.0)

test_ofi: $(BACKTEST_BIN)
	@echo "========================================="
	@echo "Testing OFI Strategy"
	@echo "========================================="
	$(call run_backtest,ofi,0.10,0.05)
	$(call run_backtest,ofi,0.15,0.07)
	$(call run_backtest,ofi,0.20,0.10)

test_microprice: $(BACKTEST_BIN)
	@echo "========================================="
	@echo "Testing Microprice Strategy"
	@echo "========================================="
	$(call run_backtest,microprice,0.0005,5.0)
	$(call run_backtest,microprice,0.001,5.0)
	$(call run_backtest,microprice,0.002,5.0)

test_quote_intensity: $(BACKTEST_BIN)
	@echo "========================================="
	@echo "Testing Quote Intensity Strategy"
	@echo "========================================="
	$(call run_backtest,quote_intensity,0.3,3.0)
	$(call run_backtest,quote_intensity,0.4,3.0)
	$(call run_backtest,quote_intensity,0.5,3.0)

test_spread_reversion: $(BACKTEST_BIN)
	@echo "========================================="
	@echo "Testing Spread Reversion Strategy"
	@echo "========================================="
	$(call run_backtest,spread_reversion,1.3,5.0)
	$(call run_backtest,spread_reversion,1.5,5.0)
	$(call run_backtest,spread_reversion,2.0,5.0)

test_vpin: $(BACKTEST_BIN)
	@echo "========================================="
	@echo "Testing VPIN Strategy"
	@echo "========================================="
	$(call run_backtest,vpin,0.2,5.0)
	$(call run_backtest,vpin,0.3,5.0)
	$(call run_backtest,vpin,0.4,5.0)

test_vw_spread: $(BACKTEST_BIN)
	@echo "========================================="
	@echo "Testing Volume Weighted Spread Strategy"
	@echo "========================================="
	$(call run_backtest,vw_spread,0.001,4.0)
	$(call run_backtest,vw_spread,0.002,4.0)
	$(call run_backtest,vw_spread,0.003,4.0)

# Test all strategies
.PHONY: test_all_strategies
test_all_strategies: $(BACKTEST_BIN) $(SCAN_BIN)
	@if [ -z "$(LATEST_DATA)" ]; then \
		echo "Error: No data files found in data/"; \
		echo "Run 'make stream' first to collect data."; \
		exit 1; \
	fi
	@echo "=========================================="
	@echo "  AUTOMATED STRATEGY TEST SUITE"
	@echo "=========================================="
	@echo "Data: $(LATEST_DATA)"
	@echo "Product: $(ACTIVE_PROD)"
	@echo "Results will be saved to: $(RESULTS_DIR)"
	@echo ""
	@$(MAKE) test_imbalance RESULTS_DIR=$(RESULTS_DIR)
	@$(MAKE) test_ofi RESULTS_DIR=$(RESULTS_DIR)
	@$(MAKE) test_microprice RESULTS_DIR=$(RESULTS_DIR)
	@$(MAKE) test_quote_intensity RESULTS_DIR=$(RESULTS_DIR)
	@$(MAKE) test_spread_reversion RESULTS_DIR=$(RESULTS_DIR)
	@$(MAKE) test_vpin RESULTS_DIR=$(RESULTS_DIR)
	@$(MAKE) test_vw_spread RESULTS_DIR=$(RESULTS_DIR)
	@echo ""
	@echo "=========================================="
	@echo "  TEST SUITE COMPLETE!"
	@echo "=========================================="
	@echo ""
	@$(MAKE) generate_report RESULTS_DIR=$(RESULTS_DIR)

# Generate summary report
.PHONY: generate_report
generate_report:
	@echo "Generating summary report..."
	@echo "# Strategy Test Results" > $(RESULTS_DIR)/SUMMARY.md
	@echo "" >> $(RESULTS_DIR)/SUMMARY.md
	@echo "**Date:** $$(date)" >> $(RESULTS_DIR)/SUMMARY.md
	@echo "**Data:** $(LATEST_DATA)" >> $(RESULTS_DIR)/SUMMARY.md
	@echo "**Product:** $(ACTIVE_PROD)" >> $(RESULTS_DIR)/SUMMARY.md
	@echo "" >> $(RESULTS_DIR)/SUMMARY.md
	@echo "## Results" >> $(RESULTS_DIR)/SUMMARY.md
	@echo "" >> $(RESULTS_DIR)/SUMMARY.md
	@echo "| Strategy | Params | Trades | Return | Sharpe | Max DD | PnL |" >> $(RESULTS_DIR)/SUMMARY.md
	@echo "|----------|--------|--------|--------|--------|--------|-----|" >> $(RESULTS_DIR)/SUMMARY.md
	@for f in $(RESULTS_DIR)/*.txt; do \
		if [ -f "$$f" ]; then \
			name=$$(basename "$$f" .txt); \
			trades=$$(grep "^Trades" "$$f" | awk '{print $$3}' || echo "0"); \
			ret=$$(grep "^Total Return" "$$f" | awk '{print $$4}' || echo "N/A"); \
			sharpe=$$(grep "^Sharpe (annualized)" "$$f" | awk '{print $$4}' || echo "N/A"); \
			dd=$$(grep "^Max Drawdown" "$$f" | awk '{print $$4}' || echo "N/A"); \
			pnl=$$(grep "^Net PnL" "$$f" | awk '{print $$4}' || echo "N/A"); \
			echo "| $$name | - | $$trades | $$ret | $$sharpe | $$dd | $$pnl |" >> $(RESULTS_DIR)/SUMMARY.md; \
		fi; \
	done
	@echo "" >> $(RESULTS_DIR)/SUMMARY.md
	@echo "Results saved to: $(RESULTS_DIR)/SUMMARY.md"
	@cat $(RESULTS_DIR)/SUMMARY.md

# Quick test with latest data
.PHONY: quick_test
quick_test: $(BACKTEST_BIN) $(SCAN_BIN)
	@if [ -z "$(LATEST_DATA)" ]; then \
		echo "Error: No data files found"; \
		exit 1; \
	fi
	@echo "Quick test on $(ACTIVE_PROD)..."
	@$(BACKTEST_BIN) "$(LATEST_DATA)" "imbalance" "$(ACTIVE_PROD)" "1.0" "0.6" "5.0" "50000"

# Clean
clean:
	rm -rf $(BUILDDIR) test_results_*

.PHONY: help
help:
	@echo "Coin_base_HFT Makefile"
	@echo ""
	@echo "Build targets:"
	@echo "  make all                  - Build all binaries"
	@echo "  make stream               - Build stream_and_record"
	@echo "  make replay               - Build replay_and_book"
	@echo "  make backtest             - Build backtest_strategy"
	@echo "  make scan                 - Build scan_recording"
	@echo ""
	@echo "Testing targets:"
	@echo "  make test_all_strategies  - Test all strategies with parameter sweeps"
	@echo "  make test_imbalance       - Test imbalance strategy only"
	@echo "  make test_ofi             - Test OFI strategy only"
	@echo "  make test_microprice      - Test microprice strategy only"
	@echo "  make test_quote_intensity - Test quote intensity strategy"
	@echo "  make test_spread_reversion- Test spread reversion strategy"
	@echo "  make test_vpin            - Test VPIN strategy"
	@echo "  make test_vw_spread       - Test volume-weighted spread strategy"
	@echo "  make quick_test           - Quick test with default parameters"
	@echo ""
	@echo "  make clean                - Remove build artifacts"
	@echo "  make help                 - Show this help"

# Latency test binary
$(BUILDDIR)/test_latency: $(BUILDDIR)/run/test_latency.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(BUILDDIR)/test_json_only: $(BUILDDIR)/run/test_json_only.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# System latency measurement
$(BUILDDIR)/measure_system_latency: $(BUILDDIR)/run/measure_system_latency.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# System latency test binaries
$(BUILDDIR)/test_system_latency: $(BUILDDIR)/run/test_system_latency.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

$(BUILDDIR)/test_processing_latency: $(BUILDDIR)/run/test_processing_latency.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# Complete latency test
$(BUILDDIR)/complete_latency_test: $(BUILDDIR)/run/complete_latency_test.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# Arbitrage system
ARB_TEST_MAIN := run/test_arbitrage.cpp
ARB_TEST_BIN  := $(BUILDDIR)/test_arbitrage

.PHONY: test_arb arb_test
test_arb arb_test: $(ARB_TEST_BIN)

$(ARB_TEST_BIN): $(ARB_TEST_MAIN)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built arbitrage test: $@"

ARB_DEMO_MAIN := run/test_arbitrage_demo.cpp
ARB_DEMO_BIN  := $(BUILDDIR)/test_arbitrage_demo

.PHONY: test_arb_demo arb_demo
test_arb_demo arb_demo: $(ARB_DEMO_BIN)

$(ARB_DEMO_BIN): $(ARB_DEMO_MAIN)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built arbitrage demo: $@"

# ============================================================================
# LOCK-FREE QUEUE TESTS
# ============================================================================
LOCKFREE_TEST := run/test_lockfree_queues.cpp
LOCKFREE_BIN := $(BUILDDIR)/test_lockfree_queues

.PHONY: test_lockfree_queues lockfree_queues
test_lockfree_queues lockfree_queues: $(LOCKFREE_BIN)

$(LOCKFREE_BIN): $(LOCKFREE_TEST)
	@mkdir -p $(BUILDDIR)
	$(CXX) -std=c++17 -O3 -march=native -pthread -o $@ $<
	@echo "✓ Built lock-free queue tests"

# ============================================================================
# ARBITRAGE SYSTEM TESTS
# ============================================================================
ARB_TEST := run/test_arbitrage_demo.cpp
ARB_BIN := $(BUILDDIR)/test_arbitrage_demo

.PHONY: test_arbitrage_demo arb_demo
test_arbitrage_demo arb_demo: $(ARB_BIN)

$(ARB_BIN): $(ARB_TEST)
	@mkdir -p $(BUILDDIR)
	$(CXX) -std=c++17 -O3 -march=native -o $@ $<
	@echo "✓ Built arbitrage demo"

# ============================================================================
# COMPREHENSIVE TEST SUITE
# ============================================================================
.PHONY: test_all test_suite full_test
test_all test_suite full_test:
	@echo "======================================================================"
	@echo "  COMPREHENSIVE HFT SYSTEM TEST SUITE"
	@echo "======================================================================"
	@echo ""
	@echo "Phase 1: Core Infrastructure Tests"
	@echo "----------------------------------------------------------------------"
	@make test_lockfree_queues && ./build/test_lockfree_queues
	@echo ""
	@echo "Phase 2: Arbitrage System Tests"
	@echo "----------------------------------------------------------------------"
	@make test_arbitrage_demo && ./build/test_arbitrage_demo
	@echo ""
	@echo "Phase 3: Strategy Backtests"
	@echo "----------------------------------------------------------------------"
	@make test_all_strategies
	@echo ""
	@echo "======================================================================"
	@echo "  ALL TESTS COMPLETED"
	@echo "======================================================================"
	@echo "View results:"
	@echo "  - Lock-free queues: STDOUT above"
	@echo "  - Arbitrage: STDOUT above"
	@echo "  - Strategies: test_results_*/SUMMARY.md"
	@echo ""

# ============================================================================
# PERFORMANCE BENCHMARKS
# ============================================================================
.PHONY: benchmark bench
benchmark bench: test_lockfree_queues
	@echo "Running performance benchmarks..."
	@./build/test_lockfree_queues
	@echo "Benchmark complete!"

# ============================================================================
# HELP
# ============================================================================
.PHONY: help_tests
help_tests:
	@echo "HFT System Test Targets:"
	@echo ""
	@echo "  Lock-Free Queue Tests:"
	@echo "    make test_lockfree_queues   - Test SPSC/MPMC queues"
	@echo ""
	@echo "  Arbitrage Tests:"
	@echo "    make test_arbitrage_demo    - Test cross-exchange arbitrage"
	@echo ""
	@echo "  Strategy Tests:"
	@echo "    make test_all_strategies    - Backtest all 7 strategies"
	@echo "    make test_imbalance         - Test imbalance strategy"
	@echo "    make test_ofi               - Test OFI strategy"
	@echo ""
	@echo "  Full Suite:"
	@echo "    make test_all               - Run complete test suite"
	@echo "    make benchmark              - Run performance benchmarks"
	@echo ""
	@echo "  Reports:"
	@echo "    Results saved to: reports/ and test_results_*/"
	@echo ""


# Integrated Pipeline Demo
PIPELINE_DEMO := run/integrated_pipeline_demo.cpp
PIPELINE_BIN := $(BUILDDIR)/integrated_pipeline_demo

.PHONY: integrated_pipeline pipeline_demo
integrated_pipeline pipeline_demo: $(PIPELINE_BIN)

$(PIPELINE_BIN): $(PIPELINE_DEMO)
	@mkdir -p $(BUILDDIR)
	$(CXX) -std=c++20 -O2 -pthread -I. -o $@ $< -lsqlite3
	@echo "✓ Built integrated pipeline demo"

.PHONY: run_pipeline
run_pipeline: $(PIPELINE_BIN)
	@echo "Running integrated multi-exchange pipeline..."
	@./$(PIPELINE_BIN)

# Production Multi-Exchange Pipeline (Step 5)
PROD_MULTI_EXCHANGE_SRC := run/production_multi_exchange.cpp
PROD_MULTI_EXCHANGE_BIN := $(BUILDDIR)/production_multi_exchange

COMMON_DEPS_MULTIEX := discovery/discovery.cpp

.PHONY: production_multi_exchange prod_pipeline
production_multi_exchange prod_pipeline: $(PROD_MULTI_EXCHANGE_BIN)

$(PROD_MULTI_EXCHANGE_BIN): $(PROD_MULTI_EXCHANGE_SRC) $(COMMON_DEPS_MULTIEX)
	@mkdir -p $(BUILDDIR) $(BUILDDIR)/discovery
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LIBS) -lsqlite3 -lzmq -lprotobuf
	@echo "✓ Built production multi-exchange pipeline"

.PHONY: run_prod_pipeline
run_prod_pipeline: $(PROD_MULTI_EXCHANGE_BIN)
	@echo "Running PRODUCTION multi-exchange pipeline (Step 5)..."
	@./$(PROD_MULTI_EXCHANGE_BIN)

# ============================================================================
# PRODUCTION MULTI-EXCHANGE PIPELINE (Step 5)
# ============================================================================
PROD_MULTI_SRC := run/production_multi_exchange.cpp
PROD_MULTI_BIN := $(BUILDDIR)/production_multi_exchange

$(PROD_MULTI_BIN): $(PROD_MULTI_SRC)
	@mkdir -p $(BUILDDIR)
	@echo "Building production multi-exchange pipeline..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I/opt/homebrew/opt/websocketpp/include \
	    -o $@ $< $(LIBS) -lsqlite3 -lzmq -lprotobuf -lpthread
	@echo "✓ Built production_multi_exchange"

.PHONY: production_pipeline prod step5
production_pipeline prod step5: $(PROD_MULTI_BIN)

.PHONY: run_production run_prod
run_production run_prod: $(PROD_MULTI_BIN)
	@echo "Starting production multi-exchange pipeline..."
	@./$(PROD_MULTI_BIN)

# ============================================================================
# ZMQ PUB/SUB TEST
# ============================================================================
ZMQ_PUBSUB_SRC := run/test_zmq_pubsub.cpp
ZMQ_PUBSUB_BIN := $(BUILDDIR)/test_zmq_pubsub

.PHONY: test_zmq_pubsub zmq_pubsub
test_zmq_pubsub zmq_pubsub: $(ZMQ_PUBSUB_BIN)

$(ZMQ_PUBSUB_BIN): $(ZMQ_PUBSUB_SRC) $(BUILDDIR)/proto/messages.pb.o
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LIBS)
	@echo "✓ Built ZMQ pub/sub test"

# ============================================================================
# MATCHING ENGINE TEST  
# ============================================================================
MATCHING_SIMPLE_SRC := run/test_matching_engine_simple.cpp
MATCHING_SIMPLE_BIN := $(BUILDDIR)/test_matching_engine_simple

.PHONY: test_matching_engine_simple matching_simple
test_matching_engine_simple matching_simple: $(MATCHING_SIMPLE_BIN)

$(MATCHING_SIMPLE_BIN): $(MATCHING_SIMPLE_SRC)
	@mkdir -p $(BUILDDIR)
	$(CXX) -std=c++20 -O2 $(INCLUDES) -o $@ $<
	@echo "✓ Built matching engine simple test"

# ============================================================================
# MULTI-EXCHANGE FUNDING RATE FETCHER
# ============================================================================

# Test multi-exchange funding fetcher
TEST_MULTI_EXCHANGE_FUNDING_MAIN := run/test_multi_exchange_funding.cpp
TEST_MULTI_EXCHANGE_FUNDING_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(TEST_MULTI_EXCHANGE_FUNDING_MAIN))
TEST_MULTI_EXCHANGE_FUNDING_BIN  := $(BUILDDIR)/test_multi_exchange_funding

$(TEST_MULTI_EXCHANGE_FUNDING_BIN): $(TEST_MULTI_EXCHANGE_FUNDING_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)
	@echo "✓ Built test_multi_exchange_funding"

.PHONY: test_funding_fetcher
test_funding_fetcher: $(TEST_MULTI_EXCHANGE_FUNDING_BIN)

# ============================================================================
# MULTI-EXCHANGE L2 + FUNDING FETCHER
# ============================================================================

TEST_MULTI_EXCHANGE_L2_MAIN := test_scripts/test_multi_exchange_l2.cpp
TEST_MULTI_EXCHANGE_L2_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(TEST_MULTI_EXCHANGE_L2_MAIN))
TEST_MULTI_EXCHANGE_L2_BIN  := $(BUILDDIR)/test_multi_exchange_l2

$(TEST_MULTI_EXCHANGE_L2_BIN): $(TEST_MULTI_EXCHANGE_L2_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)
	@echo "✓ Built test_multi_exchange_l2"

.PHONY: test_multi_exchange_l2
test_multi_exchange_l2: $(TEST_MULTI_EXCHANGE_L2_BIN)

# Individual exchange test targets
.PHONY: test_binance test_bybit test_okx test_gateio test_mexc test_kucoin test_kraken test_bitget test_htx test_bingx
test_binance: $(TEST_MULTI_EXCHANGE_L2_BIN)
	@echo "=== Testing Binance Exchange ==="
	@$(TEST_MULTI_EXCHANGE_L2_BIN) binance

test_bybit: $(TEST_MULTI_EXCHANGE_L2_BIN)
	@echo "=== Testing Bybit Exchange ==="
	@$(TEST_MULTI_EXCHANGE_L2_BIN) bybit

test_okx: $(TEST_MULTI_EXCHANGE_L2_BIN)
	@echo "=== Testing OKX Exchange ==="
	@$(TEST_MULTI_EXCHANGE_L2_BIN) okx

test_gateio: $(TEST_MULTI_EXCHANGE_L2_BIN)
	@echo "=== Testing Gate.io Exchange ==="
	@$(TEST_MULTI_EXCHANGE_L2_BIN) gateio

test_mexc: $(TEST_MULTI_EXCHANGE_L2_BIN)
	@echo "=== Testing MEXC Exchange ==="
	@$(TEST_MULTI_EXCHANGE_L2_BIN) mexc

test_kucoin: $(TEST_MULTI_EXCHANGE_L2_BIN)
	@echo "=== Testing KuCoin Exchange ==="
	@$(TEST_MULTI_EXCHANGE_L2_BIN) kucoin

test_kraken: $(TEST_MULTI_EXCHANGE_L2_BIN)
	@echo "=== Testing Kraken Exchange ==="
	@$(TEST_MULTI_EXCHANGE_L2_BIN) kraken

test_bitget: $(TEST_MULTI_EXCHANGE_L2_BIN)
	@echo "=== Testing Bitget Exchange ==="
	@$(TEST_MULTI_EXCHANGE_L2_BIN) bitget

test_htx: $(TEST_MULTI_EXCHANGE_L2_BIN)
	@echo "=== Testing HTX Exchange ==="
	@$(TEST_MULTI_EXCHANGE_L2_BIN) htx

test_bingx: $(TEST_MULTI_EXCHANGE_L2_BIN)
	@echo "=== Testing BingX Exchange ==="
	@$(TEST_MULTI_EXCHANGE_L2_BIN) bingx

.PHONY: test_all_exchanges
test_all_exchanges: $(TEST_MULTI_EXCHANGE_L2_BIN)
	@echo "=== Testing ALL Exchanges ==="
	@$(TEST_MULTI_EXCHANGE_L2_BIN) all

# ============================================================================
# TEST INTEGRATED PIPELINE (Full Pipeline with SPSC Queues + Storage)
# ============================================================================

TEST_INTEGRATED_PIPELINE_MAIN := test_scripts/test_integrated_pipeline.cpp
TEST_INTEGRATED_PIPELINE_OBJ  := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(TEST_INTEGRATED_PIPELINE_MAIN))
TEST_INTEGRATED_PIPELINE_BIN  := $(BUILDDIR)/test_integrated_pipeline

$(TEST_INTEGRATED_PIPELINE_BIN): $(COMMON_OBJS) $(TEST_INTEGRATED_PIPELINE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -lsqlite3
	@echo "✓ Built test_integrated_pipeline (Full Pipeline)"

.PHONY: test_integrated_pipeline test_pipeline
test_integrated_pipeline: $(TEST_INTEGRATED_PIPELINE_BIN)
test_pipeline: test_integrated_pipeline

# ============================================================================
