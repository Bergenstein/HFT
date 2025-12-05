#include "discovery.hpp"
#include "../net/http_client.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>

using json = nlohmann::json;
static const std::string EX_HOST = "api.exchange.coinbase.com";

std::vector<std::string> Discovery::tradable_products() {
  std::vector<std::string> out;
  try {
    json products = json::parse(HttpClient::get(EX_HOST, "/products"));
    out.reserve(products.size());
    for (auto& p : products) {
      if (!p.contains("id") || !p["id"].is_string()) continue;
      if (!p.contains("status") || p["status"] != "online") continue;
      if (p.contains("trading_disabled") && p["trading_disabled"] == true) continue;
      out.push_back(p["id"].get<std::string>());
    }
  } catch (const std::exception& e) {
    std::cerr << "Discovery tradable_products error: " << e.what() << "\n";
  }
  return out;
}

std::unordered_map<std::string,double> Discovery::volume_24h() {
  std::unordered_map<std::string,double> vol;
  try {
    json volsum = json::parse(HttpClient::get(EX_HOST, "/products/volume-summary"));
    if (volsum.is_array()) {
      for (auto& v : volsum) {
        if (!v.contains("product_id") || !v["product_id"].is_string()) continue;
        const std::string pid = v["product_id"].get<std::string>();
        double x = 0.0;
        if (v.contains("volume_24h") && v["volume_24h"].is_string()) {
          try { x = std::stod(v["volume_24h"].get<std::string>()); } catch(...) {}
        } else if (v.contains("volume") && v["volume"].is_string()) {
          try { x = std::stod(v["volume"].get<std::string>()); } catch(...) {}
        }
        vol[pid] = x;
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "Discovery volume_24h error: " << e.what() << "\n";
  }
  return vol;
}

std::vector<std::string> Discovery::top_by_volume(const std::vector<std::string>& universe,
                                                  const std::unordered_map<std::string,double>& vol,
                                                  std::size_t N) {
  struct Item { std::string id; double v; };
  std::vector<Item> items; items.reserve(universe.size());
  for (auto& pid : universe) {
    auto it = vol.find(pid);
    double x = (it==vol.end()? 0.0 : it->second);
    items.push_back({pid, x});
  }
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b){ return a.v > b.v; });
  if (items.size() > N) items.resize(N);
  std::vector<std::string> out; out.reserve(items.size());
  for (auto& it : items) out.push_back(it.id);
  return out;
}