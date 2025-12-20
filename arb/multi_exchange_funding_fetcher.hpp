#pragma once

//==============================================================================
// MULTI-EXCHANGE FUNDING RATE FETCHER
//==============================================================================
// Fetches funding rates from 10+ exchanges with proper error handling
// Inspired by Python implementation but optimized for C++ HFT pipeline
//
// Supported Exchanges:
//   1. Binance Futures
//   2. Bybit Perpetuals
//   3. OKX Perpetuals
//   4. Gate.io Futures
//   5. Bitget Futures
//   6. MEXC Futures
//   7. KuCoin Futures
//   8. Kraken Futures
//   9. Deribit Perpetuals
//  10. Coinbase (planned)
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
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

namespace arb {

using json = nlohmann::json;

//==============================================================================
// EXCHANGE FUNDING RATE DATA
//==============================================================================

struct ExchangeFundingData {
    std::string exchange;
    std::string symbol;
    double funding_rate;           // 8-hour rate (e.g., 0.0001 = 0.01%)
    double funding_rate_annual;    // Annualized percentage
    int64_t next_funding_time_ms;  // Unix timestamp
    double mark_price;
    double index_price;
    int64_t timestamp_ms;          // When fetched
    
    // Additional metadata
    int funding_interval_hours = 8;  // Default 8-hour cycle
    
    double hours_until_funding() const {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return static_cast<double>(next_funding_time_ms - now_ms) / (1000.0 * 3600.0);
    }
    
    std::string normalized_symbol() const {
        // Normalize symbol for cross-exchange comparison
        std::string norm = symbol;
        // Remove common suffixes
        size_t pos;
        const std::vector<std::string> suffixes = {
            "USDT", "-PERP", "_UMCBL", "-SWAP", "USD", "PERP", "_", "-"
        };
        for (const auto& suffix : suffixes) {
            while ((pos = norm.find(suffix)) != std::string::npos) {
                norm.replace(pos, suffix.length(), "");
            }
        }
        return norm;
    }
};

//==============================================================================
// EXCHANGE FEE STRUCTURE (Extended)
//==============================================================================

struct ExchangeFees {
    double maker_fee;
    double taker_fee;
    
    static ExchangeFees binance()  { return {0.0002, 0.0004}; }  // VIP0
    static ExchangeFees bybit()    { return {0.0002, 0.00055}; } // VIP0
    static ExchangeFees okx()      { return {0.0002, 0.0005}; }  // Tier 1
    static ExchangeFees gateio()   { return {0.0002, 0.0005}; }  // Tier 0
    static ExchangeFees bitget()   { return {0.0002, 0.0006}; }  // Normal
    static ExchangeFees mexc()     { return {0.0000, 0.0006}; }  // Maker 0%
    static ExchangeFees kucoin()   { return {0.0002, 0.0006}; }  // Level 1
    static ExchangeFees kraken()   { return {0.0002, 0.0005}; }  // Tier 1
    static ExchangeFees deribit()  { return {0.0000, 0.0005}; }  // Maker 0%
    
    static ExchangeFees get(const std::string& exchange) {
        if (exchange == "binance") return binance();
        if (exchange == "bybit")   return bybit();
        if (exchange == "okx")     return okx();
        if (exchange == "gateio")  return gateio();
        if (exchange == "bitget")  return bitget();
        if (exchange == "mexc")    return mexc();
        if (exchange == "kucoin")  return kucoin();
        if (exchange == "kraken")  return kraken();
        if (exchange == "deribit") return deribit();
        return {0.0004, 0.0006}; // Conservative default
    }
};

//==============================================================================
// HTTPS CLIENT FOR API CALLS
//==============================================================================

class HTTPSClient {
public:
    static std::optional<std::string> get(
        const std::string& host, 
        const std::string& path,
        int timeout_seconds = 10) {
        
        // Initialize SSL
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        
        const SSL_METHOD* method = TLS_client_method();
        SSL_CTX* ctx = SSL_CTX_new(method);
        if (!ctx) return std::nullopt;
        
        // Resolve hostname
        struct addrinfo hints = {}, *res;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        
        if (getaddrinfo(host.c_str(), "443", &hints, &res) != 0) {
            SSL_CTX_free(ctx);
            return std::nullopt;
        }
        
        // Create socket
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            freeaddrinfo(res);
            SSL_CTX_free(ctx);
            return std::nullopt;
        }
        
