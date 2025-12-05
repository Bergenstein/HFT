#pragma once

//==============================================================================
// FUNDING RATE FETCHER
//==============================================================================
// Fetches funding rates from exchange REST APIs:
//   - Binance: GET /fapi/v1/premiumIndex
//   - Bybit: GET /v5/market/tickers
//   - OKX: GET /api/v5/public/funding-rate
//==============================================================================

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <optional>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>

// For HTTPS requests
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace arb {

using json = nlohmann::json;

//==============================================================================
// REAL FUNDING RATE DATA
//==============================================================================

struct RealFundingRate {
    std::string exchange;
    std::string symbol;
    double funding_rate;          // Current 8-hour rate (e.g., 0.0001 = 0.01%)
    double predicted_rate;        // Predicted next funding
    double mark_price;            // Current mark price
    double index_price;           // Spot index price
    int64_t next_funding_time_ms; // Unix timestamp of next funding
    int64_t timestamp_ms;         // When this data was fetched
    
    double hours_until_funding() const {
        auto now = std::chrono::system_clock::now();
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        return static_cast<double>(next_funding_time_ms - now_ms) / (1000.0 * 3600.0);
    }
    
    // APY calculation (for reference only, NOT used in strategy metrics)
    double annualized_rate() const {
        return funding_rate * 3 * 365 * 100; // 3 fundings/day, as percentage
    }
};

//==============================================================================
// EXCHANGE FEE STRUCTURE (REAL FEES)
//==============================================================================

struct ExchangeFees {
    double maker_fee;  // e.g., 0.0002 = 0.02%
    double taker_fee;  // e.g., 0.0004 = 0.04%
    
    static ExchangeFees binance() { return {0.0002, 0.0004}; }  // VIP0
    static ExchangeFees bybit()   { return {0.0002, 0.00055}; } // VIP0
    static ExchangeFees okx()     { return {0.0002, 0.0005}; }  // Tier 1
    static ExchangeFees dydx()    { return {0.0002, 0.0005}; }
    
    static ExchangeFees get(const std::string& exchange) {
        if (exchange == "binance") return binance();
        if (exchange == "bybit") return bybit();
        if (exchange == "okx") return okx();
        if (exchange == "dydx") return dydx();
        return {0.0004, 0.0006}; // Conservative default
    }
};

//==============================================================================
// HTTPS CLIENT (Simple implementation for API calls)
//==============================================================================

class SimpleHTTPSClient {
public:
    static std::optional<std::string> get(const std::string& host, const std::string& path) {
        // Initialize SSL
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        
        const SSL_METHOD* method = TLS_client_method();
        SSL_CTX* ctx = SSL_CTX_new(method);
        if (!ctx) {
            std::cerr << "[HTTPS] Failed to create SSL context\n";
            return std::nullopt;
        }
        
        // Create socket
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            SSL_CTX_free(ctx);
            return std::nullopt;
        }
        
        // Resolve hostname
        struct hostent* server = gethostbyname(host.c_str());
        if (!server) {
            close(sockfd);
            SSL_CTX_free(ctx);
            return std::nullopt;
        }
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(443);
        memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);
        
        // Connect
        if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sockfd);
            SSL_CTX_free(ctx);
            return std::nullopt;
        }
        
        // SSL handshake
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, sockfd);
        SSL_set_tlsext_host_name(ssl, host.c_str());
        
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl);
            close(sockfd);
            SSL_CTX_free(ctx);
            return std::nullopt;
        }
        
        // Build and send request
        std::string request = "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + "\r\n";
        request += "Connection: close\r\n";
        request += "User-Agent: HFT-System/1.0\r\n";
        request += "\r\n";
        
        SSL_write(ssl, request.c_str(), request.length());
        
        // Read response
        std::string response;
        char buffer[4096];
        int bytes;
        while ((bytes = SSL_read(ssl, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes] = '\0';
            response += buffer;
        }
        
        // Cleanup
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(sockfd);
        SSL_CTX_free(ctx);
        
        // Extract body (after headers)
        auto body_start = response.find("\r\n\r\n");
        if (body_start == std::string::npos) {
            return std::nullopt;
        }
        
        return response.substr(body_start + 4);
    }
};

//==============================================================================
// REAL FUNDING RATE FETCHER
//==============================================================================

class RealFundingRateFetcher {
public:
    //--------------------------------------------------------------------------
    // Fetch from Binance Futures
    // API: GET https://fapi.binance.com/fapi/v1/premiumIndex?symbol=BTCUSDT
    //--------------------------------------------------------------------------
    static std::optional<RealFundingRate> fetch_binance(const std::string& symbol) {
        std::string path = "/fapi/v1/premiumIndex?symbol=" + symbol;
        auto response = SimpleHTTPSClient::get("fapi.binance.com", path);
        
        if (!response) {
            std::cerr << "[BINANCE] Failed to fetch funding rate for " << symbol << "\n";
            return std::nullopt;
        }
        
        try {
            json j = json::parse(*response);
            
            RealFundingRate rate;
            rate.exchange = "binance";
            rate.symbol = symbol;
            rate.funding_rate = std::stod(j["lastFundingRate"].get<std::string>());
            rate.mark_price = std::stod(j["markPrice"].get<std::string>());
            rate.index_price = std::stod(j["indexPrice"].get<std::string>());
            rate.next_funding_time_ms = j["nextFundingTime"].get<int64_t>();
            rate.predicted_rate = rate.funding_rate; // Binance doesn't provide predicted
            rate.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            return rate;
        } catch (const std::exception& e) {
            std::cerr << "[BINANCE] Parse error: " << e.what() << "\n";
            return std::nullopt;
        }
    }
    
