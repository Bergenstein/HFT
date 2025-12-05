# Coin_base_HFT Project Status

**Last Updated:** November 7, 2025  
**Status:** ✅ Active Development - Strategy Testing Phase

---

## 🎯 Executive Summary

High-frequency trading research platform for cryptocurrency markets (Coinbase). Successfully built complete backtesting infrastructure with 7+ microstructure-based strategies.

**Current Phase:** Testing and refining strategies on live market data

---

## ✅ Completed This Session (Nov 7, 2025)

### 1. Infrastructure Fixes
- ✅ Fixed missing `sim/fx.hpp` dependency
- ✅ Successfully compiled all 4 binaries:
  - `stream_and_record` - Live data collection
  - `replay_and_book` - Historical replay
  - `scan_recording` - Data analysis
  - `backtest_imbalance` - Strategy backtesting

### 2. New Strategies Implemented (5 new!)
1. ✅ **Microprice Strategy** - Volume-weighted price mean reversion
2. ✅ **Quote Intensity Strategy** - Update rate imbalance detection
3. ✅ **Spread Reversion Strategy** - Abnormal spread trading
4. ✅ **VPIN Strategy** - Volume-synchronized toxic flow detection
5. ✅ **Volume-Weighted Spread Strategy** - Depth-based directional signals

### 3. Testing Infrastructure
- ✅ First successful backtest on ZEC-USD
- ✅ Created comprehensive PROJECT_STATUS.md documentation
- 📝 Test automation script (in progress)

---

## 📊 Strategy Library (7 Total)

### A. **Imbalance Taker** ✅ TESTED
**File:** `strats/imbalance_taker.hpp`  
**Theory:** Order book imbalance predicts short-term price movements  
**Signal:** `I = (bid_qty - ask_qty) / (bid_qty + ask_qty)`

**Latest Results (ZEC-USD):**
```
Trades: 343 | Return: -0.35% | Sharpe: -11,990 | Fees: $123
```
**Status:** ❌ Unprofitable - overtrading issue

---

### B. **Recursive OFI (Order Flow Imbalance)** ⏳ NOT TESTED
**File:** `strats/strategy_ofi.hpp`  
**Theory:** Exponentially-weighted order flow changes  
**Parameters:** decay=0.97, enter_thr=0.15, exit_thr=0.07

---

### C. **Microprice Strategy** ⏳ NOT TESTED
**File:** `strats/microprice_strategy.hpp`  
**Theory:** Volume-weighted "true" price vs mid-price  
**Formula:** `μ = (Q_ask × P_bid + Q_bid × P_ask) / (Q_bid + Q_ask)`  
**Logic:** Mean revert when mid-price deviates from microprice

**References:**
- Stoikov & Cont (2010) "The Price Impact of Order Book Events"

---

### D. **Quote Intensity Strategy** ⏳ NOT TESTED
**File:** `strats/quote_intensity_strategy.hpp`  
**Theory:** Quote update arrival rates signal informed trading  
**Metric:** `intensity_imb = (bid_updates - ask_updates) / total_updates`

**References:**
- Easley, López de Prado, O'Hara (2012) "Flow Toxicity and Liquidity"

---

### E. **Spread Reversion Strategy** ⏳ NOT TESTED
**File:** `strats/spread_reversion_strategy.hpp`  
**Theory:** Abnormally wide spreads revert to normal  
**Logic:** Enter when spread > avg_spread × threshold

**References:**
- Grossman & Miller (1988) "Liquidity and Market Structure"

---

### F. **VPIN Strategy** ⏳ NOT TESTED
**File:** `strats/vpin_strategy.hpp`  
**Theory:** Volume-synchronized probability of informed trading  
**Logic:** Only trade when VPIN < threshold (low toxic flow)

**References:**
- Easley, López de Prado, O'Hara (2012) "The Volume Clock"

---

### G. **Volume-Weighted Spread Strategy** ⏳ NOT TESTED
**File:** `strats/volume_weighted_spread_strategy.hpp`  
**Theory:** Depth-weighted prices reveal hidden pressure  
**Logic:** VW mid vs quoted mid deviation

**References:**
- Biais, Hillion & Spatt (1995) "Limit Order Book Analysis"

---

## 📈 Test Results

### Test #1: ZEC-USD Imbalance Strategy
**Date:** Nov 7, 2025 18:00  
**Data:** 1,140 ticks (~1 minute)  
**Strategy:** ImbalanceTaker(thresh=0.6, hold=5s)

| Metric | Value |
|--------|-------|
| Trades | 343 |
| Return | -0.35% |
| Sharpe (ann.) | -11,990 |
| Fees | $123.26 |
| Net P&L | -$175.62 |

