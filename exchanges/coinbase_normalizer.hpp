// exchanges/coinbase_normalizer.hpp - Coinbase Data Normalizer
#pragma once

#include <nlohmann/json.hpp>
#include "../core/order_book.hpp"
#include <string>
#include <vector>

using json = nlohmann::json;

namespace exchanges {

class CoinbaseNormalizer {
public:
    static core::OrderBookUpdate normalize_l2_update(const json& msg) {
        core::OrderBookUpdate update;
        
        try {
            if (!msg.contains("type")) {
                return update;
            }
            
            std::string msg_type = msg["type"].get<std::string>();
            
            // Handle snapshot
            if (msg_type == "snapshot") {
                update.is_snapshot = true;
                
                if (msg.contains("product_id")) {
                    update.symbol = msg["product_id"].get<std::string>();
                }
                
                update.exchange = "COINBASE";
                update.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();
                
                // Parse bids
                if (msg.contains("bids") && msg["bids"].is_array()) {
                    for (const auto& bid : msg["bids"]) {
                        if (bid.is_array() && bid.size() >= 2) {
                            core::PriceLevel level;
                            level.price = std::stod(bid[0].get<std::string>());
                            level.quantity = std::stod(bid[1].get<std::string>());
                            update.bids.push_back(level);
                        }
                    }
                }
                
                // Parse asks
                if (msg.contains("asks") && msg["asks"].is_array()) {
                    for (const auto& ask : msg["asks"]) {
                        if (ask.is_array() && ask.size() >= 2) {
                            core::PriceLevel level;
                            level.price = std::stod(ask[0].get<std::string>());
                            level.quantity = std::stod(ask[1].get<std::string>());
                            update.asks.push_back(level);
                        }
                    }
                }
            }
            // Handle l2update (from level2_batch channel)
            else if (msg_type == "l2update") {
                update.is_snapshot = false;
                
                if (msg.contains("product_id")) {
                    update.symbol = msg["product_id"].get<std::string>();
                }
                
                update.exchange = "COINBASE";
                
                if (msg.contains("time")) {
                    // Parse ISO 8601 timestamp if needed, for now use current time
                    update.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();
                } else {
                    update.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();
                }
                
                // Parse changes [side, price, size]
                if (msg.contains("changes") && msg["changes"].is_array()) {
                    for (const auto& change : msg["changes"]) {
                        if (change.is_array() && change.size() >= 3) {
                            std::string side = change[0].get<std::string>();
                            core::PriceLevel level;
                            level.price = std::stod(change[1].get<std::string>());
                            level.quantity = std::stod(change[2].get<std::string>());
                            
                            if (side == "buy") {
                                update.bids.push_back(level);
                            } else if (side == "sell") {
                                update.asks.push_back(level);
                            }
                        }
                    }
                }
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[Coinbase Normalizer] Error: " << e.what() << "\n";
        }
        
        return update;
    }
    
    static std::vector<core::Trade> normalize_trades(const json& msg) {
        std::vector<core::Trade> trades;
        
        try {
            if (!msg.contains("type")) {
                return trades;
            }
            
            std::string msg_type = msg["type"].get<std::string>();
            
            if (msg_type == "match" || msg_type == "last_match") {
                core::Trade trade;
                trade.exchange = "COINBASE";
                
                if (msg.contains("product_id")) {
                    trade.symbol = msg["product_id"].get<std::string>();
                }
                
                if (msg.contains("price")) {
                    trade.price = std::stod(msg["price"].get<std::string>());
                }
                
                if (msg.contains("size")) {
                    trade.quantity = std::stod(msg["size"].get<std::string>());
                }
                
                if (msg.contains("side")) {
                    std::string side = msg["side"].get<std::string>();
                    trade.is_buyer_maker = (side == "sell");
                }
                
                if (msg.contains("time")) {
                    // Parse ISO 8601 timestamp if needed
                    trade.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();
                } else {
                    trade.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count();
                }
                
                trades.push_back(trade);
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[Coinbase Normalizer] Trade error: " << e.what() << "\n";
        }
        
        return trades;
    }
};

} // namespace exchanges