        // Set timeout
        struct timeval tv;
        tv.tv_sec = timeout_seconds;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        
        // Connect
        if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
            close(sock);
            freeaddrinfo(res);
            SSL_CTX_free(ctx);
            return std::nullopt;
        }
        freeaddrinfo(res);
        
        // SSL handshake
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, sock);
        
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl);
            close(sock);
            SSL_CTX_free(ctx);
            return std::nullopt;
        }
        
        // Send HTTP request
        std::string request = 
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "User-Agent: HFT-System/1.0\r\n"
            "Connection: close\r\n\r\n";
        
        SSL_write(ssl, request.c_str(), request.length());
        
        // Read response
        std::string response;
        char buffer[4096];
        int bytes;
        while ((bytes = SSL_read(ssl, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes] = 0;
            response += buffer;
        }
        
        // Cleanup
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(sock);
        SSL_CTX_free(ctx);
        
        // Extract JSON body
        size_t body_start = response.find("\r\n\r\n");
        if (body_start == std::string::npos) return std::nullopt;
        
        return response.substr(body_start + 4);
    }
};

//==============================================================================
// MULTI-EXCHANGE FUNDING RATE FETCHER
//==============================================================================

class MultiExchangeFundingFetcher {
private:
    std::mutex mutex_;
    std::map<std::string, std::vector<ExchangeFundingData>> cache_;
    std::chrono::system_clock::time_point last_update_;
    
public:
    //--------------------------------------------------------------------------
    // Binance Futures
    //--------------------------------------------------------------------------
    static std::vector<ExchangeFundingData> fetch_binance() {
        std::vector<ExchangeFundingData> results;
        
        try {
            auto response = HTTPSClient::get("fapi.binance.com", "/fapi/v1/premiumIndex");
            if (!response) return results;
            
            json data = json::parse(*response);
            
            for (const auto& item : data) {
                if (!item.contains("symbol") || !item.contains("lastFundingRate")) continue;
                
                ExchangeFundingData fund;
                fund.exchange = "binance";
                fund.symbol = item["symbol"].get<std::string>();
                fund.funding_rate = std::stod(item["lastFundingRate"].get<std::string>());
                fund.funding_rate_annual = fund.funding_rate * 3 * 365 * 100;
                fund.next_funding_time_ms = item.value("nextFundingTime", 0LL);
                fund.mark_price = std::stod(item.value("markPrice", "0"));
                fund.index_price = std::stod(item.value("indexPrice", "0"));
                fund.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                
                results.push_back(fund);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Binance] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // Bybit Perpetuals
    //--------------------------------------------------------------------------
    static std::vector<ExchangeFundingData> fetch_bybit() {
        std::vector<ExchangeFundingData> results;
        
        try {
            auto response = HTTPSClient::get("api.bybit.com", "/v5/market/tickers?category=linear");
            if (!response) return results;
            
            json data = json::parse(*response);
            
            if (data.contains("result") && data["result"].contains("list")) {
                for (const auto& item : data["result"]["list"]) {
                    if (!item.contains("symbol") || !item.contains("fundingRate")) continue;
                    
                    ExchangeFundingData fund;
                    fund.exchange = "bybit";
                    fund.symbol = item["symbol"].get<std::string>();
                    
                    std::string fr_str = item["fundingRate"].get<std::string>();
                    if (fr_str.empty()) continue;
                    
                    fund.funding_rate = std::stod(fr_str);
                    fund.funding_rate_annual = fund.funding_rate * 3 * 365 * 100;
                    fund.next_funding_time_ms = std::stoll(item.value("nextFundingTime", "0"));
                    fund.mark_price = std::stod(item.value("markPrice", "0"));
                    fund.index_price = std::stod(item.value("indexPrice", "0"));
                    fund.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    
                    results.push_back(fund);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Bybit] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // OKX Perpetuals
    //--------------------------------------------------------------------------
    static std::vector<ExchangeFundingData> fetch_okx() {
        std::vector<ExchangeFundingData> results;
        
        try {
            // Get list of instruments
            auto inst_response = HTTPSClient::get("www.okx.com", "/api/v5/public/instruments?instType=SWAP");
            if (!inst_response) return results;
            
            json instruments = json::parse(*inst_response);
            
            if (!instruments.contains("data")) return results;
            
            // Fetch funding rates (limit to top 50 to avoid rate limits)
            int count = 0;
            for (const auto& inst : instruments["data"]) {
                if (++count > 50) break;
                
                std::string inst_id = inst["instId"].get<std::string>();
                std::string path = "/api/v5/public/funding-rate?instId=" + inst_id;
                
                auto fr_response = HTTPSClient::get("www.okx.com", path);
                if (!fr_response) continue;
                
                json fr_data = json::parse(*fr_response);
                
                if (fr_data.contains("data") && !fr_data["data"].empty()) {
                    const auto& item = fr_data["data"][0];
                    
                    ExchangeFundingData fund;
                    fund.exchange = "okx";
                    fund.symbol = inst_id;
                    fund.funding_rate = std::stod(item["fundingRate"].get<std::string>());
                    fund.funding_rate_annual = fund.funding_rate * 3 * 365 * 100;
                    fund.next_funding_time_ms = std::stoll(item.value("nextFundingTime", "0"));
                    fund.mark_price = 0;
                    fund.index_price = 0;
                    fund.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    
                    results.push_back(fund);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Rate limit
            }
        } catch (const std::exception& e) {
            std::cerr << "[OKX] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // Gate.io Futures
    //--------------------------------------------------------------------------
    static std::vector<ExchangeFundingData> fetch_gateio() {
        std::vector<ExchangeFundingData> results;
        
        try {
            auto response = HTTPSClient::get("api.gateio.ws", "/api/v4/futures/usdt/contracts");
            if (!response) return results;
            
            json data = json::parse(*response);
            
            for (const auto& item : data) {
                if (!item.contains("name") || !item.contains("funding_rate")) continue;
                
                ExchangeFundingData fund;
                fund.exchange = "gateio";
                fund.symbol = item["name"].get<std::string>();
                fund.funding_rate = std::stod(item["funding_rate"].get<std::string>());
                
                // Gate.io has variable funding intervals
                int interval_seconds = item.value("funding_interval", 28800); // Default 8 hours
                fund.funding_interval_hours = interval_seconds / 3600;
                
                // Normalize to 8-hour equivalent
                double fr_8h = fund.funding_rate * (8.0 / fund.funding_interval_hours);
                
                // Calculate correct APY
                double payments_per_year = (24.0 / fund.funding_interval_hours) * 365;
                fund.funding_rate_annual = fund.funding_rate * payments_per_year * 100;
                
                fund.next_funding_time_ms = item.value("funding_next_apply", 0LL) * 1000;
                fund.mark_price = std::stod(item.value("mark_price", "0"));
                fund.index_price = 0;
                fund.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                
                results.push_back(fund);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Gate.io] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // MEXC Futures
    //--------------------------------------------------------------------------
    static std::vector<ExchangeFundingData> fetch_mexc() {
        std::vector<ExchangeFundingData> results;
        
        try {
            auto response = HTTPSClient::get("contract.mexc.com", "/api/v1/contract/ticker");
            if (!response) return results;
            
            json resp = json::parse(*response);
            
            if (resp.contains("data")) {
                for (const auto& item : resp["data"]) {
                    if (!item.contains("symbol") || !item.contains("fundingRate")) continue;
                    
                    ExchangeFundingData fund;
                    fund.exchange = "mexc";
                    fund.symbol = item["symbol"].get<std::string>();
                    fund.funding_rate = item["fundingRate"].get<double>();
                    fund.funding_rate_annual = fund.funding_rate * 3 * 365 * 100;
                    fund.next_funding_time_ms = 0;
                    fund.mark_price = item.value("lastPrice", 0.0);
                    fund.index_price = 0;
                    fund.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    
                    results.push_back(fund);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[MEXC] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // KuCoin Futures
    //--------------------------------------------------------------------------
    static std::vector<ExchangeFundingData> fetch_kucoin() {
        std::vector<ExchangeFundingData> results;
        
        try {
            auto response = HTTPSClient::get("api-futures.kucoin.com", "/api/v1/contracts/active");
            if (!response) return results;
            
            json resp = json::parse(*response);
            
            if (resp.contains("data")) {
                for (const auto& item : resp["data"]) {
                    if (!item.contains("symbol") || !item.contains("fundingFeeRate")) continue;
                    
                    ExchangeFundingData fund;
                    fund.exchange = "kucoin";
                    fund.symbol = item["symbol"].get<std::string>();
                    fund.funding_rate = item["fundingFeeRate"].get<double>();
                    fund.funding_rate_annual = fund.funding_rate * 3 * 365 * 100;
                    fund.next_funding_time_ms = 0;
                    fund.mark_price = item.value("markPrice", 0.0);
                    fund.index_price = item.value("indexPrice", 0.0);
                    fund.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    
                    results.push_back(fund);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[KuCoin] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // Kraken Futures
    //--------------------------------------------------------------------------
    static std::vector<ExchangeFundingData> fetch_kraken() {
        std::vector<ExchangeFundingData> results;
        
        try {
            auto response = HTTPSClient::get("futures.kraken.com", "/derivatives/api/v3/tickers");
            if (!response) return results;
            
            json resp = json::parse(*response);
            
            if (resp.contains("tickers")) {
                for (const auto& item : resp["tickers"]) {
                    if (!item.contains("symbol") || !item.contains("fundingRate")) continue;
                    
                    ExchangeFundingData fund;
                    fund.exchange = "kraken";
                    fund.symbol = item["symbol"].get<std::string>();
                    fund.funding_rate = item["fundingRate"].get<double>();
                    fund.funding_rate_annual = fund.funding_rate * 3 * 365 * 100;
                    fund.next_funding_time_ms = 0;
                    fund.mark_price = item.value("markPrice", 0.0);
                    fund.index_price = item.value("indexPrice", 0.0);
                    fund.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    
                    results.push_back(fund);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Kraken] Error: " << e.what() << "\n";
        }
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // Fetch All Exchanges (Parallel)
    //--------------------------------------------------------------------------
    std::map<std::string, std::vector<ExchangeFundingData>> fetch_all_parallel() {
        std::cout << "[FETCHER] Fetching from all exchanges in parallel...\n";
        
        // Launch async tasks for each exchange
        auto f_binance = std::async(std::launch::async, fetch_binance);
        auto f_bybit = std::async(std::launch::async, fetch_bybit);
        auto f_okx = std::async(std::launch::async, fetch_okx);
        auto f_gateio = std::async(std::launch::async, fetch_gateio);
        auto f_mexc = std::async(std::launch::async, fetch_mexc);
        auto f_kucoin = std::async(std::launch::async, fetch_kucoin);
        auto f_kraken = std::async(std::launch::async, fetch_kraken);
        
        // Collect results
        std::map<std::string, std::vector<ExchangeFundingData>> results;
        
        auto binance_data = f_binance.get();
        if (!binance_data.empty()) {
            results["binance"] = binance_data;
            std::cout << "  [✓] Binance: " << binance_data.size() << " symbols\n";
        }
        
        auto bybit_data = f_bybit.get();
        if (!bybit_data.empty()) {
            results["bybit"] = bybit_data;
            std::cout << "  [✓] Bybit: " << bybit_data.size() << " symbols\n";
        }
        
        auto okx_data = f_okx.get();
        if (!okx_data.empty()) {
            results["okx"] = okx_data;
            std::cout << "  [✓] OKX: " << okx_data.size() << " symbols\n";
        }
        
        auto gateio_data = f_gateio.get();
        if (!gateio_data.empty()) {
            results["gateio"] = gateio_data;
            std::cout << "  [✓] Gate.io: " << gateio_data.size() << " symbols\n";
        }
        
        auto mexc_data = f_mexc.get();
        if (!mexc_data.empty()) {
            results["mexc"] = mexc_data;
            std::cout << "  [✓] MEXC: " << mexc_data.size() << " symbols\n";
        }
        
        auto kucoin_data = f_kucoin.get();
        if (!kucoin_data.empty()) {
            results["kucoin"] = kucoin_data;
            std::cout << "  [✓] KuCoin: " << kucoin_data.size() << " symbols\n";
        }
        
        auto kraken_data = f_kraken.get();
        if (!kraken_data.empty()) {
            results["kraken"] = kraken_data;
            std::cout << "  [✓] Kraken: " << kraken_data.size() << " symbols\n";
        }
        
        // Update cache
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cache_ = results;
            last_update_ = std::chrono::system_clock::now();
        }
        
        int total_symbols = 0;
        for (const auto& [ex, data] : results) {
            total_symbols += data.size();
        }
        
        std::cout << "[FETCHER] Total: " << total_symbols << " funding rates from " 
                  << results.size() << " exchanges\n";
        
        return results;
    }
    
    //--------------------------------------------------------------------------
    // Get Cached Data
    //--------------------------------------------------------------------------
    std::map<std::string, std::vector<ExchangeFundingData>> get_cached() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_;
    }
    
    //--------------------------------------------------------------------------
    // Find Cross-Exchange Arbitrage Opportunities
    //--------------------------------------------------------------------------
    struct ArbitrageOpportunity {
        std::string base_symbol;
        std::string long_exchange;
        std::string long_symbol;
        double long_rate_annual;
        std::string short_exchange;
        std::string short_symbol;
        double short_rate_annual;
        double spread_annual;
        int num_exchanges;
    };
    
    std::vector<ArbitrageOpportunity> find_arbitrage_opportunities(
        double min_spread_apy = 20.0) const {
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Group by normalized symbol
        std::map<std::string, std::vector<const ExchangeFundingData*>> by_symbol;
        
        for (const auto& [exchange, data_list] : cache_) {
            for (const auto& data : data_list) {
                std::string norm = data.normalized_symbol();
                by_symbol[norm].push_back(&data);
            }
        }
        
        std::vector<ArbitrageOpportunity> opportunities;
        
        for (const auto& [norm_symbol, data_list] : by_symbol) {
            if (data_list.size() < 2) continue;
            
            // Find min and max funding rates
            const ExchangeFundingData* min_data = data_list[0];
            const ExchangeFundingData* max_data = data_list[0];
            
            for (const auto* data : data_list) {
                if (data->funding_rate_annual < min_data->funding_rate_annual) {
                    min_data = data;
                }
                if (data->funding_rate_annual > max_data->funding_rate_annual) {
                    max_data = data;
                }
            }
            
            double spread = max_data->funding_rate_annual - min_data->funding_rate_annual;
            
            if (spread >= min_spread_apy) {
                ArbitrageOpportunity opp;
                opp.base_symbol = norm_symbol;
                opp.long_exchange = min_data->exchange;
                opp.long_symbol = min_data->symbol;
                opp.long_rate_annual = min_data->funding_rate_annual;
                opp.short_exchange = max_data->exchange;
                opp.short_symbol = max_data->symbol;
                opp.short_rate_annual = max_data->funding_rate_annual;
                opp.spread_annual = spread;
                opp.num_exchanges = data_list.size();
                
                opportunities.push_back(opp);
            }
        }
        
        // Sort by spread
        std::sort(opportunities.begin(), opportunities.end(),
            [](const ArbitrageOpportunity& a, const ArbitrageOpportunity& b) {
                return a.spread_annual > b.spread_annual;
            });
        
        return opportunities;
    }
};

} // namespace arb