**Analysis:**
- ❌ Overtrading (343 trades/minute)
- ❌ Fees exceed P&L
- ❌ Poor signal quality

**Action Items:**
1. Reduce trade frequency (longer hold times)
2. Add entry filters (volatility, spread checks)
3. Test other strategies

---

## 🔧 System Components

### Built Binaries (4/4 Complete)
```
build/stream_and_record    1.1 MB   ✅ Working
build/replay_and_book      794 KB   ✅ Working  
build/scan_recording       178 KB   ✅ Working
build/backtest_imbalance   863 KB   ✅ Working
```

### Data Collection
```
data/raw_20251107_174837_ws0.ndjson  10 MB
data/raw_20251107_174838_ws1.ndjson  10 MB
data/raw_20251107_174838_ws2.ndjson  4.5 MB
```

**Products recorded:** 20 (ZEC-USD, AVAX-USD, AERO-USD, BCH-EUR, etc.)

---

## 🚀 Next Steps

### Immediate (Tonight)
1. **Collect longer data recordings**
   - Run stream_and_record for 1+ hours
   - Multiple products (BTC-USD, ETH-USD priority)

2. **Test all strategies systematically**
   - Parameter sweeps for each strategy
   - Document results in this file
   - Find profitable configurations

3. **Create test automation**
   - Finish test_strategies.sh script
   - Auto-generate comparison reports
   - Track best parameters per product

### Short-Term (This Week)
4. **Strategy improvements**
   - Add position sizing based on volatility
   - Implement stop-loss / take-profit
   - Multi-timeframe filters
   - Combine signals (ensemble strategies)

5. **Analysis tools**
   - Performance visualization
   - Equity curve plotting
   - Drawdown analysis
   - Trade distribution stats

### Medium-Term (This Month)
6. **Advanced strategies**
   - Machine learning features (sklearn integration)
   - Multi-product arbitrage
   - Market making (passive orders)
   - Optimal execution (TWAP, VWAP)

7. **Risk management**
   - Position limits by product
   - Portfolio-level risk
   - Correlation analysis
   - VaR calculation

---

## 📚 Theoretical Foundation

### Market Microstructure Principles

1. **Information Asymmetry**
   - Informed traders leave footprints in order flow
   - Measured by: OFI, VPIN, quote intensity

2. **Price Discovery**
   - True price ≠ last trade price
   - Better estimates: microprice, volume-weighted mid

3. **Liquidity Provision**
   - Spread = compensation for adverse selection
   - Wide spreads signal information events

4. **Mean Reversion**
   - Microstructure noise causes temporary deviations
   - Opportunity for HFT strategies

### Academic References
- Cont, Stoikov & Talreja (2010) "A Stochastic Model for Order Book Dynamics"
- Cartea, Jaimungal & Penalva (2015) "Algorithmic and High-Frequency Trading"
- Easley, López de Prado & O'Hara (2012) "Flow Toxicity and Liquidity in a HFT World"

---

## 🐛 Known Issues & Limitations

1. **Short data samples** - Only ~1 min recordings, need hours/days
2. **No parameter optimization** - Manual tuning only
3. **Single product testing** - Need cross-validation
4. **Simplified fee model** - Assumes constant fees
5. **No slippage modeling** - Assumes perfect fills at posted prices

---

## 📊 Performance Tracking Matrix

| Strategy | Product | Sharpe | Return | Trades | Fees | P&L | Status |
|----------|---------|--------|--------|--------|------|-----|--------|
| ImbalanceTaker | ZEC-USD | -11990 | -0.35% | 343 | $123 | -$176 | ❌ |
| ROFI | - | - | - | - | - | - | ⏳ |
| Microprice | - | - | - | - | - | - | ⏳ |
| QuoteIntensity | - | - | - | - | - | - | ⏳ |
| SpreadReversion | - | - | - | - | - | - | ⏳ |
| VPIN | - | - | - | - | - | - | ⏳ |
| VWSpread | - | - | - | - | - | - | ⏳ |

---

## 🎯 Success Criteria

### Research Phase (Current)
- [x] Build functional backtesting framework
- [x] Implement 5+ microstructure strategies
- [ ] Identify profitable strategy (Sharpe > 1.5)
- [ ] Validate on out-of-sample data
- [ ] Document all strategy logic

### Production Phase (Future)
- [ ] Deploy real trading system
- [ ] Achieve Sharpe > 2.0 consistently
- [ ] Maximum drawdown < 5%
- [ ] Operational uptime > 99.9%

---

**Project Owner:** Israel Bergenstein  
**Environment:** macOS, C++20, Coinbase WebSocket API  
**Repository:** Coin_base_HFT/

*Auto-updated after each development session*
