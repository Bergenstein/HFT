// sim/order.hpp - Order types and state management for exchange simulator
#pragma once

#include <string>
#include <chrono>
#include <cstdint>

namespace sim {

enum class OrderSide { BUY, SELL };
enum class OrderType { LIMIT, MARKET, STOP_LIMIT, STOP_MARKET };
enum class TimeInForce { GTC, IOC, FOK, GTD };  // Good-Till-Cancel, Immediate-Or-Cancel, Fill-Or-Kill, Good-Till-Date

enum class OrderStatus {
    PENDING,      // Submitted but not yet acknowledged
    OPEN,         // Active in order book
    PARTIALLY_FILLED,
    FILLED,       // Completely filled
    CANCELLED,    // Cancelled by user
    REJECTED,     // Rejected by exchange
    EXPIRED       // Expired (GTD orders)
};

struct Order {
    std::string order_id;
    std::string client_order_id;
    std::string product_id;
    
    OrderSide side;
    OrderType type;
    TimeInForce time_in_force;
    OrderStatus status;
    
    double price;           // Limit price (0 for market orders)
    double stop_price;      // Stop trigger price (for stop orders)
    double size;            // Original order size
    double filled_size;     // Amount filled so far
    double remaining_size;  // size - filled_size
    
    double avg_fill_price;  // Volume-weighted average fill price
    double total_fees;      // Accumulated fees
    
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    
    // Constructor
    Order(const std::string& id, const std::string& client_id, 
          const std::string& product, OrderSide s, OrderType t, 
          double p, double sz)
        : order_id(id), client_order_id(client_id), product_id(product),
          side(s), type(t), time_in_force(TimeInForce::GTC),
          status(OrderStatus::PENDING), price(p), stop_price(0.0),
          size(sz), filled_size(0.0), remaining_size(sz),
          avg_fill_price(0.0), total_fees(0.0),
          created_at(std::chrono::system_clock::now()),
          updated_at(std::chrono::system_clock::now()) {}
    
    // Update fill information
    void add_fill(double fill_size, double fill_price, double fee) {
        filled_size += fill_size;
        remaining_size = size - filled_size;
        
        // Update average fill price (volume-weighted)
        avg_fill_price = (avg_fill_price * (filled_size - fill_size) + 
                         fill_price * fill_size) / filled_size;
        
        total_fees += fee;
        updated_at = std::chrono::system_clock::now();
        
        // Update status
        if (remaining_size <= 1e-8) {
            status = OrderStatus::FILLED;
        } else if (filled_size > 0) {
            status = OrderStatus::PARTIALLY_FILLED;
        }
    }
    
    // Check if order is active (can be filled)
    bool is_active() const {
        return status == OrderStatus::OPEN || status == OrderStatus::PARTIALLY_FILLED;
    }
    
    // Check if order is terminal (no more updates)
    bool is_terminal() const {
        return status == OrderStatus::FILLED || 
               status == OrderStatus::CANCELLED || 
               status == OrderStatus::REJECTED ||
               status == OrderStatus::EXPIRED;
    }
};

// Fill event (execution)
struct Fill {
    std::string fill_id;
    std::string order_id;
    std::string product_id;
    OrderSide side;
    
    double price;
    double size;
    double fee;
    std::string fee_currency;
    
    std::chrono::system_clock::time_point timestamp;
    
    // Liquidity flag: true = maker, false = taker
    bool is_maker;
    
    Fill(const std::string& fid, const std::string& oid, 
         const std::string& product, OrderSide s,
         double p, double sz, double f, const std::string& fee_ccy, bool maker)
        : fill_id(fid), order_id(oid), product_id(product), side(s),
          price(p), size(sz), fee(f), fee_currency(fee_ccy),
          timestamp(std::chrono::system_clock::now()), is_maker(maker) {}
};

// Helper functions for string conversion
inline std::string order_status_to_string(OrderStatus status) {
    switch (status) {
        case OrderStatus::PENDING: return "PENDING";
        case OrderStatus::OPEN: return "OPEN";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED: return "FILLED";
        case OrderStatus::CANCELLED: return "CANCELLED";
        case OrderStatus::REJECTED: return "REJECTED";
        case OrderStatus::EXPIRED: return "EXPIRED";
        default: return "UNKNOWN";
    }
}

inline std::string order_side_to_string(OrderSide side) {
    return (side == OrderSide::BUY) ? "BUY" : "SELL";
}

inline std::string order_type_to_string(OrderType type) {
    switch (type) {
        case OrderType::LIMIT: return "LIMIT";
        case OrderType::MARKET: return "MARKET";
        case OrderType::STOP_LIMIT: return "STOP_LIMIT";
        case OrderType::STOP_MARKET: return "STOP_MARKET";
        default: return "UNKNOWN";
    }
}

} // namespace sim
