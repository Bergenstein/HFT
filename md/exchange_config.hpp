// md/exchange_config.hpp
#pragma once
#include <string>
#include <map>
#include <vector>

namespace md {

struct ExchangeConfig {
    std::string name;
    std::string ws_url;
    std::string rest_url;
    bool requires_auth;
    double maker_fee;  // bps
    double taker_fee;  // bps
    int min_latency_ms; // typical latency
};

// Global exchange configurations
inline const std::map<std::string, ExchangeConfig> EXCHANGES = {
    {"coinbase", {
        "Coinbase",
        "wss://ws-feed.exchange.coinbase.com",
        "https://api.exchange.coinbase.com",
        false,
        0.40,  // 0.40% maker
        0.60,  // 0.60% taker
        50
    }},
    {"binance", {
        "Binance",
        "wss://stream.binance.com:9443/ws",
        "https://api.binance.com",
        false,
        0.10,  // 0.10% maker
        0.10,  // 0.10% taker
        30
    }},
    {"kraken", {
        "Kraken",
        "wss://ws.kraken.com",
        "https://api.kraken.com",
        false,
        0.16,  // 0.16% maker
        0.26,  // 0.26% taker
        100
    }},
    {"okx", {
        "OKX",
        "wss://ws.okx.com:8443/ws/v5/public",
        "https://www.okx.com",
        false,
        0.08,  // 0.08% maker
        0.10,  // 0.10% taker
        40
    }},
    {"bybit", {
        "Bybit",
        "wss://stream.bybit.com/v5/public/spot",
        "https://api.bybit.com",
        false,
        0.10,  // 0.10% maker
        0.10,  // 0.10% taker
        45
    }},
    {"huobi", {
        "Huobi",
        "wss://api.huobi.pro/ws",
        "https://api.huobi.pro",
        false,
        0.20,  // 0.20% maker
        0.20,  // 0.20% taker
        60
    }}
};

// Product mapping between exchanges (normalized names)
struct ProductMapping {
    std::map<std::string, std::string> exchange_symbols; // exchange -> native symbol
    std::string base;
    std::string quote;
};

// Common trading pairs across exchanges
inline const std::map<std::string, ProductMapping> COMMON_PRODUCTS = {
    {"BTC-USD", {
        {{"coinbase", "BTC-USD"}, {"binance", "BTCUSDT"}, {"kraken", "XBT/USD"}, 
         {"okx", "BTC-USDT"}, {"bybit", "BTCUSDT"}, {"huobi", "btcusdt"}},
        "BTC", "USD"
    }},
    {"BTC-USDT", {
        {{"coinbase", "BTC-USDT"}, {"binance", "BTCUSDT"}, {"kraken", "XBT/USDT"}, 
         {"okx", "BTC-USDT"}, {"bybit", "BTCUSDT"}, {"huobi", "btcusdt"}},
        "BTC", "USDT"
    }},
    {"ETH-USD", {
        {{"coinbase", "ETH-USD"}, {"binance", "ETHUSDT"}, {"kraken", "ETH/USD"}, 
         {"okx", "ETH-USDT"}, {"bybit", "ETHUSDT"}, {"huobi", "ethusdt"}},
        "ETH", "USD"
    }},
    {"ETH-USDT", {
        {{"coinbase", "ETH-USDT"}, {"binance", "ETHUSDT"}, {"kraken", "ETH/USDT"}, 
         {"okx", "ETH-USDT"}, {"bybit", "ETHUSDT"}, {"huobi", "ethusdt"}},
        "ETH", "USDT"
    }},
    {"SOL-USD", {
        {{"coinbase", "SOL-USD"}, {"binance", "SOLUSDT"}, {"kraken", "SOL/USD"}, 
         {"okx", "SOL-USDT"}, {"bybit", "SOLUSDT"}, {"huobi", "solusdt"}},
        "SOL", "USD"
    }},
    {"AVAX-USD", {
        {{"coinbase", "AVAX-USD"}, {"binance", "AVAXUSDT"}, {"kraken", "AVAX/USD"}, 
         {"okx", "AVAX-USDT"}, {"bybit", "AVAXUSDT"}, {"huobi", "avaxusdt"}},
        "AVAX", "USD"
    }},
    {"MATIC-USD", {
        {{"coinbase", "MATIC-USD"}, {"binance", "MATICUSDT"}, {"kraken", "MATIC/USD"}, 
         {"okx", "MATIC-USDT"}, {"bybit", "MATICUSDT"}, {"huobi", "maticusdt"}},
        "MATIC", "USD"
    }},
    {"BNB-USDT", {
        {{"binance", "BNBUSDT"}, {"okx", "BNB-USDT"}, {"bybit", "BNBUSDT"}, {"huobi", "bnbusdt"}},
        "BNB", "USDT"
    }}
};

} // namespace md