    //--------------------------------------------------------------------------
    // Fetch from Bybit
    // API: GET https://api.bybit.com/v5/market/tickers?category=linear&symbol=BTCUSDT
    //--------------------------------------------------------------------------
    static std::optional<RealFundingRate> fetch_bybit(const std::string& symbol) {
        std::string path = "/v5/market/tickers?category=linear&symbol=" + symbol;
        auto response = SimpleHTTPSClient::get("api.bybit.com", path);
        
        if (!response) {
            std::cerr << "[BYBIT] Failed to fetch funding rate for " << symbol << "\n";
            return std::nullopt;
        }
        
        try {
            json j = json::parse(*response);
            
            if (j["retCode"].get<int>() != 0) {
                std::cerr << "[BYBIT] API error: " << j["retMsg"].get<std::string>() << "\n";
                return std::nullopt;
            }
            
            const auto& list = j["result"]["list"];
            if (list.empty()) {
                return std::nullopt;
            }
            
            const auto& data = list[0];
            
            RealFundingRate rate;
            rate.exchange = "bybit";
            rate.symbol = symbol;
            rate.funding_rate = std::stod(data["fundingRate"].get<std::string>());
            rate.mark_price = std::stod(data["markPrice"].get<std::string>());
            rate.index_price = std::stod(data["indexPrice"].get<std::string>());
            rate.next_funding_time_ms = std::stoll(data["nextFundingTime"].get<std::string>());
            rate.predicted_rate = rate.funding_rate;
            rate.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            return rate;
        } catch (const std::exception& e) {
            std::cerr << "[BYBIT] Parse error: " << e.what() << "\n";
            return std::nullopt;
        }
    }
    
    //--------------------------------------------------------------------------
    // Fetch from OKX
    // API: GET https://www.okx.com/api/v5/public/funding-rate?instId=BTC-USDT-SWAP
    //--------------------------------------------------------------------------
    static std::optional<RealFundingRate> fetch_okx(const std::string& symbol) {
        // Convert symbol format: BTCUSDT -> BTC-USDT-SWAP
        std::string inst_id = symbol;
        if (symbol.find("-") == std::string::npos) {
            // Convert BTCUSDT to BTC-USDT-SWAP
            size_t pos = symbol.find("USDT");
            if (pos != std::string::npos) {
                inst_id = symbol.substr(0, pos) + "-USDT-SWAP";
            }
        }
        
        std::string path = "/api/v5/public/funding-rate?instId=" + inst_id;
        auto response = SimpleHTTPSClient::get("www.okx.com", path);
        
        if (!response) {
            std::cerr << "[OKX] Failed to fetch funding rate for " << symbol << "\n";
            return std::nullopt;
        }
        
        try {
            json j = json::parse(*response);
            
            if (j["code"].get<std::string>() != "0") {
                std::cerr << "[OKX] API error: " << j["msg"].get<std::string>() << "\n";
                return std::nullopt;
            }
            
            const auto& data = j["data"];
            if (data.empty()) {
                return std::nullopt;
            }
            
            const auto& item = data[0];
            
            RealFundingRate rate;
            rate.exchange = "okx";
            rate.symbol = symbol;
            rate.funding_rate = std::stod(item["fundingRate"].get<std::string>());
            rate.next_funding_time_ms = std::stoll(item["nextFundingTime"].get<std::string>());
            rate.predicted_rate = rate.funding_rate;
            rate.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            
            // OKX funding endpoint doesn't include mark/index price
            // Would need separate API call for that
            rate.mark_price = 0;
            rate.index_price = 0;
            
            return rate;
        } catch (const std::exception& e) {
            std::cerr << "[OKX] Parse error: " << e.what() << "\n";
            return std::nullopt;
        }
    }
    
    //--------------------------------------------------------------------------
    // Fetch from all exchanges
    //--------------------------------------------------------------------------
    static std::vector<RealFundingRate> fetch_all(const std::string& symbol) {
        std::vector<RealFundingRate> rates;
        
        std::cout << "[FETCHING] Real funding rates for " << symbol << "...\n";
        
        if (auto r = fetch_binance(symbol)) {
            rates.push_back(*r);
            std::cout << "  [BINANCE] " << symbol << ": " 
                      << std::fixed << std::setprecision(4) << (r->funding_rate * 100) << "%\n";
        }
        
        if (auto r = fetch_bybit(symbol)) {
            rates.push_back(*r);
            std::cout << "  [BYBIT]   " << symbol << ": " 
                      << std::fixed << std::setprecision(4) << (r->funding_rate * 100) << "%\n";
        }
        
        if (auto r = fetch_okx(symbol)) {
            rates.push_back(*r);
            std::cout << "  [OKX]     " << symbol << ": " 
                      << std::fixed << std::setprecision(4) << (r->funding_rate * 100) << "%\n";
        }
        
        return rates;
    }
    
