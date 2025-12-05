#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <unordered_map>
#include <cctype>
#include <iomanip>
#include <cstdlib>

#include "../bt/backtester.hpp"
#include "../strats/imbalance_taker.hpp"
#include "../strats/multi_imbalance_taker.hpp"
#include "../sim/fx.hpp"

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto l = std::find_if(item.begin(), item.end(), [](int c){return !std::isspace(c);});
        auto r = std::find_if(item.rbegin(), item.rend(), [](int c){return !std::isspace(c);}).base();
        if (l < r) out.emplace_back(l, r);
    }
    return out;
}

static bool is_qty_spec(const std::string& s) {
    return s.find('=') != std::string::npos || s.find(':') != std::string::npos;
}

static std::unordered_map<std::string,double> parse_qty_spec(const std::string& s) {
    std::unordered_map<std::string,double> m;
    std::stringstream ss(s);
    std::string item;
    auto trim = [](std::string& t){
        auto l = std::find_if(t.begin(), t.end(), [](int c){return !std::isspace(c);});
        auto r = std::find_if(t.rbegin(), t.rend(), [](int c){return !std::isspace(c);}).base();
        t = (l<r) ? std::string(l,r) : std::string();
    };
    while (std::getline(ss, item, ',')) {
        trim(item);
        if (item.empty()) continue;
        auto sep = item.find('=');
        if (sep == std::string::npos) sep = item.find(':');
        if (sep == std::string::npos) continue;
        std::string k = item.substr(0, sep), v = item.substr(sep + 1);
        trim(k); trim(v);
        try { m.emplace(k, std::stod(v)); } catch (...) {}
    }
    return m;
}

static inline std::string quote_ccy(const std::string& pid) {
    auto s = pid.find('-');
    return (s == std::string::npos) ? "USD" : pid.substr(s+1);
}

static inline void fx_load_spec(sim::FX& fx, const std::string& spec){
    std::stringstream ss(spec);
    std::string item;
    auto trim = [](std::string& t){
        auto l = std::find_if(t.begin(), t.end(), [](int c){return !std::isspace(c);});
        auto r = std::find_if(t.rbegin(), t.rend(), [](int c){return !std::isspace(c);}).base();
        t = (l<r) ? std::string(l,r) : std::string();
    };
    while (std::getline(ss, item, ',')) {
        trim(item);
        if (item.empty()) continue;
        auto eq = item.find('=');
        if (eq == std::string::npos) continue;
        std::string pair = item.substr(0, eq), val = item.substr(eq+1);
        trim(pair); trim(val);

        std::string a, b;
        std::string letters;
        for (char c: pair) if (std::isalpha((unsigned char)c)) letters.push_back(std::toupper((unsigned char)c));
        if (letters.size() == 6) { a = letters.substr(0,3); b = letters.substr(3); }
        else {
            auto dash = pair.find_first_of("-/");
            if (dash==std::string::npos) continue;
            a = pair.substr(0,dash); b = pair.substr(dash+1);
        }
        try { fx.set(a, b, std::stod(val)); } catch (...) {}
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr <<
            "Usage: " << argv[0] <<
            " <NDJSON_FILE> <PRODUCT_ID[,PRODUCT_ID,...]> [QTY|QTY_SPEC] [THRESH] [HOLD_TICKS] [CAPITAL] [LEVERAGE]\n";
        return 1;
    }
    const std::string path      = argv[1];
    const std::string prods_csv = argv[2];

    double default_qty = 0.01;
    if (argc > 3 && !is_qty_spec(argv[3])) default_qty = std::stod(argv[3]);

    const double thresh   = (argc > 4) ? std::stod(argv[4]) : 0.3;
    const int    hold     = (argc > 5) ? std::stoi(argv[5]) : 5;
    const double capital  = (argc > 6) ? std::stod(argv[6]) : 50000.0;
    const double leverage = (argc > 7) ? std::stod(argv[7]) : 1.0;

    const auto products = split_csv(prods_csv);
    if (products.empty()) { std::cerr << "No valid products parsed.\n"; return 2; }

    ExecConfig exec;
    exec.default_qty  = default_qty;
    exec.fee_bps      = 5.0;
    exec.taker        = true;
    exec.max_leverage = leverage;
    if (argc > 3 && is_qty_spec(argv[3])) {
        exec.per_qty = parse_qty_spec(argv[3]);
    }

    sim::FX fx;
    if (const char* env = std::getenv("FX_SPEC")) fx_load_spec(fx, env);

    try {
        Metrics m_ann, m_period;
        Backtester::PnLSummary p;

        if (products.size() == 1) {
            ImbalanceTaker strat(thresh, hold);
            Backtester bt(products[0], exec, strat, capital);
            bt.run_file(path);
            m_ann    = bt.finalize_metrics();
            m_period = bt.finalize_metrics_period();
            p        = bt.pnl_summary();
        } else {
            MultiImbalanceTaker strat(thresh, hold);
            Backtester bt(products, exec, strat, capital);
            bt.run_file(path);
            m_ann    = bt.finalize_metrics();
            m_period = bt.finalize_metrics_period();
            p        = bt.pnl_summary();
        }

        std::cout << "=== Results (" << prods_csv << ") ===\n";
        std::cout << "Trades               : " << m_ann.trades << "\n";
        std::cout << "Total Return         : " << m_ann.total_return * 100.0 << " %\n";
        std::cout << "Sharpe (period)      : " << m_period.sharpe  << "\n";
        std::cout << "Sortino (period)     : " << m_period.sortino << "\n";
        std::cout << "Sharpe (annualized)  : " << m_ann.sharpe     << "\n";
        std::cout << "Sortino (annualized) : " << m_ann.sortino    << "\n";
        std::cout << "Max Drawdown         : " << m_ann.max_drawdown * 100.0 << " %\n";

        std::cout << std::fixed << std::setprecision(6);
        std::cout << "---- PnL ----\n";
        std::cout << "Start Equity     : " << p.start_equity << "\n";
        std::cout << "Final Equity     : " << p.final_equity << "\n";
        std::cout << "Net PnL          : " << p.net_pnl
                  << " (" << p.net_pnl_pct * 100.0 << " %)\n";
        std::cout << "Fees (total)     : " << p.fees_total << "\n";

        double fees_usd_total = 0.0, net_usd_total = 0.0;
        for (const auto& L : p.legs) {
            const std::string q = quote_ccy(L.pid);
            const double cash_usd = fx.to_usd(q, L.cash);
            const double mtm_usd  = fx.to_usd(q, L.mtm);
            const double fees_usd = fx.to_usd(q, L.fees);
            const double net_usd  = fx.to_usd(q, L.net);
            fees_usd_total += fees_usd;
            net_usd_total  += net_usd;

            std::cout << "  [" << L.pid << "] trades=" << L.trades
                      << " qty="  << L.qty
                      << " cash=" << L.cash
                      << " mtm="  << L.mtm
                      << " fees=" << L.fees
                      << " net="  << L.net
                      << "  | USD cash=" << cash_usd
                      << " mtm="  << mtm_usd
                      << " fees=" << fees_usd
                      << " net="  << net_usd
                      << "\n";
        }
        std::cout << "Fees (total USD) : " << fees_usd_total << "\n";
        std::cout << "Net PnL (USD sum): " << net_usd_total  << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Backtest error: " << e.what() << "\n";
        return 3;
    }
    return 0;
}
