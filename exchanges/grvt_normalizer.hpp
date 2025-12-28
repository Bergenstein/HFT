// exchanges/grvt_normalizer.hpp - GRVT Data Normalizer
#pragma once

#include <nlohmann/json.hpp>
#include "../core/order_book.hpp"
#include <string>
#include <vector>

using json = nlohmann::json;

namespace exchanges {

class GRVTNormalizer {
public:
    static core::OrderBookUpdate normalize_l2_update(const json& msg) {
        core::OrderBookUpdate update;
        
        try {
            if (!msg.contains("channel") || !msg.contains("data")) {
                return update;
            }
            
            std::string channel = msg["channel"].get<std::string>();
            if (channel.find("orderbook") == std::string::npos) {
                return update;
            }
            
            const auto& data = msg["data"];
            
            // Extract symbol
            if (data.contains("instrument")) {
                update.symbol = data["instrument"].get<std::string>();
            }
            
            // Extract timestamp
            if (data.contains("timestamp")) {
                update.timestamp_us = data["timestamp"].get<uint64_t>();
            } else {
                update.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();
            }
            
            update.exchange = "GRVT";
            update.is_snapshot = data.contains("snapshot") && data["snapshot"].get<bool>();
            
            // Parse bids
            if (data.contains("bids") && data["bids"].is_array()) {
                for (const auto& bid : data["bids"]) {
                    if (bid.is_array() && bid.size() >= 2) {
                        core::PriceLevel level;
                        level.price = std::stod(bid[0].get<std::string>());
                        level.quantity = std::stod(bid[1].get<std::string>());
                        update.bids.push_back(level);
                    }
                }
            }
            
            // Parse asks
            if (data.contains("asks") && data["asks"].is_array()) {
                for (const auto& ask : data["asks"]) {
                    if (ask.is_array() && ask.size() >= 2) {
                        core::PriceLevel level;
                        level.price = std::stod(ask[0].get<std::string>());
                        level.quantity = std::stod(ask[1].get<std::string>());
                        update.asks.push_back(level);
                    }
                }
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[GRVT Normalizer] Error: " << e.what() << "\n";
        }
        
        return update;
    }
    
    static std::vector<core::Trade> normalize_trades(const json& msg) {
        std::vector<core::Trade> trades;
        
        try {
            if (!msg.contains("channel") || !msg.contains("data")) {
                return trades;
            }
            
            std::string channel = msg["channel"].get<std::string>();
            if (channel.find("trades") == std::string::npos) {
                return trades;
            }
            
            const auto& data = msg["data"];
            if (!data.is_array()) {
                return trades;
            }
            
            for (const auto& trade_data : data) {
                core::Trade trade;
                trade.exchange = "GRVT";
                
                if (trade_data.contains("instrument")) {
                    trade.symbol = trade_data["instrument"].get<std::string>();
                }
                
                if (trade_data.contains("price")) {
                    trade.price = std::stod(trade_data["price"].get<std::string>());
                }
                
                if (trade_data.contains("size")) {
                    trade.quantity = std::stod(trade_data["size"].get<std::string>());
                }
                
                if (trade_data.contains("side")) {
                    std::string side = trade_data["side"].get<std::string>();
                    trade.is_buyer_maker = (side == "sell");
                }
                
                if (trade_data.contains("timestamp")) {
                    trade.timestamp_us = trade_data["timestamp"].get<uint64_t>();
                } else {
                    trade.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();
                }
                
                trades.push_back(trade);
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[GRVT Normalizer] Trade error: " << e.what() << "\n";
        }
        
        return trades;
    }
};

} // namespace exchanges
