# HFT System Latency Analysis

**Date**: November 7, 2025  
**System**: Coin_base_HFT  
**Connection**: Coinbase Advanced Trade WebSocket API

---

## 📊 Latency Components

### 1. Network Latency (Most Critical)

**What it measures**: Time from Coinbase sending data → Your machine receiving first byte

**Expected Values**:
- **Same Data Center**: 0.1-0.5 ms (100-500 μs) ⚡
- **Same City**: 1-5 ms (1,000-5,000 μs)
- **Same Region**: 10-30 ms (10,000-30,000 μs)
- **Cross-Country**: 50-100 ms (50,000-100,000 μs)
- **International**: 100-300 ms (100,000-300,000 μs)

**How to measure**: 
```bash
# Ping Coinbase WebSocket endpoint
ping advanced-trade-ws.coinbase.com
```

**Current Implementation**: ❌ Not measured (ws.read() is blocking)

---

### 2. WebSocket Receive + Parse

**What it measures**: Time from `ws.read()` returning → String parsed

**Expected Values**: 1-10 μs (microseconds)

**Current Implementation**: ✅ Measured in `md/ws_l2_client.hpp`
```cpp
ws.read(buffer);  // Blocks until full message received
auto t1 = now();
std::string raw = beast::buffers_to_string(buffer.data());
auto t2 = now();
latency_ns = t2 - t1;  // This is what we measure
```

---

### 3. JSON Deserialization

**What it measures**: String → nlohmann::json object

**Expected Values**: 5-50 μs depending on message size

**Current Implementation**: ⏳ Can be added to `md/normalizer.hpp`

---

### 4. Order Book Update

**What it measures**: Applying L2 update to order book structure

**Expected Values**: 
- **Snapshot**: 100-500 μs (rebuild entire book)
- **Update**: 5-20 μs (modify 1-2 price levels)

**Current Implementation**: ⏳ Can be added to `md/order_book.hpp`

---

### 5. Strategy Decision

**What it measures**: Order book → Trading signal

**Expected Values**:
- **Simple (Imbalance)**: 1-5 μs
- **Medium (OFI, Microprice)**: 5-20 μs
- **Complex (VPIN)**: 20-100 μs

**Current Implementation**: ⏳ Can be added to `bt/backtester.hpp`

---

## 🎯 Target Latencies for HFT

| Component | Target | Acceptable | Poor |
|-----------|--------|------------|------|
| Network (total) | < 1 ms | < 10 ms | > 50 ms |
| Parse + Deserialize | < 10 μs | < 50 μs | > 100 μs |
| Order Book Update | < 20 μs | < 100 μs | > 500 μs |
| Strategy Logic | < 10 μs | < 50 μs | > 200 μs |
| **End-to-End** | **< 1.5 ms** | **< 15 ms** | **> 100 ms** |

---

## 🔍 Current Measurements

### From Existing Data (Backtest)
```
Mean dt (sec): 0.0557553
```
This is **55.7 milliseconds** between ticks (average time between order book updates).

**NOT the same as latency!** This is the **inter-arrival time** of market data.

---

## 📈 How to Measure True Network Latency

### Option 1: Ping Test (Rough Estimate)
```bash
ping -c 100 advanced-trade-ws.coinbase.com | grep avg
```

### Option 2: Add Exchange Timestamp Comparison
Coinbase includes `timestamp` in messages. Compare:
- `exchange_timestamp` (from Coinbase)
- `receive_timestamp` (when we get it)
- Difference = **Network + Exchange Processing**

### Option 3: Add High-Precision Timestamps
Modify `md/ws_l2_client.hpp`:
```cpp
for (;;) {
    auto t0_before_read = now();
    ws.read(buffer);  
    auto t1_after_read = now();
    
    std::string raw = beast::buffers_to_string(buffer.data());
    auto t2_after_parse = now();
    
    // Network latency (rough): t1 - t0
    // Parse latency: t2 - t1
}
```

---

## 🚀 Optimization Strategies

### If Network Latency is High (> 10ms):
1. **Colocation**: Move server to same data center as Coinbase
2. **Better ISP**: Use direct fiber connection
3. **Geographic**: Move closer to Coinbase servers (AWS us-east-1)

### If Processing Latency is High (> 100μs):
1. **Zero-copy parsing**: Avoid string copies
2. **Lock-free data structures**: For order book
3. **SIMD**: Vectorize calculations
4. **Better compiler flags**: `-O3 -march=native`

### If Strategy Latency is High (> 50μs):
1. **Precompute**: Cache frequently used values
2. **Simplify logic**: Remove unnecessary calculations
3. **Profile**: Use `perf` to find hotspots

---

## 📝 TODO: Complete Latency Instrumentation

1. ✅ Add parsing latency (DONE)
2. ⏳ Add network latency measurement
3. ⏳ Add order book update latency
4. ⏳ Add strategy decision latency
5. ⏳ Log latency breakdown per message
6. ⏳ Create latency visualization

---

## 🎯 Best Practices for Low Latency

### Code Level:
- Use `-O3 -march=native` compilation
- Avoid heap allocations in hot path
- Use stack buffers where possible
- Prefer `std::array` over `std::vector`
- Use `constexpr` for compile-time computation

### System Level:
- CPU affinity (pin threads to cores)
- Disable CPU frequency scaling
- Use kernel bypass (DPDK) for extreme cases
- Disable hyperthreading
- Use huge pages

### Network Level:
- Use direct fiber connection
- Colocate with exchange
- Optimize TCP/IP stack
- Consider UDP (if exchange supports)
- Monitor network metrics with `iftop`

---

## 📊 Measuring Right Now

To get current latency measurements, run:
```bash
# Will print stats every 1000 messages
./build/stream_and_record 2>&1 | grep LATENCY
```

Expected output (after 1000 messages):
```
[LATENCY] n=1000 | min=1.23μs | avg=8.45μs | p50=7.21μs | p95=15.67μs | p99=23.45μs | max=156.78μs
```

These are **parsing latencies only** (not full end-to-end).

---

## 🎓 References

- [QuantStart: Low Latency Trading](https://www.quantstart.com/articles/)
- [Mechanical Markets Blog](https://mechanicalmarkets.wordpress.com/)
- [CME Group Latency Best Practices](https://www.cmegroup.com/confluence/display/EPICSANDBOX/Best+Practices+-+Optimize+Latency)

