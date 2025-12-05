#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>

using json = nlohmann::json;

struct L2Update {
  std::string product;
  bool is_bid;
  double price;
  double qty;
  uint64_t seq;
  std::string event_time;
};

struct L2Snapshot {
  std::string product;
  std::vector<std::pair<double,double>> bids;
  std::vector<std::pair<double,double>> asks;
  uint64_t seq;
};

struct Normalizer {
  static void parse(const std::string& raw,
                    std::vector<L2Snapshot>& out_snaps,
                    std::vector<L2Update>& out_updates) {
    out_snaps.clear(); out_updates.clear();
    json m = json::parse(raw, nullptr, false);
    if (m.is_discarded()) return;
    if (!m.contains("channel") || m["channel"] != "l2_data") return;
    uint64_t seq = m.value("sequence_num", 0ULL);
    if (!m.contains("events") || !m["events"].is_array()) return;
    for (auto& ev : m["events"]) {
      if (!ev.contains("type") || !ev.contains("product_id")) continue;
      const std::string type = ev["type"].get<std::string>();
      const std::string pid  = ev["product_id"].get<std::string>();
      if (type == "snapshot") {
        L2Snapshot s; s.product = pid; s.seq = seq;
        auto grab_side = [](const json& side, std::vector<std::pair<double,double>>& out){
          if (!side.is_array()) return;
          for (auto& x : side) {
            if (!x.contains("price_level")) continue;
            const std::string p = x["price_level"].get<std::string>();
            std::string q = x.contains("new_quantity") ? x["new_quantity"].get<std::string>()
                                                       : (x.contains("quantity") ? x["quantity"].get<std::string>() : "0");
            try { out.emplace_back(std::stod(p), std::stod(q)); } catch(...) {}
          }
        };
        if (ev.contains("bids")) grab_side(ev["bids"], s.bids);
        if (ev.contains("asks")) grab_side(ev["asks"], s.asks);
        out_snaps.push_back(std::move(s));
      } else if (type == "update") {
        if (!ev.contains("updates") || !ev["updates"].is_array()) continue;
        for (auto& u : ev["updates"]) {
          if (!u.contains("side") || !u.contains("price_level")) continue;
          const std::string side = u["side"].get<std::string>();
          const std::string pstr = u["price_level"].get<std::string>();
          const std::string qstr = u.contains("new_quantity") ? u["new_quantity"].get<std::string>() : "0";
          L2Update up;
          up.product = pid;
          up.is_bid  = (side == "bid");
          up.seq     = seq;
          up.event_time = u.value("event_time", "");
          try { up.price = std::stod(pstr); } catch(...) { continue; }
          try { up.qty   = std::stod(qstr); } catch(...) { up.qty = 0; }
          out_updates.push_back(std::move(up));
        }
      }
    }
  }
};