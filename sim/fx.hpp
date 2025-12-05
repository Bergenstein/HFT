// sim/fx.hpp - Minimal FX utilities for the simulator
//
// PURPOSE:
// - Provides a tiny in-memory FX table for converting base/quote amounts and
//   computing notional values for strategies and fee accounting.
// - Useful for multi-currency simulations where strategies measure PnL or size
//   in quote currency (e.g., USD) or where product pair conversions are needed.
//
// DESIGN:
// - `mid` map stores mid prices keyed by "BASE-QUOTE" (e.g., "ETH-USD").
// - `alias` map lets you treat USDT/USDC/DAI as USD equivalents for simple
//   simulations (1:1 mapping) — change as needed to model real exchange spreads.
// - Conversions are simple: direct pair lookup, inverse lookup, and a 2-hop
//   USD bridge, if required. If no data, we fall back to passthrough (no conversion).
//
// THREAD SAFETY:
// - This structure is not internally synchronized. It is intended to be updated
//   during initialization (single thread). If you update FX rates dynamically
//   at runtime (e.g., mid-price updates), protect it with an external mutex.
//
// USAGE EXAMPLES:
//   FX fx;
//   fx.set("ETH","USD", 3500.0);
//   double usd_value = fx.to_usd("ETH", 2.0); // 7000 USD
//
// NOTES:
// - This is a test helper; for production-level FX you may want to integrate
//   a dedicated FX service with timestamps, tick updates, and liquidity modeling.

#pragma once
#include <unordered_map>
#include <string>
#include <algorithm>
#include <cctype>
#include <utility>
#include <stdexcept>

namespace sim {

// Minimal FX helper: store mids for "BASE-QUOTE" pairs and convert amounts.
// Defaults: treats common USD stables as aliases of USD (1:1).
struct FX {
  // Mid prices keyed by "BASE-QUOTE" (e.g., "ETH-USD" -> 3520.5).
  std::unordered_map<std::string, double> mid;

  // Currency aliases, e.g., USDT->USD (1:1)
  std::unordered_map<std::string, std::string> alias;

  FX() {
    // Stablecoin aliases to USD (adjust if you need basis spread handling)
    alias["USDT"] = "USD";
    alias["USDC"] = "USD";
    alias["DAI"]  = "USD";
    alias["FDUSD"]= "USD";
    alias["BUSD"] = "USD";
    // Identity mids
    set("USD","USD",1.0);
    set("EUR","EUR",1.0);
    set("GBP","GBP",1.0);
  }

  // Normalize codes & pairs to canonical uppercase "AAA-BBB"
  static std::string norm_ccy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::toupper(c); });
    return s;
  }
  static std::string make_pair(std::string base, std::string quote) {
    base  = norm_ccy(base);
    quote = norm_ccy(quote);
    return base + "-" + quote;
  }

  // Optional: add/override an alias (e.g., set_alias("EURT","EUR"))
  void set_alias(std::string from, std::string to) {
    alias[norm_ccy(from)] = norm_ccy(to);
  }

  // Insert/update mid for BASE/QUOTE and its inverse.
  void set(std::string base, std::string quote, double m) {
    base  = norm_ccy(base);
    quote = norm_ccy(quote);
    if (m <= 0.0) return;
    mid[make_pair(base, quote)]  = m;
    mid[make_pair(quote, base)]  = 1.0 / m;
  }

  // Get mid if present (throws if missing)
  double get(std::string base, std::string quote) const {
    base  = norm_ccy(base);
    quote = norm_ccy(quote);
    auto it = mid.find(make_pair(base, quote));
    if (it == mid.end()) throw std::runtime_error("FX mid missing for " + make_pair(base, quote));
    return it->second;
  }

  // Safe lookup: returns {found?, mid}
  std::pair<bool,double> try_get(std::string base, std::string quote) const {
    base  = norm_ccy(base);
    quote = norm_ccy(quote);
    auto it = mid.find(make_pair(base, quote));
    if (it == mid.end()) return {false, 0.0};
    return {true, it->second};
  }

  // Convert amount from 'from_ccy' to 'to_ccy'.
  // Strategy:
  //  1) apply aliases
  //  2) direct pair or inverse if known
  //  3) if converting to USD and from_ccy is aliased to USD, treat 1:1
  //  4) last resort: if neither direct nor inverse exists and to==from, passthrough
  double convert(std::string from_ccy, std::string to_ccy, double amount) const {
    from_ccy = norm_ccy(from_ccy);
    to_ccy   = norm_ccy(to_ccy);

    // alias remap (e.g., USDT -> USD)
    auto itf = alias.find(from_ccy);
    if (itf != alias.end()) from_ccy = itf->second;
    auto itt = alias.find(to_ccy);
    if (itt != alias.end()) to_ccy = itt->second;

    if (from_ccy == to_ccy) return amount;

    // direct
    if (auto [ok, m] = try_get(from_ccy, to_ccy); ok) return amount * m;
    // inverse
    if (auto [ok, m] = try_get(to_ccy, from_ccy); ok) return amount / m;

    // two-hop via USD if both legs exist (rarely needed if you only call to_usd)
    if (from_ccy != "USD" && to_ccy != "USD") {
      if (auto [ok1, m1] = try_get(from_ccy, "USD"); ok1) {
        if (auto [ok2, m2] = try_get("USD", to_ccy); ok2) {
          return amount * m1 * m2;
        }
      }
      if (auto [ok1, m1] = try_get("USD", from_ccy); ok1) {
        if (auto [ok2, m2] = try_get(to_ccy, "USD"); ok2) {
          return amount / m1 / m2;
        }
      }
    }

    // Fallback (no data): passthrough (safer than guessing)
    return amount;
  }

  // Convenience
  double to_usd(std::string ccy, double amount) const {
    return convert(ccy, "USD", amount);
  }
};

} // namespace sim
