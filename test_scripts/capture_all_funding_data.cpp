#include "arb/multi_exchange_l2_fetcher.hpp"
#include <iostream>
#include <fstream>
#include <map>
#include <algorithm>

using namespace arb;

int main() {
    std::cout << "\n╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  COMPREHENSIVE FUNDING DATA CAPTURE - ALL EXCHANGES      ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";
    
    auto start = std::chrono::steady_clock::now();
    
    auto f_binance = std::async(std::launch::async, MultiExchangeL2Fetcher::fetch_binance, 100);
    auto f_bybit = std::async(std::launch::async, MultiExchangeL2Fetcher::fetch_bybit, 100);
    auto f_okx = std::async(std::launch::async, MultiExchangeL2Fetcher::fetch_okx, 50);
    auto f_gateio = std::async(std::launch::async, MultiExchangeL2Fetcher::fetch_gateio, 50);
    auto f_mexc = std::async(std::launch::async, MultiExchangeL2Fetcher::fetch_mexc, 50);
    auto f_kucoin = std::async(std::launch::async, MultiExchangeL2Fetcher::fetch_kucoin, 50);
    auto f_kraken = std::async(std::launch::async, MultiExchangeL2Fetcher::fetch_kraken, 50);
    auto f_bitget = std::async(std::launch::async, MultiExchangeL2Fetcher::fetch_bitget, 50);
    auto f_htx = std::async(std::launch::async, MultiExchangeL2Fetcher::fetch_htx, 50);
    auto f_bingx = std::async(std::launch::async, MultiExchangeL2Fetcher::fetch_bingx, 50);
    
    std::map<std::string, std::vector<ExchangeMarketData>> all_data;
    
    auto binance = f_binance.get();
    if (!binance.empty()) { std::cout << "[✓] Binance: " << binance.size() << " markets\n"; all_data["binance"] = binance; }
    
    auto bybit = f_bybit.get();
    if (!bybit.empty()) { std::cout << "[✓] Bybit: " << bybit.size() << " markets\n"; all_data["bybit"] = bybit; }
    
    auto okx = f_okx.get();
    if (!okx.empty()) { std::cout << "[✓] OKX: " << okx.size() << " markets\n"; all_data["okx"] = okx; }
    
    auto gateio = f_gateio.get();
    if (!gateio.empty()) { std::cout << "[✓] Gate.io: " << gateio.size() << " markets\n"; all_data["gateio"] = gateio; }
    
    auto mexc = f_mexc.get();
    if (!mexc.empty()) { std::cout << "[✓] MEXC: " << mexc.size() << " markets\n"; all_data["mexc"] = mexc; }
    
    auto kucoin = f_kucoin.get();
    if (!kucoin.empty()) { std::cout << "[✓] KuCoin: " << kucoin.size() << " markets\n"; all_data["kucoin"] = kucoin; }
    
    auto kraken = f_kraken.get();
    if (!kraken.empty()) { std::cout << "[✓] Kraken: " << kraken.size() << " markets\n"; all_data["kraken"] = kraken; }
    
    auto bitget = f_bitget.get();
    if (!bitget.empty()) { std::cout << "[✓] Bitget: " << bitget.size() << " markets\n"; all_data["bitget"] = bitget; }
    
    auto htx = f_htx.get();
    if (!htx.empty()) { std::cout << "[✓] HTX: " << htx.size() << " markets\n"; all_data["htx"] = htx; }
    
    auto bingx = f_bingx.get();
    if (!bingx.empty()) { std::cout << "[✓] BingX: " << bingx.size() << " markets\n"; all_data["bingx"] = bingx; }
    
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
    std::cout << "\n[DONE] Fetched in " << elapsed << " seconds\n\n";
    
    // Export CSV
    std::ofstream csv("data/all_funding_snapshot.csv");
    csv << "exchange,symbol,funding_rate_8h,funding_rate_annual_pct,mark_price\n";
    
    int total_markets = 0, total_with_funding = 0;
    std::map<std::string, std::vector<std::pair<std::string, const FundingRateData*>>> by_symbol;
    
    for (const auto& [exchange, markets] : all_data) {
        for (const auto& market : markets) {
            total_markets++;
            if (market.funding) {
                total_with_funding++;
                csv << exchange << "," << market.funding->symbol << ","
                    << std::fixed << std::setprecision(6) << market.funding->funding_rate << ","
                    << std::setprecision(2) << market.funding->funding_rate_annual << ","
                    << market.funding->mark_price << "\n";
                
                std::string norm = market.funding->normalized_symbol;
                by_symbol[norm].push_back({exchange, &(*market.funding)});
            }
        }
    }
    csv.close();
    
    // Find opportunities
    std::vector<std::tuple<std::string, double, std::string, double, std::string, double>> opportunities;
    
    for (const auto& [symbol, exchanges] : by_symbol) {
        if (exchanges.size() < 2) continue;
        
        for (size_t i = 0; i < exchanges.size(); ++i) {
            for (size_t j = i + 1; j < exchanges.size(); ++j) {
                double rate_i = exchanges[i].second->funding_rate_annual;
                double rate_j = exchanges[j].second->funding_rate_annual;
                double spread = std::abs(rate_i - rate_j);
                
                if (spread > 15.0) {
                    if (rate_i < rate_j) {
                        opportunities.push_back({symbol, spread, exchanges[i].first, rate_i, exchanges[j].first, rate_j});
                    } else {
                        opportunities.push_back({symbol, spread, exchanges[j].first, rate_j, exchanges[i].first, rate_i});
                    }
                }
            }
        }
    }
    
    std::sort(opportunities.begin(), opportunities.end(),
        [](const auto& a, const auto& b) { return std::get<1>(a) > std::get<1>(b); });
    
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    SUMMARY                               ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
    std::cout << "Exchanges connected: " << all_data.size() << "\n";
    std::cout << "Total markets: " << total_markets << "\n";
    std::cout << "Markets with funding: " << total_with_funding << "\n";
    std::cout << "Unique symbols: " << by_symbol.size() << "\n";
    std::cout << "Opportunities >15% APY: " << opportunities.size() << "\n\n";
    
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║            TOP 30 ARBITRAGE OPPORTUNITIES                ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";
    
    int shown = 0;
    for (const auto& [symbol, spread, long_ex, long_rate, short_ex, short_rate] : opportunities) {
        if (++shown > 30) break;
        std::cout << shown << ". " << symbol << ": " << std::fixed << std::setprecision(2) << spread << "% APY\n";
        std::cout << "   Long:  " << long_ex << " @ " << long_rate << "% APY\n";
        std::cout << "   Short: " << short_ex << " @ " << short_rate << "% APY\n\n";
    }
    
    std::cout << "✓ Data saved to: data/all_funding_snapshot.csv\n\n";
    
    return 0;
}
