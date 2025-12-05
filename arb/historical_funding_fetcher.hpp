#pragma once

//==============================================================================
// HISTORICAL FUNDING RATE FETCHER
//==============================================================================
// Fetches historical funding rates from exchange REST APIs:
//   - Binance: GET /fapi/v1/fundingRate
//   - Bybit: GET /v5/market/funding/history  
//   - OKX: GET /api/v5/public/funding-rate-history
//==============================================================================

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>

#include "real_funding_rate_fetcher.hpp"

namespace arb {

//==============================================================================
// HISTORICAL FUNDING DATA
//==============================================================================

struct HistoricalFundingRate {
    std::string exchange;
    std::string symbol;
    double funding_rate;          // 8-hour rate (e.g., 0.0001 = 0.01%)
    int64_t funding_time_ms;      // When this funding was paid
    
    // For CSV storage
    std::string to_csv() const {
        std::ostringstream oss;
        oss << exchange << "," << symbol << "," 
            << std::fixed << std::setprecision(8) << funding_rate << ","
            << funding_time_ms;
        return oss.str();
    }
    
    static HistoricalFundingRate from_csv(const std::string& line) {
        HistoricalFundingRate r;
        std::istringstream iss(line);
        std::string token;
        
        std::getline(iss, r.exchange, ',');
        std::getline(iss, r.symbol, ',');
        std::getline(iss, token, ',');
        r.funding_rate = std::stod(token);
        std::getline(iss, token, ',');
        r.funding_time_ms = std::stoll(token);
        
        return r;
    }
};

//==============================================================================
// HISTORICAL FUNDING RATE FETCHER
//==============================================================================

class HistoricalFundingFetcher {
public:
    //--------------------------------------------------------------------------
    // Fetch historical funding from Binance
    // API: GET https://fapi.binance.com/fapi/v1/fundingRate?symbol=BTCUSDT&limit=1000
    // Returns up to 1000 records, most recent first
    //--------------------------------------------------------------------------
    static std::vector<HistoricalFundingRate> fetch_binance_history(
        const std::string& symbol,
        int limit = 1000
    ) {
        std::vector<HistoricalFundingRate> rates;
        
        std::string path = "/fapi/v1/fundingRate?symbol=" + symbol + 
                          "&limit=" + std::to_string(limit);
        auto response = SimpleHTTPSClient::get("fapi.binance.com", path);
        
        if (!response) {
            return rates;
        }
        
        try {
            json j = json::parse(*response);
            
            for (const auto& item : j) {
                HistoricalFundingRate rate;
                rate.exchange = "binance";
                rate.symbol = symbol;
                rate.funding_rate = std::stod(item["fundingRate"].get<std::string>());
                rate.funding_time_ms = item["fundingTime"].get<int64_t>();
                rates.push_back(rate);
            }
            
        } catch (const std::exception&) {
            // Parse error - return empty
        }
        
        return rates;
    }
    
    //--------------------------------------------------------------------------
    // Fetch historical funding from Bybit
    // API: GET https://api.bybit.com/v5/market/funding/history?category=linear&symbol=BTCUSDT&limit=200
    // Returns up to 200 records per request
    //--------------------------------------------------------------------------
    static std::vector<HistoricalFundingRate> fetch_bybit_history(
        const std::string& symbol,
        int limit = 200
    ) {
        std::vector<HistoricalFundingRate> rates;
        
        std::string path = "/v5/market/funding/history?category=linear&symbol=" + symbol +
                          "&limit=" + std::to_string(std::min(limit, 200));
        auto response = SimpleHTTPSClient::get("api.bybit.com", path);
        
        if (!response) {
            return rates;
        }
        
        try {
            json j = json::parse(*response);
            
            if (j["retCode"].get<int>() != 0) {
                return rates;
            }
            
            const auto& list = j["result"]["list"];
            
            for (const auto& item : list) {
                HistoricalFundingRate rate;
                rate.exchange = "bybit";
                rate.symbol = symbol;
                rate.funding_rate = std::stod(item["fundingRate"].get<std::string>());
                rate.funding_time_ms = std::stoll(item["fundingRateTimestamp"].get<std::string>());
                rates.push_back(rate);
            }
        } catch (const std::exception&) {
            // Parse error
        }
        
        return rates;
    }
    
