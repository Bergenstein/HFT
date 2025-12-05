#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <numeric>
#include <limits>
#include <cctype>
#include <cmath>
#include <nlohmann/json.hpp>
#include "../core/order_book.hpp"
#include "../core/metrics.hpp"

using json = nlohmann::json;
using OrderBook = core::OrderBook;

struct TickContext {
    double t = 0.0;
    double best_bid = 0.0;
    double best_ask = 0.0;
    double mid      = 0.0;
    double imb      = 0.0;
    double micro    = 0.0;
    uint64_t seq    = 0;
    std::string product;
};

struct ExecConfig {
    double default_qty = 0.01;
    std::unordered_map<std::string,double> per_qty;
    double fee_bps = 5.0;
    bool   taker = true;
    double max_leverage = 1.0;
    double qty_for(const std::string& pid) const {
        auto it = per_qty.find(pid);
        return (it != per_qty.end()) ? it->second : default_qty;
    }
};

struct Position {
    double qty  = 0.0;
    double cash = 0.0;
    double total_fees = 0.0;
    size_t trades = 0;
};

struct Strategy {
    virtual ~Strategy() = default;
    virtual int on_tick(const TickContext& tc, const OrderBook& ob) = 0;
};

class Backtester {
public:
    Backtester(const std::string& product, ExecConfig exec, Strategy& strat, double start_equity = 10000.0)
        : products_{product}, product_set_{products_.begin(), products_.end()},
          exec_(exec), strat_(strat), start_equity_(start_equity) {}

    Backtester(const std::vector<std::string>& products, ExecConfig exec, Strategy& strat, double start_equity = 10000.0)
        : products_(products), product_set_(products.begin(), products.end()),
          exec_(exec), strat_(strat), start_equity_(start_equity) {}

