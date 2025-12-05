#pragma once
#include <map>
#include <string>
#include <utility>
#include <limits>
#include <optional>
#include <nlohmann/json.hpp>

namespace core {
struct OrderBook {
    using Price = double;
    using Qty   = double;
    using Level = std::pair<Price, Qty>;
    using PriceMap = std::map<Price, Qty, std::less<Price>>;
    
    PriceMap bids;
    PriceMap asks;

    inline void set_level(bool is_bid, Price px, Qty qty) {
        PriceMap& side = is_bid ? bids : asks;
        if (qty <= 0.0) side.erase(px);
        else            side[px] = qty;
    }

    inline void clear() { bids.clear(); asks.clear(); }

    inline void apply_updates(const nlohmann::json& updates) {
        for (const auto& u : updates) {
            const auto side = u.at("side").get<std::string>();
            const bool is_bid = (side == "bid");
            const Price px = std::stod(u.at("price_level").get<std::string>());
            const Qty   q  = std::stod(u.at("new_quantity").get<std::string>());
            set_level(is_bid, px, q);
        }
    }

    inline bool top_valid() const { return !bids.empty() && !asks.empty(); }

    inline std::optional<Level> best_bid() const {
        if (bids.empty()) return std::nullopt;
        auto it = std::prev(bids.end());
        return Level{it->first, it->second};
    }

    inline std::optional<Level> best_ask() const {
        if (asks.empty()) return std::nullopt;
        auto it = asks.begin();
        return Level{it->first, it->second};
    }

    inline double mid() const {
        if (!top_valid()) return std::numeric_limits<double>::quiet_NaN();
        auto bb = *best_bid(); auto aa = *best_ask();
        return (bb.first + aa.first) / 2.0;
    }

    inline double top_imbalance() const {
        if (!top_valid()) return std::numeric_limits<double>::quiet_NaN();
        auto bb = *best_bid(); auto aa = *best_ask();
        const double num = bb.second - aa.second;
        const double den = bb.second + aa.second;
        if (den == 0.0) return 0.0;
        return num / den;
    }

    inline double microprice() const {
        if (!top_valid()) return std::numeric_limits<double>::quiet_NaN();
        auto [bp, bq] = *best_bid();
        auto [ap, aq] = *best_ask();
        const double den = bq + aq;
        if (den == 0.0) return std::numeric_limits<double>::quiet_NaN();
        return (ap * bq + bp * aq) / den;
    }
};
}