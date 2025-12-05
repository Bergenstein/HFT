//sim/units.hpp

#pragma once
#include <string>
#include <tuple>

namespace sim {

// Parses "BASE-QUOTE" like "BTC-USDT"
inline std::pair<std::string,std::string> split_product(const std::string& pid) {
    auto p = pid.find('-');
    if (p == std::string::npos) return {pid, ""};
    return { pid.substr(0,p), pid.substr(p+1) };
}

// Return a display suffix for the fee currency of a product (its quote currency)
inline std::string fee_currency_for(const std::string& product_id) {
    return split_product(product_id).second; // "USDT" for BTC-USDT, etc.
}

// Convert qty * mid_price into quote notional (for fixed-notional sizing)
inline double notional(double qty, double mid_price) { return qty * mid_price; }

} // namespace sim
