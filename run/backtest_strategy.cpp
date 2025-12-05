// Unified backtest runner supporting multiple strategies via factory

#include <iostream>
#include <fstream>
#include <string>
#include <iomanip>

#include "../bt/backtester.hpp"
#include "../strats/strategy_factory.hpp"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] 
                  << " <NDJSON_FILE> <STRATEGY> <PRODUCT> [QTY] [PARAM1] [PARAM2] [CAPITAL]\n";
        std::cerr << "\nAvailable strategies:\n";
        StrategyFactory::print_available_strategies();
        return 1;
    }

    const std::string path = argv[1];
    const std::string strategy_name = argv[2];
    const std::string product = argv[3];
    const double qty = (argc > 4) ? std::stod(argv[4]) : 1.0;
    const double param1 = (argc > 5) ? std::stod(argv[5]) : 0.6;
    const double param2 = (argc > 6) ? std::stod(argv[6]) : 5.0;
    const double capital = (argc > 7) ? std::stod(argv[7]) : 50000.0;

    // Create strategy
    std::unique_ptr<Strategy> strat;
    try {
        strat = StrategyFactory::create(strategy_name, param1, param2);
    } catch (const std::exception& e) {
        std::cerr << "Error creating strategy: " << e.what() << "\n";
        StrategyFactory::print_available_strategies();
        return 2;
    }

    // Exec config
    ExecConfig exec;
    exec.default_qty = qty;
    exec.fee_bps = 5.0;
    exec.taker = true;
    exec.max_leverage = 1.0;

    // Create backtester and run
    Backtester bt(product, exec, *strat, capital);
    bt.run_file(path);
    
    auto m_ann = bt.finalize_metrics();
    auto m_period = bt.finalize_metrics_period();
    auto p = bt.pnl_summary();

    // Print results
    std::cout << "\n========== BACKTEST RESULTS ==========\n";
    std::cout << "Strategy         : " << strategy_name << "\n";
    std::cout << "Product          : " << product << "\n";
    std::cout << "Quantity         : " << qty << "\n";
    std::cout << "Param1           : " << param1 << "\n";
    std::cout << "Param2           : " << param2 << "\n";
    std::cout << "Capital          : $" << capital << "\n";
    std::cout << "--------------------------------------\n";
    std::cout << "Trades           : " << m_ann.trades << "\n";
    std::cout << "Total Return     : " << (m_ann.total_return * 100.0) << "%\n";
    std::cout << "Sharpe (period)  : " << m_period.sharpe << "\n";
    std::cout << "Sharpe (annual)  : " << m_ann.sharpe << "\n";
    std::cout << "Sortino (period) : " << m_period.sortino << "\n";
    std::cout << "Sortino (annual) : " << m_ann.sortino << "\n";
    std::cout << "Max Drawdown     : " << (m_ann.max_drawdown * 100.0) << "%\n";
    
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "--------------------------------------\n";
    std::cout << "Start Equity     : $" << p.start_equity << "\n";
    std::cout << "Final Equity     : $" << p.final_equity << "\n";
    std::cout << "Net PnL          : $" << p.net_pnl << " (" << (p.net_pnl_pct * 100.0) << "%)\n";
    std::cout << "Fees (total)     : $" << p.fees_total << "\n";
    std::cout << "======================================\n";

    return 0;
}