    void run_file(const std::string& path) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("Cannot open " + path);
        reset_();
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            auto brace = line.find('{');
            if (brace == std::string::npos) continue;
            json outer = json::parse(line.substr(brace), nullptr, false);
            if (outer.is_discarded()) continue;
            curr_ts_ns_ = get_ts_ns_(outer);
            json msg = outer;
            if (outer.contains("raw") && outer["raw"].is_string()) {
                json inner = json::parse(outer["raw"].get<std::string>(), nullptr, false);
                if (!inner.is_discarded()) msg.swap(inner);
            }
            if (msg.contains("events") && msg["events"].is_array()) {
                if (msg.contains("channel") && msg["channel"].is_string()) {
                    const std::string ch = msg["channel"].get<std::string>();
                    if (!(ch == "l2_data" || ch == "level2")) continue;
                }
                for (const auto& ev : msg["events"]) {
                    if (!ev.is_object()) continue;
                    const std::string* pid = get_pid_(ev);
                    if (!pid || !is_tracked_(*pid)) continue;
                    if (is_snapshot_(ev)) {
                        apply_snapshot_ev_(ob_[*pid], ev);
                        on_book_change_(*pid);
                    }
                    if (ev.contains("updates") && ev["updates"].is_array()) {
                        ob_[*pid].apply_updates(ev["updates"]);
                        on_book_change_(*pid);
                    }
                }
                continue;
            }
            if (msg.contains("product_id") && msg["product_id"].is_string() &&
                msg.contains("updates") && msg["updates"].is_array())
            {
                const std::string* pid = get_pid_(msg);
                if (pid && is_tracked_(*pid)) {
                    ob_[*pid].apply_updates(msg["updates"]);
                    on_book_change_(*pid);
                }
                continue;
            }
            if (msg.contains("type") && msg["type"].is_string() && msg["type"] == "snapshot") {
                const std::string* pid = get_pid_(msg);
                if (!pid || !is_tracked_(*pid)) continue;
                apply_snapshot_msg_(ob_[*pid], msg);
                on_book_change_(*pid);
                continue;
            }
            if (msg.contains("type") && msg["type"].is_string() && msg["type"] == "l2update") {
                const std::string* pid = get_pid_(msg);
                if (!pid || !is_tracked_(*pid)) continue;
                if (!msg.contains("changes") || !msg["changes"].is_array()) continue;
                json updates = json::array();
                for (const auto& c : msg["changes"]) {
                    if (!c.is_array() || c.size() < 3 || !c[0].is_string()) continue;
                    const std::string side = c[0].get<std::string>();
                    const double px  = to_double_(c[1]);
                    const double qty = to_double_(c[2]);
                    if (!std::isfinite(px) || qty < 0.0) continue;
                    updates.push_back({{"side", side}, {"price_level", px}, {"new_quantity", qty}});
                }
                if (!updates.empty()) {
                    ob_[*pid].apply_updates(updates);
                    on_book_change_(*pid);
                }
                continue;
            }
        }
        std::cerr << "Backtest complete. Ticks: "
                  << (equity_.empty() ? 0 : (equity_.size() - 1));
        for (const auto& [pid, p] : pos_) {
            std::cerr << ", Trades(" << pid << "): " << p.trades << ", Fees: " << p.total_fees;
        }
        std::cerr << "\n";
        if (!dts_.empty()) {
            const double mean_dt = std::accumulate(dts_.begin(), dts_.end(), 0.0) / dts_.size();
            std::cerr << "Mean dt (sec): " << mean_dt << "\n";
        }
    }

    Metrics finalize_metrics() const {
        double ann = 365.0 * 24.0 * 60.0;
        if (!dts_.empty()) {
            double sum = std::accumulate(dts_.begin(), dts_.end(), 0.0);
            const double mean_dt_sec = sum / dts_.size();
            if (mean_dt_sec > 0.0) ann = 365.0 * 86400.0 / mean_dt_sec;
        }
        return compute_metrics(equity_, step_rets_, ann, total_trades_());
    }

    Metrics finalize_metrics_period() const {
        return compute_metrics(equity_, step_rets_, 1.0, total_trades_());
    }

    double estimated_ann_factor() const {
        double ann = 365.0 * 24.0 * 60.0;
        if (!dts_.empty()) {
            double sum = std::accumulate(dts_.begin(), dts_.end(), 0.0);
            const double mean_dt_sec = sum / dts_.size();
            if (mean_dt_sec > 0.0) ann = 365.0 * 86400.0 / mean_dt_sec;
        }
        return ann;
    }

public:
    struct PnLSummary {
        double start_equity = 0.0;
        double final_equity = 0.0;
        double net_pnl      = 0.0;
        double net_pnl_pct  = 0.0;
        double fees_total   = 0.0;
        struct Leg {
            std::string pid;
            double qty   = 0.0;
            double mtm   = 0.0;
            double cash  = 0.0;
            double fees  = 0.0;
            size_t trades= 0;
            double net   = 0.0;
        };
        std::vector<Leg> legs;
    };

    PnLSummary pnl_summary() const {
        PnLSummary s;
        s.start_equity = start_equity_;
        double mtm_sum = 0.0, cash_sum = 0.0, fees_sum = 0.0;
        for (const auto& [pid, p] : pos_) {
            PnLSummary::Leg L;
            L.pid    = pid;
            L.qty    = p.qty;
            L.cash   = p.cash;
            L.fees   = p.total_fees;
            L.trades = p.trades;
            auto it = ob_.find(pid);
            double mid = 0.0;
            if (it != ob_.end() && it->second.top_valid()) mid = it->second.mid();
            L.mtm = L.qty * mid;
            L.net = L.cash + L.mtm;
            mtm_sum  += L.mtm;
            cash_sum += L.cash;
            fees_sum += L.fees;
            s.legs.push_back(L);
        }
        s.final_equity = start_equity_ + cash_sum + mtm_sum;
        s.net_pnl      = s.final_equity - s.start_equity;
        s.net_pnl_pct  = (s.start_equity != 0.0) ? s.net_pnl / s.start_equity : 0.0;
        s.fees_total   = fees_sum;
        return s;
    }

