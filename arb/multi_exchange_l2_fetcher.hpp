#pragma once

//==============================================================================
// MULTI-EXCHANGE L2 ORDERBOOK + FUNDING RATE FETCHER
//==============================================================================
// Retrieves L2 orderbook data and funding rates from multiple exchanges
// Normalized format compatible with existing HFT pipeline
//
// Supported Exchanges:
//   1. Binance Futures (L2 + Funding)
//   2. Bybit Perpetuals (L2 + Funding)
//   3. OKX Perpetuals (L2 + Funding)
//   4. Gate.io Futures (L2 + Funding)
//   5. MEXC Futures (L2 + Funding)
//   6. KuCoin Futures (L2 + Funding)
//   7. Bitget Futures (L2 + Funding)
//   8. Kraken Futures (L2 + Funding)
//==============================================================================

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <chrono>
#include <thread>
#include <mutex>
#include <future>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

namespace arb {

using json = nlohmann::json;

//==============================================================================
// NORMALIZED L2 ORDERBOOK DATA
//==============================================================================

struct L2Level {
    double price;
    double quantity;
    
    L2Level() : price(0), quantity(0) {}
    L2Level(double p, double q) : price(p), quantity(q) {}
};

struct NormalizedL2Book {
    std::string exchange;
    std::string symbol;              // Original exchange symbol
    std::string normalized_symbol;   // BTC, ETH, etc.
    std::vector<L2Level> bids;       // Sorted descending
    std::vector<L2Level> asks;       // Sorted ascending
    int64_t timestamp_ms;
    int64_t exchange_timestamp_ms;
    
    // Best bid/ask convenience
    std::optional<L2Level> best_bid() const {
        if (bids.empty()) return std::nullopt;
        return bids[0];
    }
    
    std::optional<L2Level> best_ask() const {
        if (asks.empty()) return std::nullopt;
        return asks[0];
    }
    
    double mid_price() const {
        auto bb = best_bid();
        auto ba = best_ask();
        if (!bb || !ba) return 0.0;
        return (bb->price + ba->price) / 2.0;
    }
    
    double spread_bps() const {
        auto bb = best_bid();
        auto ba = best_ask();
        if (!bb || !ba || bb->price == 0) return 0.0;
        return ((ba->price - bb->price) / bb->price) * 10000.0;
    }
    
    double liquidity_imbalance() const {
        auto bb = best_bid();
        auto ba = best_ask();
        if (!bb || !ba) return 0.0;
        double total = bb->quantity + ba->quantity;
        if (total == 0) return 0.0;
        return (bb->quantity - ba->quantity) / total;
    }
};

//==============================================================================
// FUNDING RATE DATA (Enhanced)
//==============================================================================

struct FundingRateData {
    std::string exchange;
    std::string symbol;
    std::string normalized_symbol;
    double funding_rate;           // 8-hour rate
    double funding_rate_annual;    // APY %
    int64_t next_funding_time_ms;
    double mark_price;
    double index_price;
    int64_t timestamp_ms;
    int funding_interval_hours;
    
    FundingRateData() : funding_rate(0), funding_rate_annual(0), 
                        next_funding_time_ms(0), mark_price(0), 
                        index_price(0), timestamp_ms(0), 
                        funding_interval_hours(8) {}
    
    double hours_until_funding() const {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return static_cast<double>(next_funding_time_ms - now_ms) / (1000.0 * 3600.0);
    }
};

//==============================================================================
// COMBINED MARKET DATA (L2 + Funding)
//==============================================================================

struct ExchangeMarketData {
    NormalizedL2Book orderbook;
    std::optional<FundingRateData> funding;
    
    bool is_valid() const {
        return !orderbook.bids.empty() && !orderbook.asks.empty();
    }
};

//==============================================================================
// SYMBOL NORMALIZER
//==============================================================================

class SymbolNormalizer {
public:
    static std::string normalize(const std::string& symbol) {
        std::string norm = symbol;
        
        // Convert to uppercase
        std::transform(norm.begin(), norm.end(), norm.begin(), ::toupper);
        
        // Remove common suffixes
        const std::vector<std::string> suffixes = {
            "USDT", "USDC", "USD", "PERP", "-PERP", "_PERP",
            "-SWAP", "_SWAP", "SWAP", "-UMCBL", "_UMCBL",
            "BUSD", "TUSD", "_", "-"
        };
        
        for (const auto& suffix : suffixes) {
            size_t pos = norm.find(suffix);
            if (pos != std::string::npos) {
                norm = norm.substr(0, pos);
            }
        }
        
        return norm;
    }
    
    static bool is_perpetual(const std::string& symbol) {
        std::string upper = symbol;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        return upper.find("PERP") != std::string::npos ||
               upper.find("SWAP") != std::string::npos ||
               upper.find("USD") != std::string::npos;
    }
};

//==============================================================================
// HTTPS CLIENT (using libcurl - same as Python requests)
//==============================================================================

class HTTPSClient {
private:
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
    
public:
    static std::optional<std::string> get(
        const std::string& host, 
        const std::string& path,
        int timeout_seconds = 10) {
        
        CURL* curl = curl_easy_init();
        if (!curl) return std::nullopt;
        
        std::string url = "https://" + host + path;
        std::string response_data;
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // For testing, disable in production
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "HFT-System/1.0");
        