    //--------------------------------------------------------------------------
    // Fetch historical funding from OKX
    // API: GET https://www.okx.com/api/v5/public/funding-rate-history?instId=BTC-USDT-SWAP&limit=100
    // Returns up to 100 records per request
    //--------------------------------------------------------------------------
    static std::vector<HistoricalFundingRate> fetch_okx_history(
        const std::string& symbol,
        int limit = 100
    ) {
        std::vector<HistoricalFundingRate> rates;
        
        // Convert symbol format: BTCUSDT -> BTC-USDT-SWAP
        std::string inst_id = symbol;
        if (symbol.find("-") == std::string::npos) {
            size_t pos = symbol.find("USDT");
            if (pos != std::string::npos) {
                inst_id = symbol.substr(0, pos) + "-USDT-SWAP";
            }
        }
        
        std::string path = "/api/v5/public/funding-rate-history?instId=" + inst_id +
                          "&limit=" + std::to_string(std::min(limit, 100));
        auto response = SimpleHTTPSClient::get("www.okx.com", path);
        
        if (!response) {
            return rates;
        }
        
        try {
            json j = json::parse(*response);
            
            if (j["code"].get<std::string>() != "0") {
                return rates;
            }
            
            const auto& data = j["data"];
            
            for (const auto& item : data) {
                HistoricalFundingRate rate;
                rate.exchange = "okx";
                rate.symbol = symbol;
                rate.funding_rate = std::stod(item["fundingRate"].get<std::string>());
                rate.funding_time_ms = std::stoll(item["fundingTime"].get<std::string>());
                rates.push_back(rate);
            }
        } catch (const std::exception&) {
            // Parse error
        }
        
        return rates;
    }
    
    //--------------------------------------------------------------------------
    // Fetch from all exchanges and merge
    //--------------------------------------------------------------------------
    static std::vector<HistoricalFundingRate> fetch_all_history(const std::string& symbol) {
        std::vector<HistoricalFundingRate> all;
        
        auto binance = fetch_binance_history(symbol);
        all.insert(all.end(), binance.begin(), binance.end());
        
        auto bybit = fetch_bybit_history(symbol);
        all.insert(all.end(), bybit.begin(), bybit.end());
        
        auto okx = fetch_okx_history(symbol);
        all.insert(all.end(), okx.begin(), okx.end());
        
        // Sort by time
        std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
            return a.funding_time_ms < b.funding_time_ms;
        });
        
        return all;
    }
    
    //--------------------------------------------------------------------------
    // Save to CSV file
    //--------------------------------------------------------------------------
    static bool save_to_csv(const std::vector<HistoricalFundingRate>& rates, 
                           const std::string& filepath) {
        std::ofstream file(filepath);
        if (!file.is_open()) {
            return false;
        }
        
        file << "exchange,symbol,funding_rate,funding_time_ms\n";
        for (const auto& r : rates) {
            file << r.to_csv() << "\n";
        }
        
        return true;
    }
    
    //--------------------------------------------------------------------------
    // Load from CSV file
    //--------------------------------------------------------------------------
    static std::vector<HistoricalFundingRate> load_from_csv(const std::string& filepath) {
        std::vector<HistoricalFundingRate> rates;
        std::ifstream file(filepath);
        
        if (!file.is_open()) {
            return rates;
        }
        
        std::string line;
        std::getline(file, line);  // Skip header
        
        while (std::getline(file, line)) {
            if (!line.empty()) {
                rates.push_back(HistoricalFundingRate::from_csv(line));
            }
        }
        
        return rates;
    }
};

//==============================================================================
// HISTORICAL FUNDING RATE BACKTEST (REAL DATA)
//==============================================================================

struct HistoricalArbBacktestResult {
    std::string symbol;
    int num_periods;
    int num_trades;
    double total_pnl;
    double total_fees;
    double net_pnl;
    double avg_funding_diff_bps;
    double win_rate;
    double max_drawdown;
    
    // NOT annualized metrics
    double daily_return;
    double daily_sharpe;
    double daily_sortino;
    
    void print() const {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Symbol: " << symbol << "\n";
        std::cout << "Periods: " << num_periods << "\n";
        std::cout << "Trades: " << num_trades << "\n";
        std::cout << "P&L: $" << net_pnl << "\n";
        std::cout << "Daily Sharpe: " << std::setprecision(3) << daily_sharpe << "\n";
    }
};

