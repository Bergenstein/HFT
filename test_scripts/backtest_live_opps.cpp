#include <iostream>
#include <vector>
#include <string>
#include <iomanip>

struct Opportunity {
    std::string symbol;
    double gross_apy;
    double liquidity;
    std::string long_ex;
    std::string short_ex;
};

int main() {
    std::vector<Opportunity> opps = {
        {"4/USDT", 72.97, 12808, "binance", "bybit"},
        {"0G/USDT", 54.04, 387, "binance", "bybit"},
        {"1000XEC/USDT", 28.90, 7490, "bybit", "binance"},
        {"1000FLOKI/USDT", 24.98, 21145, "binance", "bybit"},
        {"ACE/USDT", 17.30, 2047, "binance", "bybit"},
        {"1000LUNC/USDT", 15.94, 17015, "bybit", "binance"}
    };
    
    double capital = 100000;
    double position_pct = 0.5;
    
    // USE MAKER FEES ONLY (limit orders on both exchanges)
    double maker_fee = 0.0002;  // 0.02% Binance/Bybit maker
    double total_fee_per_leg = maker_fee;  // NO taker fees
    
    std::cout << "\n=== BACKTEST: LIVE OPPORTUNITIES ===\n\n";
    std::cout << "Capital: $" << capital << "\n";
    std::cout << "Position Size: " << (position_pct * 100) << "%\n";
    std::cout << "Fees: " << (total_fee_per_leg * 100) << "% per leg\n\n";
    
    double total_pnl = 0;
    int tradeable = 0;
    
    for (const auto& opp : opps) {
        double max_from_capital = capital * position_pct;
        double max_from_liquidity = opp.liquidity * 0.5;
        double position_size = std::min(max_from_capital, max_from_liquidity);
        
        // Entry: Long + Short (2 legs)
        double entry_fee = position_size * total_fee_per_leg * 2;
        // Exit: Close both positions (2 legs)
        double exit_fee = position_size * total_fee_per_leg * 2;
        double total_fees = entry_fee + exit_fee;
        
        // HOLD FOR 3 FUNDING CYCLES (24 hours)
        int num_cycles = 3;
        double gross_profit_8h = position_size * (opp.gross_apy / 100.0) / 365.0 / 3.0;
        double total_funding = gross_profit_8h * num_cycles;
        
        double net_pnl = total_funding - total_fees;
        double net_return_pct = (net_pnl / position_size) * 100.0;
        
        std::cout << opp.symbol << ":\n";
        std::cout << "  Liquidity: $" << std::fixed << std::setprecision(0) << opp.liquidity << "\n";
        std::cout << "  Position: $" << position_size << " (10% of liq)\n";
        std::cout << "  Gross APY: " << std::setprecision(2) << opp.gross_apy << "%\n";
        std::cout << "  Funding (3 cycles = 24h): $" << total_funding << "\n";
        std::cout << "  Fees (entry+exit): $" << total_fees << "\n";
        std::cout << "  Net P&L: $" << net_pnl << " (" << net_return_pct << "%)\n";
        
        if (net_pnl > 0) {
            total_pnl += net_pnl;
            tradeable++;
            std::cout << "  ✓ PROFITABLE\n";
        } else {
            std::cout << "  ✗ UNPROFITABLE\n";
        }
        std::cout << "\n";
    }
    
    std::cout << "=== SUMMARY ===\n";
    std::cout << "Opportunities: " << opps.size() << "\n";
    std::cout << "Profitable: " << tradeable << "\n";
    std::cout << "Total P&L (all trades): $" << std::fixed << std::setprecision(2) << total_pnl << "\n";
    std::cout << "Per Trade Avg: $" << (tradeable > 0 ? total_pnl / tradeable : 0) << "\n";
    std::cout << "Return per 8h cycle: " << (total_pnl / capital * 100) << "%\n";
    std::cout << "Extrapolated Daily: " << (total_pnl * 3 / capital * 100) << "%\n";
    std::cout << "Extrapolated Annual: " << (total_pnl * 3 * 365 / capital * 100) << "%\n\n";
    
    return 0;
}