private:
    static const std::string* get_pid_(const json& j) {
        if (j.contains("product_id") && j["product_id"].is_string())
            return &j["product_id"].get_ref<const std::string&>();
        return nullptr;
    }
    static bool is_snapshot_(const json& ev) {
        return ev.contains("type") && ev["type"].is_string() && ev["type"] == "snapshot";
    }
    static double to_double_(const json& x) {
        if (x.is_number()) return x.get<double>();
        if (x.is_string()) {
            try { return std::stod(x.get<std::string>()); } catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
    static long long get_ts_ns_(const json& outer) {
        try {
            if (outer.contains("ts_recv_ns")) {
                const auto& v = outer["ts_recv_ns"];
                if (v.is_number_integer()) return v.get<long long>();
                if (v.is_string()) return std::stoll(v.get<std::string>());
                if (v.is_number()) return static_cast<long long>(v.get<double>());
            }
        } catch (...) {}
        return 0;
    }
    static double parse_level_price_(const json& lvl) {
        if (lvl.is_array() && lvl.size() >= 1) return to_double_(lvl[0]);
        if (lvl.is_object()) {
            if (lvl.contains("price"))        return to_double_(lvl["price"]);
            if (lvl.contains("price_level"))  return to_double_(lvl["price_level"]);
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
    static double parse_level_qty_(const json& lvl) {
        if (lvl.is_array() && lvl.size() >= 2) return to_double_(lvl[1]);
        if (lvl.is_object()) {
            if (lvl.contains("size"))         return to_double_(lvl["size"]);
            if (lvl.contains("quantity"))     return to_double_(lvl["quantity"]);
            if (lvl.contains("new_quantity")) return to_double_(lvl["new_quantity"]);
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
    static void apply_snapshot_ev_(OrderBook& ob, const json& ev) {
        if (!(ev.contains("bids") && ev["bids"].is_array() &&
              ev.contains("asks") && ev["asks"].is_array())) return;
        for (const auto& lvl : ev["bids"]) {
            const double px = parse_level_price_(lvl);
            const double q  = parse_level_qty_(lvl);
            if (std::isfinite(px) && q >= 0.0) ob.set_level(true,  px, q);
        }
        for (const auto& lvl : ev["asks"]) {
            const double px = parse_level_price_(lvl);
            const double q  = parse_level_qty_(lvl);
            if (std::isfinite(px) && q >= 0.0) ob.set_level(false, px, q);
        }
    }
    static void apply_snapshot_msg_(OrderBook& ob, const json& msg) {
        if (!(msg.contains("bids") && msg["bids"].is_array() &&
              msg.contains("asks") && msg["asks"].is_array())) return;
        for (const auto& a : msg["bids"]) {
            if (!a.is_array() || a.size() < 2) continue;
            const double px = to_double_(a[0]);
            const double q  = to_double_(a[1]);
            if (std::isfinite(px) && q >= 0.0) ob.set_level(true,  px, q);
        }
        for (const auto& a : msg["asks"]) {
            if (!a.is_array() || a.size() < 2) continue;
            const double px = to_double_(a[0]);
            const double q  = to_double_(a[1]);
            if (std::isfinite(px) && q >= 0.0) ob.set_level(false, px, q);
        }
    }
    double equity_snapshot_() const {
        double mtm = 0.0;
        for (const auto& [pid, p] : pos_) {
            auto it = ob_.find(pid);
            if (it != ob_.end() && it->second.top_valid()) {
                mtm += p.qty * it->second.mid();
            }
        }
        const double cash_sum = std::accumulate(
            pos_.begin(), pos_.end(), 0.0,
            [](double s, const auto& kv){ return s + kv.second.cash; });
        return start_equity_ + cash_sum + mtm;
    }
    void on_book_change_(const std::string& pid) {
        auto& ob = ob_[pid];
        if (!ob.top_valid()) return;
        auto bb_opt = ob.best_bid();
        auto ba_opt = ob.best_ask();
        if (!bb_opt || !ba_opt) return;
        const double best_bid_px = bb_opt->first;
        const double best_ask_px = ba_opt->first;
        TickContext tc;
        tc.product = pid;
        tc.best_bid = best_bid_px;
        tc.best_ask = best_ask_px;
        tc.mid      = ob.mid();
        tc.imb      = ob.top_imbalance();
        tc.micro    = ob.microprice();
        const int sig = strat_.on_tick(tc, ob);
        if (sig != 0 && std::isfinite(tc.mid)) {
            const double fee_frac = exec_.fee_bps * 1e-4;
            auto& pos = pos_[pid];
            const double qty = exec_.qty_for(pid);
            if (sig > 0) {
                const double px = best_ask_px;
                const double notional = qty * px;
                if (exec_.max_leverage <= 0.0 || notional <= equity_snapshot_() * exec_.max_leverage) {
                    const double fee = qty * px * fee_frac;
                    pos.qty  += qty;
                    pos.cash -= notional + fee;
                    pos.trades++;
                    pos.total_fees += fee;
                }
            } else {
                const double px = best_bid_px;
                const double fee = qty * px * fee_frac;
                pos.qty  -= qty;
                pos.cash += qty * px - fee;
                pos.trades++;
                pos.total_fees += fee;
            }
        }
        append_equity_();
    }
    void append_equity_() {
        double mtm = 0.0;
        for (const auto& [pid, ob] : ob_) {
            if (ob.top_valid()) mtm += pos_[pid].qty * ob.mid();
        }
        const double cash_sum = std::accumulate(
            pos_.begin(), pos_.end(), 0.0,
            [](double s, const auto& kv) { return s + kv.second.cash; });
        const double eq_total   = start_equity_ + cash_sum + mtm;
        const double prev_total = equity_.empty() ? start_equity_ : equity_.back();
        equity_.push_back(eq_total);
        const double ret = (prev_total != 0.0) ? (eq_total - prev_total) / prev_total : 0.0;
        step_rets_.push_back(ret);
        if (curr_ts_ns_ > 0) {
            if (prev_ts_ns_ > 0 && curr_ts_ns_ >= prev_ts_ns_) {
                const double dt = double(curr_ts_ns_ - prev_ts_ns_) * 1e-9;
                if (dt > 0.0) dts_.push_back(dt);
            }
            prev_ts_ns_ = curr_ts_ns_;
        }
    }
    void reset_() {
        ob_.clear();
        pos_.clear();
        equity_.clear();
        step_rets_.clear();
        dts_.clear();
        equity_.push_back(start_equity_);
        prev_ts_ns_ = 0;
        curr_ts_ns_ = 0;
    }
    bool is_tracked_(const std::string& pid) const {
        return product_set_.empty() ? true : (product_set_.count(pid) > 0);
    }
    size_t total_trades_() const {
        return std::accumulate(pos_.begin(), pos_.end(), size_t{0},
            [](size_t s, const auto& kv) { return s + kv.second.trades; });
    }

private:
    std::vector<std::string>                    products_;
    std::unordered_set<std::string>             product_set_;
    ExecConfig                                  exec_;
    Strategy&                                   strat_;
    std::unordered_map<std::string, OrderBook>  ob_;
    std::unordered_map<std::string, Position>   pos_;
    std::vector<double>                         equity_;
    std::vector<double>                         step_rets_;
    std::vector<double>                         dts_;
    double                                      start_equity_{10000.0};
    long long                                   prev_ts_ns_{0};
    long long                                   curr_ts_ns_{0};
};