//==============================================================================
// sim/production_matching_engine_impl.hpp - Implementation Details
//==============================================================================

#pragma once

#include "production_matching_engine.hpp"

namespace sim {

// Match limit order
template<typename BookType>
void ProductionMatchingEngine::match_limit_order(
    std::shared_ptr<Order> order, bool post_only, bool reduce_only) {
    
    // Get opposite book
    auto& opposite_book = (order->side == OrderSide::BUY) ? asks_ : bids_;
    
    // Check if we can match immediately
    while (order->remaining_size > 1e-10 && !opposite_book.empty()) {
        auto& best_level = opposite_book.begin()->second;
        
        // Check if price crosses
        bool can_match = (order->side == OrderSide::BUY) ? 
                        (order->price >= best_level.price) :
                        (order->price <= best_level.price);
        
        if (!can_match) break;
        
        // Post-only orders reject if they would take liquidity
        if (post_only) {
            order->status = OrderStatus::REJECTED;
            return;
        }
        
        // Match against front of queue
        auto& front_order = best_level.orders.front();
        double fill_size = std::min(order->remaining_size, front_order->remaining_size);
        double fill_price = best_level.price;
        
        // Apply realistic slippage for large orders
        if (config_.enable_slippage_model && order->size > config_.avg_trade_size * 3) {
            double slippage_bps = std::log(order->size / config_.avg_trade_size) * 0.5;
            fill_price *= (order->side == OrderSide::BUY) ? 
                         (1.0 + slippage_bps / 10000.0) :
                         (1.0 - slippage_bps / 10000.0);
        }
        
        // Create fills
        add_fill(front_order, order, fill_price, fill_size, true);  // maker
        add_fill(order, front_order, fill_price, fill_size, false); // taker
        
        // Update sizes
        order->remaining_size -= fill_size;
        order->filled_size += fill_size;
        front_order->remaining_size -= fill_size;
        front_order->filled_size += fill_size;
        best_level.remove_size(fill_size);
        
        // Update recent volume
        recent_volume_ += fill_size;
        
        // Remove fully filled maker order
        if (front_order->remaining_size < 1e-10) {
            front_order->status = OrderStatus::FILLED;
            if (order_update_callback_) order_update_callback_(*front_order);
            best_level.orders.pop_front();
        } else {
            front_order->status = OrderStatus::PARTIALLY_FILLED;
            if (order_update_callback_) order_update_callback_(*front_order);
        }
        
        // Remove empty level
        if (best_level.total_size < 1e-10) {
            opposite_book.erase(opposite_book.begin());
        }
    }
    
    // Add remaining size to book
    if (order->remaining_size > 1e-10) {
        if (order->side == OrderSide::BUY) {
            add_to_book(bids_, order);
        } else {
            add_to_book(asks_, order);
        }
        
        if (order->filled_size > 0) {
            order->status = OrderStatus::PARTIALLY_FILLED;
        }
        
        // Notify queue position
        if (queue_update_callback_ && config_.enable_queue_position) {
            auto pos = get_queue_position(order->order_id);
            if (pos) queue_update_callback_(order->order_id, *pos);
        }
    } else {
        order->status = OrderStatus::FILLED;
    }
}

// Match market order
void ProductionMatchingEngine::match_market_order(
    std::shared_ptr<Order> order, bool post_only) {
    
    if (post_only) {
        order->status = OrderStatus::REJECTED;
        return;
    }
    
    // Get opposite book
    auto& opposite_book = (order->side == OrderSide::BUY) ? asks_ : bids_;
    
    // Walk through price levels
    while (order->remaining_size > 1e-10 && !opposite_book.empty()) {
        auto& best_level = opposite_book.begin()->second;
        
        // Match as much as possible at this level
        while (order->remaining_size > 1e-10 && !best_level.orders.empty()) {
            auto& front_order = best_level.orders.front();
            double fill_size = std::min(order->remaining_size, front_order->remaining_size);
            double fill_price = best_level.price;
            
            // Apply market impact for large orders
            if (config_.enable_slippage_model) {
                double depth_consumed = order->filled_size / (best_level.total_size + order->filled_size);
                double impact_bps = depth_consumed * 5.0;  // 5bps per 100% of level consumed
                fill_price *= (order->side == OrderSide::BUY) ?
                             (1.0 + impact_bps / 10000.0) :
                             (1.0 - impact_bps / 10000.0);
            }
            
            // Create fills
            add_fill(front_order, order, fill_price, fill_size, true);
            add_fill(order, front_order, fill_price, fill_size, false);
            
            // Update sizes
            order->remaining_size -= fill_size;
            order->filled_size += fill_size;
            front_order->remaining_size -= fill_size;
            front_order->filled_size += fill_size;
            best_level.remove_size(fill_size);
            
            recent_volume_ += fill_size;
            
            // Remove fully filled maker order
            if (front_order->remaining_size < 1e-10) {
                front_order->status = OrderStatus::FILLED;
                if (order_update_callback_) order_update_callback_(*front_order);
                best_level.orders.pop_front();
            }
        }
        
        // Remove empty level
        if (best_level.total_size < 1e-10) {
            opposite_book.erase(opposite_book.begin());
        }
    }
    
    // Market orders are either filled or rejected (no posting)
    order->status = (order->remaining_size < 1e-10) ? 
                    OrderStatus::FILLED : OrderStatus::REJECTED;
}

// Add order to bid book
void ProductionMatchingEngine::add_to_book(
    std::map<double, EnhancedPriceLevel, std::greater<double>>& book,
    std::shared_ptr<Order> order) {
    
    auto it = book.find(order->price);
    if (it == book.end()) {
        it = book.emplace(order->price, EnhancedPriceLevel(order->price)).first;
    }
    it->second.add_order(order);
}

// Add order to ask book
void ProductionMatchingEngine::add_to_book(
    std::map<double, EnhancedPriceLevel, std::less<double>>& book,
    std::shared_ptr<Order> order) {
    
    auto it = book.find(order->price);
    if (it == book.end()) {
        it = book.emplace(order->price, EnhancedPriceLevel(order->price)).first;
    }
    it->second.add_order(order);
}

// Remove order from book
template<typename BookType>
void ProductionMatchingEngine::remove_from_book(
    BookType& book, std::shared_ptr<Order> order) {
    
    auto level_it = book.find(order->price);
    if (level_it == book.end()) return;
    
    auto& level = level_it->second;
    auto& orders = level.orders;
    
    // Remove order from deque
    orders.erase(
        std::remove_if(orders.begin(), orders.end(),
                      [&](const auto& o) { return o->order_id == order->order_id; }),
        orders.end()
    );
    
    level.remove_size(order->remaining_size);
    
    // Remove empty level
    if (level.total_size < 1e-10) {
        book.erase(level_it);
    }
}

// Process maker fills based on fill probability model
template<typename BookType>
void ProductionMatchingEngine::process_maker_fills(
    BookType& book, double elapsed_time_ms) {
    
    for (auto& [price, level] : book) {
        for (auto it = level.orders.begin(); it != level.orders.end(); ) {
            auto& order = *it;
            
            // Get queue position
            auto pos = level.get_queue_position(order->order_id);
            int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
            double time_in_queue_ms = (now - order->timestamp) / 1e6;
            
            // Calculate fill probability
            double fill_prob = FillProbabilityModel::calculate_fill_probability(
                pos, time_in_queue_ms, recent_volume_, config_.avg_trade_size
            );
            
            // Random fill based on probability
            std::uniform_real_distribution<> dis(0.0, 1.0);
            double roll = dis(rng_);
            
            if (roll < fill_prob * (elapsed_time_ms / 1000.0)) {
                // Simulate a fill (partial or full)
                std::uniform_real_distribution<> size_dis(0.1, 1.0);
                double fill_fraction = size_dis(rng_);
                double fill_size = std::min(order->remaining_size, 
                                          order->remaining_size * fill_fraction);
                
                // Create fill
                Fill fill;
                fill.fill_id = "FILL-" + std::to_string(next_fill_id_++);
                fill.order_id = order->order_id;
                fill.product_id = product_id_;
                fill.side = order->side;
                fill.price = price;
                fill.size = fill_size;
                fill.fee = (fill_size * price * config_.maker_rebate_bps / 10000.0);
                fill.is_maker = true;
                fill.timestamp = now;
                
                if (fill_callback_) fill_callback_(fill);
                
                // Update order
                order->remaining_size -= fill_size;
                order->filled_size += fill_size;
                order->fills.push_back(fill);
                level.remove_size(fill_size);
                recent_volume_ += fill_size;
                
                // Update status
                if (order->remaining_size < 1e-10) {
                    order->status = OrderStatus::FILLED;
                    if (order_update_callback_) order_update_callback_(*order);
                    it = level.orders.erase(it);
                    continue;
                } else {
                    order->status = OrderStatus::PARTIALLY_FILLED;
                    if (order_update_callback_) order_update_callback_(*order);
                }
            }
            
            ++it;
        }
        
        // Remove empty level
        if (level.total_size < 1e-10) {
            book.erase(price);
        }
    }
}

// Check if order would self-trade
bool ProductionMatchingEngine::would_self_trade(
    std::shared_ptr<Order> order, const std::string& client_id) {
    
    auto& opposite_book = (order->side == OrderSide::BUY) ? asks_ : bids_;
    
    if (opposite_book.empty()) return false;
    
    auto& best_level = opposite_book.begin()->second;
    
    // Check if price would cross
    bool crosses = (order->side == OrderSide::BUY) ?
                  (order->price >= best_level.price) :
                  (order->price <= best_level.price);
    
    if (!crosses) return false;
    
    // Check if any orders at that level belong to same client
    for (const auto& existing_order : best_level.orders) {
        auto client_it = client_orders_.find(client_id);
        if (client_it != client_orders_.end()) {
            if (client_it->second.count(existing_order->order_id) > 0) {
                return true;
            }
        }
    }
    
    return false;
}

// Handle self-trade
void ProductionMatchingEngine::handle_self_trade(
    std::shared_ptr<Order> order, const std::string& client_id) {
    
    if (config_.stp_mode == "CANCEL_NEWEST") {
        order->status = OrderStatus::REJECTED;
    } else if (config_.stp_mode == "CANCEL_OLDEST") {
        // Cancel conflicting orders
        auto& opposite_book = (order->side == OrderSide::BUY) ? asks_ : bids_;
        auto& best_level = opposite_book.begin()->second;
        
        for (auto it = best_level.orders.begin(); it != best_level.orders.end(); ) {
            auto& existing_order = *it;
            auto client_it = client_orders_.find(client_id);
            if (client_it != client_orders_.end() && 
                client_it->second.count(existing_order->order_id) > 0) {
                existing_order->status = OrderStatus::CANCELED;
                if (order_update_callback_) order_update_callback_(*existing_order);
                it = best_level.orders.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// Create fill record
void ProductionMatchingEngine::add_fill(
    std::shared_ptr<Order> maker_order,
    std::shared_ptr<Order> taker_order,
    double fill_price,
    double fill_size,
    bool is_maker) {
    
    auto order = is_maker ? maker_order : taker_order;
    
    Fill fill;
    fill.fill_id = "FILL-" + std::to_string(next_fill_id_++);
    fill.order_id = order->order_id;
    fill.product_id = product_id_;
    fill.side = order->side;
    fill.price = fill_price;
    fill.size = fill_size;
    fill.is_maker = is_maker;
    fill.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    
    // Calculate fee
    double fee_bps = is_maker ? config_.maker_rebate_bps : config_.taker_fee_bps;
    fill.fee = (fill_size * fill_price * fee_bps / 10000.0);
    
    order->fills.push_back(fill);
    order->avg_fill_price = (order->avg_fill_price * (order->filled_size - fill_size) + 
                             fill_price * fill_size) / order->filled_size;
    
    if (fill_callback_) fill_callback_(fill);
}

} // namespace sim