        CURLcode res = curl_easy_perform(curl);
        
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK || http_code != 200) {
            return std::nullopt;
        }
        
        return response_data;
    }
};

//==============================================================================
// MULTI-EXCHANGE L2 + FUNDING FETCHER
//==============================================================================

class MultiExchangeL2Fetcher {
public:
    // Function type for exchange fetch functions
    using FetchFunc = std::function<std::vector<ExchangeMarketData>(int)>;
    
private:
    std::mutex mutex_;
    std::map<std::string, std::vector<ExchangeMarketData>> cache_;
    std::chrono::system_clock::time_point last_update_;
    
public:
    //--------------------------------------------------------------------------
    // BINANCE FUTURES - L2 + Funding
    //--------------------------------------------------------------------------
    static std::vector<ExchangeMarketData> fetch_binance(int limit = 20) {
        std::vector<ExchangeMarketData> results;
        
        try {
            // Get funding rates first
            auto fr_response = HTTPSClient::get("fapi.binance.com", "/fapi/v1/premiumIndex");
            if (!fr_response) return results;
            
            json funding_data = json::parse(*fr_response);
            std::map<std::string, FundingRateData> funding_map;
            
            for (const auto& item : funding_data) {
                if (!item.contains("symbol") || !item.contains("lastFundingRate")) continue;
                
                FundingRateData fr;
                fr.exchange = "binance";
                fr.symbol = item["symbol"].get<std::string>();
                fr.normalized_symbol = SymbolNormalizer::normalize(fr.symbol);
                fr.funding_rate = std::stod(item["lastFundingRate"].get<std::string>());
                fr.funding_rate_annual = fr.funding_rate * 3 * 365 * 100;
                fr.next_funding_time_ms = item.value("nextFundingTime", 0LL);
                fr.mark_price = std::stod(item.value("markPrice", "0"));
                fr.index_price = std::stod(item.value("indexPrice", "0"));
                fr.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                
                funding_map[fr.symbol] = fr;
            }
            
            // Get L2 orderbooks for top symbols
            int count = 0;
            for (const auto& [symbol, fr] : funding_map) {
                if (++count > 50) break;  // Limit to avoid rate limits
                
                std::string path = "/fapi/v1/depth?symbol=" + symbol + "&limit=" + std::to_string(limit);
                auto l2_response = HTTPSClient::get("fapi.binance.com", path);
                if (!l2_response) continue;
                
                json l2_data = json::parse(*l2_response);
                
                ExchangeMarketData market;
                market.orderbook.exchange = "binance";
                market.orderbook.symbol = symbol;
                market.orderbook.normalized_symbol = SymbolNormalizer::normalize(symbol);
                market.orderbook.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                market.orderbook.exchange_timestamp_ms = l2_data.value("E", 0LL);
                
                // Parse bids
                if (l2_data.contains("bids")) {
                    for (const auto& bid : l2_data["bids"]) {
                        if (bid.size() >= 2) {
                            double price = std::stod(bid[0].get<std::string>());
                            double qty = std::stod(bid[1].get<std::string>());
                            market.orderbook.bids.emplace_back(price, qty);
                        }
                    }
                }
                
                // Parse asks
                if (l2_data.contains("asks")) {
                    for (const auto& ask : l2_data["asks"]) {
                        if (ask.size() >= 2) {
                            double price = std::stod(ask[0].get<std::string>());
                            double qty = std::stod(ask[1].get<std::string>());
                            market.orderbook.asks.emplace_back(price, qty);
                        }
                    }
                }
                
                market.funding = fr;
                
                if (market.is_valid()) {
                    results.push_back(market);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[Binance] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // BYBIT PERPETUALS - L2 + Funding
    //--------------------------------------------------------------------------
    static std::vector<ExchangeMarketData> fetch_bybit(int limit = 20) {
        std::vector<ExchangeMarketData> results;
        
        try {
            // Get funding rates with tickers
            auto ticker_response = HTTPSClient::get("api.bybit.com", "/v5/market/tickers?category=linear");
            if (!ticker_response) {
                std::cerr << "[Bybit] Failed to get ticker response\n";
                return results;
            }
            
            json ticker_data = json::parse(*ticker_response);
            
            if (!ticker_data.contains("result") || !ticker_data["result"].contains("list")) {
                std::cerr << "[Bybit] Invalid ticker data structure\n";
                return results;
            }
            
            int count = 0;
            for (const auto& item : ticker_data["result"]["list"]) {
                if (++count > 30) break;
                if (!item.contains("symbol") || !item.contains("fundingRate")) continue;
                
                std::string symbol = item["symbol"].get<std::string>();
                std::string fr_str = item["fundingRate"].get<std::string>();
                if (fr_str.empty() || fr_str == "0") continue;
                
                // Get L2 orderbook
                std::string path = "/v5/market/orderbook?category=linear&symbol=" + symbol + 
                                  "&limit=" + std::to_string(limit);
                auto l2_response = HTTPSClient::get("api.bybit.com", path);
                if (!l2_response) continue;
                
                json l2_data = json::parse(*l2_response);
                
                if (!l2_data.contains("result")) continue;
                
                ExchangeMarketData market;
                market.orderbook.exchange = "bybit";
                market.orderbook.symbol = symbol;
                market.orderbook.normalized_symbol = SymbolNormalizer::normalize(symbol);
                market.orderbook.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                market.orderbook.exchange_timestamp_ms = l2_data["result"].value("ts", 0LL);
                
                // Parse bids
                if (l2_data["result"].contains("b")) {
                    for (const auto& bid : l2_data["result"]["b"]) {
                        if (bid.size() >= 2) {
                            double price = std::stod(bid[0].get<std::string>());
                            double qty = std::stod(bid[1].get<std::string>());
                            market.orderbook.bids.emplace_back(price, qty);
                        }
                    }
                }
                
                // Parse asks
                if (l2_data["result"].contains("a")) {
                    for (const auto& ask : l2_data["result"]["a"]) {
                        if (ask.size() >= 2) {
                            double price = std::stod(ask[0].get<std::string>());
                            double qty = std::stod(ask[1].get<std::string>());
                            market.orderbook.asks.emplace_back(price, qty);
                        }
                    }
                }
                
                // Add funding rate - Bybit has variable interval (1h or 8h)
                FundingRateData fr;
                fr.exchange = "bybit";
                fr.symbol = symbol;
                fr.normalized_symbol = SymbolNormalizer::normalize(symbol);
                fr.funding_rate = std::stod(fr_str);
                
                // Get funding interval from item if available
                int interval_hours = 8; // default
                if (item.contains("fundingIntervalHour")) {
                    std::string interval_str = item["fundingIntervalHour"].get<std::string>();
                    interval_hours = std::stoi(interval_str);
                }
                fr.funding_interval_hours = interval_hours;
                
                // Calculate annualized rate based on actual interval
                double payments_per_year = (24.0 / interval_hours) * 365;
                fr.funding_rate_annual = fr.funding_rate * payments_per_year * 100;
                
                fr.next_funding_time_ms = std::stoll(item.value("nextFundingTime", "0"));
                fr.mark_price = std::stod(item.value("markPrice", "0"));
                fr.index_price = std::stod(item.value("indexPrice", "0"));
                fr.timestamp_ms = market.orderbook.timestamp_ms;
                
                market.funding = fr;
                
                if (market.is_valid()) {
                    results.push_back(market);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[Bybit] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // OKX PERPETUALS - L2 + Funding
    //--------------------------------------------------------------------------
    static std::vector<ExchangeMarketData> fetch_okx(int limit = 20) {
        std::vector<ExchangeMarketData> results;
        
        try {
            // Get instruments
            auto inst_response = HTTPSClient::get("www.okx.com", "/api/v5/public/instruments?instType=SWAP");
            if (!inst_response) return results;
            
            json instruments = json::parse(*inst_response);
            if (!instruments.contains("data")) return results;
            
            int count = 0;
            for (const auto& inst : instruments["data"]) {
                if (++count > 20) break;
                
                std::string inst_id = inst["instId"].get<std::string>();
                
                // Get orderbook
                std::string l2_path = "/api/v5/market/books?instId=" + inst_id + "&sz=" + std::to_string(limit);
                auto l2_response = HTTPSClient::get("www.okx.com", l2_path);
                if (!l2_response) continue;
                
                json l2_data = json::parse(*l2_response);
                if (!l2_data.contains("data") || l2_data["data"].empty()) continue;
                
                // Get funding rate
                std::string fr_path = "/api/v5/public/funding-rate?instId=" + inst_id;
                auto fr_response = HTTPSClient::get("www.okx.com", fr_path);
                
                ExchangeMarketData market;
                market.orderbook.exchange = "okx";
                market.orderbook.symbol = inst_id;
                market.orderbook.normalized_symbol = SymbolNormalizer::normalize(inst_id);
                market.orderbook.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                market.orderbook.exchange_timestamp_ms = std::stoll(l2_data["data"][0].value("ts", "0"));
                
                // Parse bids
                if (l2_data["data"][0].contains("bids")) {
                    for (const auto& bid : l2_data["data"][0]["bids"]) {
                        if (bid.size() >= 2) {
                            double price = std::stod(bid[0].get<std::string>());
                            double qty = std::stod(bid[1].get<std::string>());
                            market.orderbook.bids.emplace_back(price, qty);
                        }
                    }
                }
                
                // Parse asks
                if (l2_data["data"][0].contains("asks")) {
                    for (const auto& ask : l2_data["data"][0]["asks"]) {
                        if (ask.size() >= 2) {
                            double price = std::stod(ask[0].get<std::string>());
                            double qty = std::stod(ask[1].get<std::string>());
                            market.orderbook.asks.emplace_back(price, qty);
                        }
                    }
                }
                
                // Add funding rate if available
                if (fr_response) {
                    json fr_data = json::parse(*fr_response);
                    if (fr_data.contains("data") && !fr_data["data"].empty()) {
                        FundingRateData fr;
                        fr.exchange = "okx";
                        fr.symbol = inst_id;
                        fr.normalized_symbol = SymbolNormalizer::normalize(inst_id);
                        fr.funding_rate = std::stod(fr_data["data"][0]["fundingRate"].get<std::string>());
                        fr.funding_rate_annual = fr.funding_rate * 3 * 365 * 100;
                        fr.next_funding_time_ms = std::stoll(fr_data["data"][0].value("nextFundingTime", "0"));
                        fr.timestamp_ms = market.orderbook.timestamp_ms;
                        
                        market.funding = fr;
                    }
                }
                
                if (market.is_valid()) {
                    results.push_back(market);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[OKX] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // GATE.IO FUTURES - L2 + Funding
    //--------------------------------------------------------------------------
    static std::vector<ExchangeMarketData> fetch_gateio(int limit = 20) {
        std::vector<ExchangeMarketData> results;
        
        try {
            // Get contracts with funding info
            auto contracts_response = HTTPSClient::get("api.gateio.ws", "/api/v4/futures/usdt/contracts");
            if (!contracts_response) return results;
            
            json contracts = json::parse(*contracts_response);
            
            int count = 0;
            for (const auto& contract : contracts) {
                if (++count > 20) break;
                if (!contract.contains("name")) continue;
                
                std::string symbol = contract["name"].get<std::string>();
                
                // Get orderbook
                std::string l2_path = "/api/v4/futures/usdt/order_book?contract=" + symbol + 
                                     "&limit=" + std::to_string(limit);
                auto l2_response = HTTPSClient::get("api.gateio.ws", l2_path);
                if (!l2_response) continue;
                
                json l2_data = json::parse(*l2_response);
                
                ExchangeMarketData market;
                market.orderbook.exchange = "gateio";
                market.orderbook.symbol = symbol;
                market.orderbook.normalized_symbol = SymbolNormalizer::normalize(symbol);
                market.orderbook.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                market.orderbook.exchange_timestamp_ms = l2_data.value("current", 0LL) * 1000;
                
                // Parse bids
                if (l2_data.contains("bids")) {
                    for (const auto& bid : l2_data["bids"]) {
                        if (bid.contains("p") && bid.contains("s")) {
                            double price = std::stod(bid["p"].get<std::string>());
                            double qty = bid["s"].get<double>();
                            market.orderbook.bids.emplace_back(price, qty);
                        }
                    }
                }
                
                // Parse asks
                if (l2_data.contains("asks")) {
                    for (const auto& ask : l2_data["asks"]) {
                        if (ask.contains("p") && ask.contains("s")) {
                            double price = std::stod(ask["p"].get<std::string>());
                            double qty = ask["s"].get<double>();
                            market.orderbook.asks.emplace_back(price, qty);
                        }
                    }
                }
                
                // Add funding rate from contract info
                if (contract.contains("funding_rate")) {
                    FundingRateData fr;
                    fr.exchange = "gateio";
                    fr.symbol = symbol;
                    fr.normalized_symbol = SymbolNormalizer::normalize(symbol);
                    fr.funding_rate = std::stod(contract["funding_rate"].get<std::string>());
                    
                    int interval_seconds = contract.value("funding_interval", 28800);
                    fr.funding_interval_hours = interval_seconds / 3600;
                    double payments_per_year = (24.0 / fr.funding_interval_hours) * 365;
                    fr.funding_rate_annual = fr.funding_rate * payments_per_year * 100;
                    
                    fr.next_funding_time_ms = contract.value("funding_next_apply", 0LL) * 1000;
                    fr.mark_price = std::stod(contract.value("mark_price", "0"));
                    fr.timestamp_ms = market.orderbook.timestamp_ms;
                    
                    market.funding = fr;
                }
                
                if (market.is_valid()) {
                    results.push_back(market);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[Gate.io] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // MEXC FUTURES - L2 + Funding
    //--------------------------------------------------------------------------
    static std::vector<ExchangeMarketData> fetch_mexc(int limit = 20) {
        std::vector<ExchangeMarketData> results;
        
        try {
            // Get ticker with funding rates
            auto ticker_response = HTTPSClient::get("contract.mexc.com", "/api/v1/contract/ticker");
            if (!ticker_response) return results;
            
            json ticker_data = json::parse(*ticker_response);
            if (!ticker_data.contains("data")) return results;
            
            int count = 0;
            for (const auto& item : ticker_data["data"]) {
                if (++count > 20) break;
                if (!item.contains("symbol")) continue;
                
                std::string symbol = item["symbol"].get<std::string>();
                
                // Get orderbook
                std::string l2_path = "/api/v1/contract/depth/" + symbol + "?limit=" + std::to_string(limit);
                auto l2_response = HTTPSClient::get("contract.mexc.com", l2_path);
                if (!l2_response) continue;
                
                json l2_data = json::parse(*l2_response);
                if (!l2_data.contains("data")) continue;
                
                ExchangeMarketData market;
                market.orderbook.exchange = "mexc";
                market.orderbook.symbol = symbol;
                market.orderbook.normalized_symbol = SymbolNormalizer::normalize(symbol);
                market.orderbook.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                market.orderbook.exchange_timestamp_ms = l2_data["data"].value("time", 0LL);
                
                // Parse bids
                if (l2_data["data"].contains("bids")) {
                    for (const auto& bid : l2_data["data"]["bids"]) {
                        if (bid.size() >= 2) {
                            market.orderbook.bids.emplace_back(bid[0].get<double>(), bid[1].get<double>());
                        }
                    }
                }
                
                // Parse asks
                if (l2_data["data"].contains("asks")) {
                    for (const auto& ask : l2_data["data"]["asks"]) {
                        if (ask.size() >= 2) {
                            market.orderbook.asks.emplace_back(ask[0].get<double>(), ask[1].get<double>());
                        }
                    }
                }
                
                // Add funding rate
                if (item.contains("fundingRate")) {
                    FundingRateData fr;
                    fr.exchange = "mexc";
                    fr.symbol = symbol;
                    fr.normalized_symbol = SymbolNormalizer::normalize(symbol);
                    fr.funding_rate = item["fundingRate"].get<double>();
                    fr.funding_rate_annual = fr.funding_rate * 3 * 365 * 100;
                    fr.mark_price = item.value("lastPrice", 0.0);
                    fr.timestamp_ms = market.orderbook.timestamp_ms;
                    
                    market.funding = fr;
                }
                
                if (market.is_valid()) {
                    results.push_back(market);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[MEXC] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // KUCOIN FUTURES - L2 + Funding
    //--------------------------------------------------------------------------
    static std::vector<ExchangeMarketData> fetch_kucoin(int limit = 20) {
        std::vector<ExchangeMarketData> results;
        
        try {
            // Get active contracts
            auto contracts_response = HTTPSClient::get("api-futures.kucoin.com", "/api/v1/contracts/active");
            if (!contracts_response) return results;
            
            json contracts_resp = json::parse(*contracts_response);
            if (!contracts_resp.contains("data")) return results;
            
            int count = 0;
            for (const auto& contract : contracts_resp["data"]) {
                if (++count > 20) break;
                if (!contract.contains("symbol")) continue;
                
                std::string symbol = contract["symbol"].get<std::string>();
                
                // Get orderbook
                std::string l2_path = "/api/v1/level2/snapshot?symbol=" + symbol;
                auto l2_response = HTTPSClient::get("api-futures.kucoin.com", l2_path);
                if (!l2_response) continue;
                
                json l2_resp = json::parse(*l2_response);
                if (!l2_resp.contains("data")) continue;
                
                json l2_data = l2_resp["data"];
                
                ExchangeMarketData market;
                market.orderbook.exchange = "kucoin";
                market.orderbook.symbol = symbol;
                market.orderbook.normalized_symbol = SymbolNormalizer::normalize(symbol);
                market.orderbook.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                market.orderbook.exchange_timestamp_ms = l2_data.value("ts", 0LL);
                
                // Parse bids
                if (l2_data.contains("bids")) {
                    for (const auto& bid : l2_data["bids"]) {
                        if (bid.size() >= 2) {
                            market.orderbook.bids.emplace_back(bid[0].get<double>(), bid[1].get<double>());
                        }
                    }
                }
                
                // Parse asks
                if (l2_data.contains("asks")) {
                    for (const auto& ask : l2_data["asks"]) {
                        if (ask.size() >= 2) {
                            market.orderbook.asks.emplace_back(ask[0].get<double>(), ask[1].get<double>());
                        }
                    }
                }
                
                // Add funding rate from contract
                if (contract.contains("fundingFeeRate")) {
                    FundingRateData fr;
                    fr.exchange = "kucoin";
                    fr.symbol = symbol;
                    fr.normalized_symbol = SymbolNormalizer::normalize(symbol);
                    fr.funding_rate = contract["fundingFeeRate"].get<double>();
                    fr.funding_rate_annual = fr.funding_rate * 3 * 365 * 100;
                    fr.mark_price = contract.value("markPrice", 0.0);
                    fr.index_price = contract.value("indexPrice", 0.0);
                    fr.timestamp_ms = market.orderbook.timestamp_ms;
                    
                    market.funding = fr;
                }
                
                if (market.is_valid()) {
                    results.push_back(market);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[KuCoin] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // KRAKEN FUTURES - L2 + Funding
    //--------------------------------------------------------------------------
    static std::vector<ExchangeMarketData> fetch_kraken(int limit = 20) {
        std::vector<ExchangeMarketData> results;
        
        try {
            // Get tickers with funding info
            auto ticker_response = HTTPSClient::get("futures.kraken.com", "/derivatives/api/v3/tickers");
            if (!ticker_response) return results;
            
            json ticker_data = json::parse(*ticker_response);
            if (!ticker_data.contains("tickers")) return results;
            
            int count = 0;
            for (const auto& ticker : ticker_data["tickers"]) {
                if (++count > 20) break;
                if (!ticker.contains("symbol")) continue;
                
                std::string symbol = ticker["symbol"].get<std::string>();
                
                // Get orderbook
                std::string l2_path = "/derivatives/api/v3/orderbook?symbol=" + symbol;
                auto l2_response = HTTPSClient::get("futures.kraken.com", l2_path);
                if (!l2_response) continue;
                
                json l2_resp = json::parse(*l2_response);
                if (!l2_resp.contains("orderBook")) continue;
                
                json l2_data = l2_resp["orderBook"];
                
                ExchangeMarketData market;
                market.orderbook.exchange = "kraken";
                market.orderbook.symbol = symbol;
                market.orderbook.normalized_symbol = SymbolNormalizer::normalize(symbol);
                market.orderbook.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                market.orderbook.exchange_timestamp_ms = market.orderbook.timestamp_ms;
                
                // Parse bids
                if (l2_data.contains("bids")) {
                    for (const auto& bid : l2_data["bids"]) {
                        if (bid.size() >= 2) {
                            market.orderbook.bids.emplace_back(bid[0].get<double>(), bid[1].get<double>());
                        }
                    }
                }
                
                // Parse asks
                if (l2_data.contains("asks")) {
                    for (const auto& ask : l2_data["asks"]) {
                        if (ask.size() >= 2) {
                            market.orderbook.asks.emplace_back(ask[0].get<double>(), ask[1].get<double>());
                        }
                    }
                }
                
                // Add funding rate from ticker
                if (ticker.contains("fundingRate")) {
                    FundingRateData fr;
                    fr.exchange = "kraken";
                    fr.symbol = symbol;
                    fr.normalized_symbol = SymbolNormalizer::normalize(symbol);
                    fr.funding_rate = ticker["fundingRate"].get<double>();
                    fr.funding_rate_annual = fr.funding_rate * 3 * 365 * 100;
                    fr.mark_price = ticker.value("markPrice", 0.0);
                    fr.index_price = ticker.value("indexPrice", 0.0);
                    fr.timestamp_ms = market.orderbook.timestamp_ms;
                    
                    market.funding = fr;
                }
                
                if (market.is_valid()) {
                    results.push_back(market);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[Kraken] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // BITGET FUTURES - L2 + Funding
    //--------------------------------------------------------------------------
    static std::vector<ExchangeMarketData> fetch_bitget(int limit = 20) {
        std::vector<ExchangeMarketData> results;
        
        try {
            // Get tickers with funding rates
            auto ticker_response = HTTPSClient::get("api.bitget.com", "/api/v2/mix/market/tickers?productType=USDT-FUTURES");
            if (!ticker_response) return results;
            
            json ticker_data = json::parse(*ticker_response);
            if (!ticker_data.contains("data")) return results;
            
            int count = 0;
            for (const auto& item : ticker_data["data"]) {
                if (++count > 30) break;
                if (!item.contains("symbol") || !item.contains("fundingRate")) continue;
                
                std::string symbol = item["symbol"].get<std::string>();
                
                // Get L2 orderbook
                std::string path = "/api/v2/mix/market/orderbook?productType=USDT-FUTURES&symbol=" + symbol + 
                                  "&limit=" + std::to_string(limit);
                auto l2_response = HTTPSClient::get("api.bitget.com", path);
                if (!l2_response) continue;
                
                json l2_data = json::parse(*l2_response);
                if (!l2_data.contains("data")) continue;
                
                ExchangeMarketData market;
                market.orderbook.exchange = "bitget";
                market.orderbook.symbol = symbol;
                market.orderbook.normalized_symbol = SymbolNormalizer::normalize(symbol);
                market.orderbook.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                market.orderbook.exchange_timestamp_ms = std::stoll(item.value("ts", "0"));
                
                // Parse bids
                if (l2_data["data"].contains("bids")) {
                    for (const auto& bid : l2_data["data"]["bids"]) {
                        if (bid.size() >= 2) {
                            double price = std::stod(bid[0].get<std::string>());
                            double qty = std::stod(bid[1].get<std::string>());
                            market.orderbook.bids.emplace_back(price, qty);
                        }
                    }
                }
                
                // Parse asks
                if (l2_data["data"].contains("asks")) {
                    for (const auto& ask : l2_data["data"]["asks"]) {
                        if (ask.size() >= 2) {
                            double price = std::stod(ask[0].get<std::string>());
                            double qty = std::stod(ask[1].get<std::string>());
                            market.orderbook.asks.emplace_back(price, qty);
                        }
                    }
                }
                
                // Add funding rate
                FundingRateData fr;
                fr.exchange = "bitget";
                fr.symbol = symbol;
                fr.normalized_symbol = SymbolNormalizer::normalize(symbol);
                fr.funding_rate = std::stod(item["fundingRate"].get<std::string>());
                fr.funding_rate_annual = fr.funding_rate * 3 * 365 * 100;
                fr.mark_price = std::stod(item.value("markPrice", "0"));
                fr.index_price = std::stod(item.value("indexPrice", "0"));
                fr.timestamp_ms = market.orderbook.timestamp_ms;
                
                market.funding = fr;
                
                if (market.is_valid()) {
                    results.push_back(market);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[Bitget] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // HTX (HUOBI) FUTURES - L2 + Funding
    //--------------------------------------------------------------------------
    static std::vector<ExchangeMarketData> fetch_htx(int limit = 20) {
        std::vector<ExchangeMarketData> results;
        
        try {
            // Get funding rates
            auto funding_response = HTTPSClient::get("api.hbdm.com", "/swap-api/v1/swap_batch_funding_rate");
            if (!funding_response) return results;
            
            json funding_data = json::parse(*funding_response);
            if (!funding_data.contains("data")) return results;
            
            int count = 0;
            for (const auto& item : funding_data["data"]) {
                if (++count > 30) break;
                if (!item.contains("contract_code") || !item.contains("funding_rate")) continue;
                
                std::string symbol = item["contract_code"].get<std::string>();
                
                // Get L2 orderbook
                std::string path = "/swap-ex/market/depth?contract_code=" + symbol + "&type=step0";
                auto l2_response = HTTPSClient::get("api.hbdm.com", path);
                if (!l2_response) continue;
                
                json l2_data = json::parse(*l2_response);
                if (!l2_data.contains("tick")) continue;
                
                ExchangeMarketData market;
                market.orderbook.exchange = "htx";
                market.orderbook.symbol = symbol;
                market.orderbook.normalized_symbol = SymbolNormalizer::normalize(symbol);
                market.orderbook.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                market.orderbook.exchange_timestamp_ms = l2_data.value("ts", 0LL);
                
                // Parse bids
                if (l2_data["tick"].contains("bids")) {
                    int bid_count = 0;
                    for (const auto& bid : l2_data["tick"]["bids"]) {
                        if (++bid_count > limit) break;
                        if (bid.size() >= 2) {
                            market.orderbook.bids.emplace_back(bid[0].get<double>(), bid[1].get<double>());
                        }
                    }
                }
                
                // Parse asks
                if (l2_data["tick"].contains("asks")) {
                    int ask_count = 0;
                    for (const auto& ask : l2_data["tick"]["asks"]) {
                        if (++ask_count > limit) break;
                        if (ask.size() >= 2) {
                            market.orderbook.asks.emplace_back(ask[0].get<double>(), ask[1].get<double>());
                        }
                    }
                }
                
                // Add funding rate
                FundingRateData fr;
                fr.exchange = "htx";
                fr.symbol = symbol;
                fr.normalized_symbol = SymbolNormalizer::normalize(symbol);
                fr.funding_rate = std::stod(item["funding_rate"].get<std::string>());
                fr.funding_rate_annual = fr.funding_rate * 3 * 365 * 100;
                fr.next_funding_time_ms = std::stoll(item.value("funding_time", "0"));
                fr.timestamp_ms = market.orderbook.timestamp_ms;
                
                market.funding = fr;
                
                if (market.is_valid()) {
                    results.push_back(market);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[HTX] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // BINGX FUTURES - L2 + Funding
    //--------------------------------------------------------------------------
    static std::vector<ExchangeMarketData> fetch_bingx(int limit = 20) {
        std::vector<ExchangeMarketData> results;
        
        try {
            // Get contracts
            auto contracts_response = HTTPSClient::get("open-api.bingx.com", "/openApi/swap/v2/quote/contracts");
            if (!contracts_response) return results;
            
            json contracts_data = json::parse(*contracts_response);
            if (!contracts_data.contains("data")) return results;
            
            int count = 0;
            for (const auto& contract : contracts_data["data"]) {
                if (++count > 30) break;
                if (!contract.contains("symbol")) continue;
                
                std::string symbol = contract["symbol"].get<std::string>();
                
                // Get funding rate
                std::string fr_path = "/openApi/swap/v2/quote/fundingRate?symbol=" + symbol;
                auto fr_response = HTTPSClient::get("open-api.bingx.com", fr_path);
                if (!fr_response) continue;
                
                json fr_json = json::parse(*fr_response);
                if (!fr_json.contains("data") || !fr_json["data"].contains("fundingRate")) continue;
                
                // Get L2 orderbook
                std::string l2_path = "/openApi/swap/v2/quote/depth?symbol=" + symbol + "&limit=" + std::to_string(limit);
                auto l2_response = HTTPSClient::get("open-api.bingx.com", l2_path);
                if (!l2_response) continue;
                
                json l2_data = json::parse(*l2_response);
                if (!l2_data.contains("data")) continue;
                
                ExchangeMarketData market;
                market.orderbook.exchange = "bingx";
                market.orderbook.symbol = symbol;
                market.orderbook.normalized_symbol = SymbolNormalizer::normalize(symbol);
                market.orderbook.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                market.orderbook.exchange_timestamp_ms = l2_data["data"].value("T", 0LL);
                
                // Parse bids
                if (l2_data["data"].contains("bids")) {
                    for (const auto& bid : l2_data["data"]["bids"]) {
                        if (bid.size() >= 2) {
                            double price = std::stod(bid[0].get<std::string>());
                            double qty = std::stod(bid[1].get<std::string>());
                            market.orderbook.bids.emplace_back(price, qty);
                        }
                    }
                }
                
                // Parse asks
                if (l2_data["data"].contains("asks")) {
                    for (const auto& ask : l2_data["data"]["asks"]) {
                        if (ask.size() >= 2) {
                            double price = std::stod(ask[0].get<std::string>());
                            double qty = std::stod(ask[1].get<std::string>());
                            market.orderbook.asks.emplace_back(price, qty);
                        }
                    }
                }
                
                // Add funding rate
                FundingRateData fr;
                fr.exchange = "bingx";
                fr.symbol = symbol;
                fr.normalized_symbol = SymbolNormalizer::normalize(symbol);
                fr.funding_rate = fr_json["data"]["fundingRate"].get<double>();
                fr.funding_rate_annual = fr.funding_rate * 3 * 365 * 100;
                fr.timestamp_ms = market.orderbook.timestamp_ms;
                
                market.funding = fr;
                
                if (market.is_valid()) {
                    results.push_back(market);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[BingX] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // FETCH ALL EXCHANGES IN PARALLEL
    //--------------------------------------------------------------------------
    std::map<std::string, std::vector<ExchangeMarketData>> fetch_all_parallel() {
        std::cout << "[FETCHER] Fetching L2 + Funding from all exchanges...\n";
        
        auto f_binance = std::async(std::launch::async, fetch_binance, 20);
        auto f_bybit = std::async(std::launch::async, fetch_bybit, 20);
        auto f_okx = std::async(std::launch::async, fetch_okx, 20);
        auto f_gateio = std::async(std::launch::async, fetch_gateio, 20);
        auto f_mexc = std::async(std::launch::async, fetch_mexc, 20);
        auto f_kucoin = std::async(std::launch::async, fetch_kucoin, 20);
        auto f_kraken = std::async(std::launch::async, fetch_kraken, 20);
        auto f_bitget = std::async(std::launch::async, fetch_bitget, 20);
        auto f_htx = std::async(std::launch::async, fetch_htx, 20);
        auto f_bingx = std::async(std::launch::async, fetch_bingx, 20);
        
        std::map<std::string, std::vector<ExchangeMarketData>> results;
        
        auto binance_data = f_binance.get();
        if (!binance_data.empty()) {
            results["binance"] = binance_data;
            std::cout << "  [✓] Binance: " << binance_data.size() << " markets\n";
        }
        
        auto bybit_data = f_bybit.get();
        if (!bybit_data.empty()) {
            results["bybit"] = bybit_data;
            std::cout << "  [✓] Bybit: " << bybit_data.size() << " markets\n";
        }
        
        auto okx_data = f_okx.get();
        if (!okx_data.empty()) {
            results["okx"] = okx_data;
            std::cout << "  [✓] OKX: " << okx_data.size() << " markets\n";
        }
        
        auto gateio_data = f_gateio.get();
        if (!gateio_data.empty()) {
            results["gateio"] = gateio_data;
            std::cout << "  [✓] Gate.io: " << gateio_data.size() << " markets\n";
        }
        
        auto mexc_data = f_mexc.get();
        if (!mexc_data.empty()) {
            results["mexc"] = mexc_data;
            std::cout << "  [✓] MEXC: " << mexc_data.size() << " markets\n";
        }
        
        auto kucoin_data = f_kucoin.get();
        if (!kucoin_data.empty()) {
            results["kucoin"] = kucoin_data;
            std::cout << "  [✓] KuCoin: " << kucoin_data.size() << " markets\n";
        }
        
        auto kraken_data = f_kraken.get();
        if (!kraken_data.empty()) {
            results["kraken"] = kraken_data;
            std::cout << "  [✓] Kraken: " << kraken_data.size() << " markets\n";
        }
        
        auto bitget_data = f_bitget.get();
        if (!bitget_data.empty()) {
            results["bitget"] = bitget_data;
            std::cout << "  [✓] Bitget: " << bitget_data.size() << " markets\n";
        }
        
        auto htx_data = f_htx.get();
        if (!htx_data.empty()) {
            results["htx"] = htx_data;
            std::cout << "  [✓] HTX: " << htx_data.size() << " markets\n";
        }
        
        auto bingx_data = f_bingx.get();
        if (!bingx_data.empty()) {
            results["bingx"] = bingx_data;
            std::cout << "  [✓] BingX: " << bingx_data.size() << " markets\n";
        }
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cache_ = results;
            last_update_ = std::chrono::system_clock::now();
        }
        
        int total = 0;
        for (const auto& [ex, data] : results) {
            total += data.size();
        }
        
        std::cout << "[FETCHER] Total: " << total << " markets from " 
                  << results.size() << " exchanges\n";
        
        return results;
    }
    
    std::map<std::string, std::vector<ExchangeMarketData>> get_cached() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        return cache_;
    }
};

} // namespace arb