    //--------------------------------------------------------------------------
    // Find arbitrage opportunity from REAL rates
    //--------------------------------------------------------------------------
    static void print_arbitrage_opportunity(const std::vector<RealFundingRate>& rates) {
        if (rates.size() < 2) {
            std::cout << "[ARB] Need at least 2 exchanges for arbitrage\n";
            return;
        }
        
        // Find max and min funding rates
        const RealFundingRate* max_rate = &rates[0];
        const RealFundingRate* min_rate = &rates[0];
        
        for (const auto& r : rates) {
            if (r.funding_rate > max_rate->funding_rate) max_rate = &r;
            if (r.funding_rate < min_rate->funding_rate) min_rate = &r;
        }
        
        double diff = max_rate->funding_rate - min_rate->funding_rate;
        double diff_bps = diff * 10000;
        
        // Get real fees
        auto fees_long = ExchangeFees::get(min_rate->exchange);
        auto fees_short = ExchangeFees::get(max_rate->exchange);
        
        // Calculate real P&L per $100k position
        double position_size = 100000.0;
        
        // Funding P&L per 8 hours
        // Long on min_rate exchange: we PAY min_rate (if positive)
        // Short on max_rate exchange: we RECEIVE max_rate (if positive)
        double funding_pnl_per_period = diff * position_size;
        
        // Entry fees (use maker on one leg, taker on other for immediacy)
        double entry_fee_long = position_size * fees_long.maker_fee;   // Limit order
        double entry_fee_short = position_size * fees_short.taker_fee; // Market order for immediacy
        double total_entry_fees = entry_fee_long + entry_fee_short;
        
        // Exit fees (same structure)
        double total_exit_fees = total_entry_fees;
        
        // Break-even periods
        double breakeven_periods = (total_entry_fees + total_exit_fees) / funding_pnl_per_period;
        
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                    REAL FUNDING RATE ARBITRAGE OPPORTUNITY                   ║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ LONG  " << std::left << std::setw(8) << min_rate->exchange 
                  << " | Funding: " << std::right << std::setw(8) << std::fixed << std::setprecision(4) 
                  << (min_rate->funding_rate * 100) << "% | Fee: " << (fees_long.maker_fee * 100) << "% (maker)     ║\n";
        std::cout << "║ SHORT " << std::left << std::setw(8) << max_rate->exchange 
                  << " | Funding: " << std::right << std::setw(8) << std::fixed << std::setprecision(4) 
                  << (max_rate->funding_rate * 100) << "% | Fee: " << (fees_short.taker_fee * 100) << "% (taker)     ║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ Funding Differential: " << std::setw(8) << std::setprecision(2) << diff_bps << " bps" 
                  << std::string(43, ' ') << "║\n";
        std::cout << "║ Position Size:        $" << std::setw(10) << std::setprecision(0) << position_size 
                  << std::string(42, ' ') << "║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ Funding P&L (8hr):    $" << std::setw(10) << std::setprecision(2) << funding_pnl_per_period
                  << std::string(42, ' ') << "║\n";
        std::cout << "║ Entry Fees:           $" << std::setw(10) << total_entry_fees 
                  << std::string(42, ' ') << "║\n";
        std::cout << "║ Exit Fees:            $" << std::setw(10) << total_exit_fees 
                  << std::string(42, ' ') << "║\n";
        std::cout << "║ Break-even Periods:   " << std::setw(10) << std::setprecision(1) << breakeven_periods << " (8hr periods)"
                  << std::string(30, ' ') << "║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        
        if (diff_bps >= 5 && breakeven_periods < 3) {
            std::cout << "║ ✅ PROFITABLE OPPORTUNITY - Enter trade                                      ║\n";
        } else if (diff_bps >= 3) {
            std::cout << "║ ⚠️  MARGINAL OPPORTUNITY - Monitor closely                                   ║\n";
        } else {
            std::cout << "║ ❌ NO OPPORTUNITY - Spread too narrow                                        ║\n";
        }
        
        std::cout << "╠══════════════════════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ EXECUTION STRATEGY:                                                          ║\n";
        std::cout << "║   1. Place LIMIT order on " << std::left << std::setw(8) << min_rate->exchange 
                  << " (maker fee, leg that pays)                   ║\n";
        std::cout << "║   2. Once filled, immediately MARKET order on " << std::setw(8) << max_rate->exchange << "           ║\n";
        std::cout << "║   3. If queue jumped on leg 1, cancel and re-enter at new top                ║\n";
        std::cout << "║   4. Exit before funding if rate converges                                   ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════════════════════╝\n";
    }
};

} // namespace arb