class HistoricalFundingBacktester {
public:
    // Backtest funding rate arbitrage using REAL historical data
    static HistoricalArbBacktestResult backtest(
        const std::vector<HistoricalFundingRate>& all_rates,
        const std::string& symbol,
        double position_size = 100000.0,
        double min_diff_bps = 3.0  // Minimum diff to enter trade
    ) {
        HistoricalArbBacktestResult result;
        result.symbol = symbol;
        result.num_periods = 0;
        result.num_trades = 0;
        result.total_pnl = 0;
        result.total_fees = 0;
        
        // Group rates by funding time
        std::map<int64_t, std::vector<HistoricalFundingRate>> by_time;
        for (const auto& r : all_rates) {
            if (r.symbol == symbol) {
                // Round to nearest 8-hour period
                int64_t period = (r.funding_time_ms / (8 * 3600 * 1000)) * (8 * 3600 * 1000);
                by_time[period].push_back(r);
            }
        }
        
        std::vector<double> daily_pnls;
        double running_pnl = 0;
        double peak_pnl = 0;
        double max_dd = 0;
        int wins = 0;
        double total_diff = 0;
        
        int64_t last_day = 0;
        double day_pnl = 0;
        
        for (const auto& [time, rates] : by_time) {
            if (rates.size() < 2) continue;
            
            // Find max and min funding rate for this period
            double max_rate = rates[0].funding_rate;
            double min_rate = rates[0].funding_rate;
            std::string max_ex = rates[0].exchange;
            std::string min_ex = rates[0].exchange;
            
            for (const auto& r : rates) {
                if (r.funding_rate > max_rate) {
                    max_rate = r.funding_rate;
                    max_ex = r.exchange;
                }
                if (r.funding_rate < min_rate) {
                    min_rate = r.funding_rate;
                    min_ex = r.exchange;
                }
            }
            
            double diff = max_rate - min_rate;
            double diff_bps = diff * 10000;
            
            if (diff_bps >= min_diff_bps) {
                // Execute trade
                double funding_pnl = diff * position_size;
                
                // Calculate fees (enter + exit)
                auto fees_long = ExchangeFees::get(min_ex);
                auto fees_short = ExchangeFees::get(max_ex);
                double entry_fee = position_size * (fees_long.maker_fee + fees_short.taker_fee);
                double exit_fee = entry_fee;  // Same structure
                
                // Net P&L for this period (assuming we hold just one period)
                double period_pnl = funding_pnl - entry_fee - exit_fee;
                
                result.total_pnl += funding_pnl;
                result.total_fees += entry_fee + exit_fee;
                result.num_trades++;
                total_diff += diff_bps;
                
                if (period_pnl > 0) wins++;
                
                running_pnl += period_pnl;
                if (running_pnl > peak_pnl) peak_pnl = running_pnl;
                double dd = peak_pnl - running_pnl;
                if (dd > max_dd) max_dd = dd;
                
                // Track daily P&L
                int64_t day = time / (24 * 3600 * 1000);
                if (day != last_day && last_day != 0) {
                    daily_pnls.push_back(day_pnl);
                    day_pnl = 0;
                }
                day_pnl += period_pnl;
                last_day = day;
            }
            
            result.num_periods++;
        }
        
        // Add last day
        if (day_pnl != 0) {
            daily_pnls.push_back(day_pnl);
        }
        
        result.net_pnl = result.total_pnl - result.total_fees;
        result.avg_funding_diff_bps = result.num_trades > 0 ? total_diff / result.num_trades : 0;
        result.win_rate = result.num_trades > 0 ? static_cast<double>(wins) / result.num_trades : 0;
        result.max_drawdown = max_dd;
        
        // Calculate daily metrics (NOT ANNUALIZED)
        if (!daily_pnls.empty()) {
            double sum = 0;
            for (double p : daily_pnls) sum += p;
            double mean = sum / daily_pnls.size();
            
            double variance = 0;
            double downside_variance = 0;
            for (double p : daily_pnls) {
                variance += (p - mean) * (p - mean);
                if (p < 0) downside_variance += p * p;
            }
            variance /= daily_pnls.size();
            downside_variance /= daily_pnls.size();
            
            double std_dev = std::sqrt(variance);
            double downside_std = std::sqrt(downside_variance);
            
            result.daily_return = mean / position_size;  // As fraction of capital
            result.daily_sharpe = std_dev > 0 ? mean / std_dev : 0;
            result.daily_sortino = downside_std > 0 ? mean / downside_std : 0;
        }
        
        return result;
    }
};

} // namespace arb
