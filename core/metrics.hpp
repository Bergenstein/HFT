#pragma once
#include <vector>
#include <cmath>
#include <limits>
#include <algorithm>

struct Metrics {
    double sharpe = std::numeric_limits<double>::quiet_NaN();
    double sortino = std::numeric_limits<double>::quiet_NaN();
    double max_drawdown = std::numeric_limits<double>::quiet_NaN();
    double total_return = 0.0;
    size_t trades = 0;
};

inline double mean(const std::vector<double>& x) {
    if (x.empty()) return 0.0;
    double s=0; for (double v: x) s+=v; return s/x.size();
}

inline double stddev(const std::vector<double>& x) {
    if (x.size() < 2) return 0.0;
    double m = mean(x), s2=0; for (double v: x){ double d=v-m; s2+=d*d; }
    return std::sqrt(s2/(x.size()-1));
}

inline double downside_stddev(const std::vector<double>& x) {
    std::vector<double> down;
    down.reserve(x.size());
    for (double v: x) if (v < 0) down.push_back(v);
    if (down.size() < 2) return 0.0;
    return stddev(down);
}

inline double max_drawdown(const std::vector<double>& equity) {
    if (equity.empty()) return 0.0;
    double peak = equity.front(), mdd = 0.0;
    for (double v: equity) {
        peak = std::max(peak, v);
        mdd = std::max(mdd, (peak - v) / (peak == 0.0 ? 1.0 : peak));
    }
    return mdd;
}

inline Metrics compute_metrics(const std::vector<double>& equity_curve,
                               const std::vector<double>& step_returns,
                               double annualization_factor,
                               size_t trades)
{
    Metrics m;
    m.total_return = equity_curve.empty() ? 0.0
                                          : (equity_curve.back() - equity_curve.front()) / (equity_curve.front()==0?1:equity_curve.front());
    const double sd = stddev(step_returns);
    const double ds = downside_stddev(step_returns);
    const double mu = mean(step_returns);
    m.sharpe  = (sd>0)? (mu/sd)*std::sqrt(annualization_factor) : 0.0;
    m.sortino = (ds>0)? (mu/ds)*std::sqrt(annualization_factor) : 0.0;
    m.max_drawdown = max_drawdown(equity_curve);
    m.trades = trades;
    return m;
}