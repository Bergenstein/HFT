// exchanges/exchange_config.hpp
#pragma once
#include <string>
#include <map>

namespace exchanges {

struct ExchangeConfig {
    std::string name;
    std::string ws_url;
    double maker_fee_pct;
    double taker_fee_pct;
    int min_latency_ms;
};

inline const std::map<std::string, ExchangeConfig> EXCHANGES = {
    {"coinbase", {"Coinbase", "wss://ws-feed.exchange.coinbase.com", 0.40, 0.60, 50}},
    {"binance", {"Binance", "wss://stream.binance.com:9443/ws", 0.10, 0.10, 30}},
    {"kraken", {"Kraken", "wss://ws.kraken.com", 0.16, 0.26, 100}},
    {"okx", {"OKX", "wss://ws.okx.com:8443/ws/v5/public", 0.08, 0.10, 40}}
};

} // namespace exchanges
