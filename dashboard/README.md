# Funding Rate Arbitrage Dashboard

## Quick Start

```bash
make build/backtest_to_json
./build/backtest_to_json BTCUSDT 1.0
python3 -m http.server 8080
open http://localhost:8080/dashboard/backtest_viewer.html
```

---

## Tests

### Backtest - Funding Rate Arbitrage

```bash
./build/backtest_to_json BTCUSDT 1.0
./build/backtest_to_json ETHUSDT 1.0
./build/backtest_to_json BTCUSDT 2.0 50000 backtest_btc_2bps.json
./build/backtest_to_json ETHUSDT 0.5 200000 backtest_eth_tight.json
```

### Live Funding Rate Scanner

```bash
./build/real_funding_rate_arb BTCUSDT
./build/real_funding_rate_arb ETHUSDT
./build/real_funding_rate_arb SOLUSDT
```

### Historical Backtest (Console Output)

```bash
./build/backtest_real_funding BTCUSDT 1.0
./build/backtest_real_funding ETHUSDT 2.0
```

---

## Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| SYMBOL | Trading pair (BTCUSDT, ETHUSDT, SOLUSDT) | required |
| MIN_DIFF_BPS | Minimum funding differential in bps | required |
| POSITION_SIZE | Position size USD | 100000 |
| OUTPUT_FILE | JSON output path | backtest_results.json |

---

## Data Sources

| Exchange | Endpoint | Max Records |
|----------|----------|-------------|
| Binance | /fapi/v1/fundingRate | 1000 |
| Bybit | /v5/market/funding/history | 200 |
| OKX | /api/v5/public/funding-rate-history | 100 |

---

## Fee Structure

| Exchange | Maker | Taker |
|----------|-------|-------|
| Binance | 0.02% | 0.04% |
| Bybit | 0.02% | 0.055% |
| OKX | 0.02% | 0.05% |

---

## Output

`backtest_results.json` contains:
- `equity_curve` - equity at each funding period
- `trades` - executed trades with P&L
- `summary` - Sharpe, Sortino, drawdown, win rate
