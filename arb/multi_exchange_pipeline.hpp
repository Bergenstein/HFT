#pragma once

//==============================================================================
// MULTI-EXCHANGE DATA PIPELINE
//==============================================================================
// Hot path: SPSC queues per exchange → Aggregator → Strategy
// Cold path: Async storage to SQLite for backtesting

#include "normalized_exchange_data.hpp"
#include "../pipeline/spsc_queue.hpp"
#include "../storage/sqlite/market_data_store.hpp"
#include <map>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>

namespace arb {

//==============================================================================
// EXCHANGE DATA PIPELINE (One per exchange)
//==============================================================================

class ExchangeDataPipeline {
public:
    using OrderbookQueue = pipeline::SPSCQueue<NormalizedOrderbookSnapshot>;
    using FundingQueue = pipeline::SPSCQueue<FundingRateSnapshot>;
    using UnifiedQueue = pipeline::SPSCQueue<UnifiedMarketData>;
    
    ExchangeDataPipeline(ExchangeID exchange_id, size_t queue_capacity = 1024 * 1024)
        : exchange_id_(exchange_id),
          orderbook_queue_(queue_capacity),
          funding_queue_(queue_capacity / 10),  // Funding updates less frequent
          unified_queue_(queue_capacity),
          running_(false)
    {}
    
    // Push orderbook update (from exchange feeder thread)
    bool push_orderbook(const NormalizedOrderbookSnapshot& snap) {
        return orderbook_queue_.try_push(snap);
    }
    
    // Push funding rate update (from exchange feeder thread)
    bool push_funding(const FundingRateSnapshot& funding) {
        return funding_queue_.try_push(funding);
    }
    
    // Pop unified market data (from strategy thread)
    bool pop_unified(UnifiedMarketData& data) {
        return unified_queue_.try_pop(data);
    }
    
    // Start aggregation thread (merges orderbook + funding)
    void start_aggregation() {
        if (running_.exchange(true)) return;  // Already running
        
        aggregation_thread_ = std::thread([this]() {
            // Cache for latest funding rates AND orderbooks by UNIFIED symbol
            std::map<std::string, FundingRateSnapshot> latest_funding;
            std::map<std::string, NormalizedOrderbookSnapshot> latest_orderbooks;
            
            while (running_.load(std::memory_order_relaxed)) {
                bool processed_any = false;
                
                // Process orderbook updates - store in cache
                NormalizedOrderbookSnapshot ob_snap;
                while (orderbook_queue_.try_pop(ob_snap)) {
                    std::string unified_key = ob_snap.unified_symbol.to_string();
                    latest_orderbooks[unified_key] = ob_snap;
                    processed_any = true;
                }
                
                // Process funding updates - store in cache AND emit unified data
                FundingRateSnapshot funding_snap;
                while (funding_queue_.try_pop(funding_snap)) {
                    std::string unified_key = funding_snap.unified_symbol.to_string();
                    latest_funding[unified_key] = funding_snap;
                    processed_any = true;
                    
                    // If we have orderbook for this symbol, emit unified data NOW
                    auto ob_it = latest_orderbooks.find(unified_key);
                    if (ob_it != latest_orderbooks.end()) {
                        UnifiedMarketData unified;
                        unified.orderbook = ob_it->second;
                        unified.funding = funding_snap;
                        
                        while (!unified_queue_.try_push(std::move(unified))) {
                            if (!running_.load(std::memory_order_relaxed)) break;
                            std::this_thread::yield();
                        }
                    }
                }
                
                // Avoid spinning
                if (!processed_any) {
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                }
            }
        });
    }
    
    void stop_aggregation() {
        running_.store(false, std::memory_order_relaxed);
        if (aggregation_thread_.joinable()) {
            aggregation_thread_.join();
        }
    }
    
    // Stats
    size_t orderbook_queue_size() const { return orderbook_queue_.size(); }
    size_t funding_queue_size() const { return funding_queue_.size(); }
    size_t unified_queue_size() const { return unified_queue_.size(); }
    
    ExchangeID exchange_id() const { return exchange_id_; }
    
    ~ExchangeDataPipeline() {
        stop_aggregation();
    }
    
private:
    ExchangeID exchange_id_;
    OrderbookQueue orderbook_queue_;
    FundingQueue funding_queue_;
    UnifiedQueue unified_queue_;
    std::atomic<bool> running_;
    std::thread aggregation_thread_;
};

//==============================================================================
// MULTI-EXCHANGE AGGREGATOR (Cross-exchange view)
//==============================================================================

class MultiExchangeAggregator {
public:
    using Callback = std::function<void(const UnifiedMarketData&)>;
    
    MultiExchangeAggregator(size_t num_exchanges = 12) {
        // std::map doesn't have reserve
        (void)num_exchanges;
    }
    
    // Add an exchange pipeline
    void add_exchange(ExchangeID exchange_id, size_t queue_capacity = 1024 * 1024) {
        auto pipeline = std::make_shared<ExchangeDataPipeline>(exchange_id, queue_capacity);
        pipelines_[exchange_id] = pipeline;
        pipeline->start_aggregation();
    }
    
    // Get pipeline for specific exchange
    std::shared_ptr<ExchangeDataPipeline> get_pipeline(ExchangeID exchange_id) {
        auto it = pipelines_.find(exchange_id);
        return (it != pipelines_.end()) ? it->second : nullptr;
    }
    
    // Register callback for unified market data
    void on_market_data(Callback callback) {
        callback_ = callback;
    }
    
    // Start consuming from all exchanges
    void start(int cpu_affinity = -1) {
        if (running_.exchange(true)) return;
        
        consumer_thread_ = std::thread([this, cpu_affinity]() {
            // Set CPU affinity if specified (Linux-specific, skip on macOS)
            #ifdef __linux__
            if (cpu_affinity >= 0) {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(cpu_affinity, &cpuset);
                pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
            }
            #else
            (void)cpu_affinity; // Suppress unused parameter warning on macOS
            #endif
            
            while (running_.load(std::memory_order_relaxed)) {
                bool any_data = false;
                
                // Round-robin through all exchanges
                for (auto& [exchange_id, pipeline] : pipelines_) {
                    UnifiedMarketData data;
                    if (pipeline->pop_unified(data)) {
                        any_data = true;
                        
                        // Update cross-exchange view
                        update_cross_exchange_view(data);
                        
                        // Invoke callback
                        if (callback_) {
                            callback_(data);
                        }
                        
                        // Store to cold path (async)
                        if (storage_) {
                            storage_queue_.try_push(data);
                        }
                    }
                }
                
                // Avoid spinning if no data
                if (!any_data) {
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                }
            }
        });
    }
    
    void stop() {
        running_.store(false, std::memory_order_relaxed);
        if (consumer_thread_.joinable()) {
            consumer_thread_.join();
        }
        if (storage_thread_.joinable()) {
            storage_thread_.join();
        }
    }
    
    // Enable cold path storage
    void enable_storage(const std::string& db_path = "multi_exchange_data.db") {
        storage_ = std::make_unique<storage::MarketDataStore>(db_path);
        
        // Start async storage thread
        storage_thread_ = std::thread([this]() {
            while (running_.load(std::memory_order_relaxed) || !storage_queue_.empty()) {
                UnifiedMarketData data;
                if (storage_queue_.try_pop(data)) {
                    // Convert to NormalizedQuote format for existing storage
                    pipeline::NormalizedQuote quote;
                    quote.exchange = exchange_to_string(data.orderbook.exchange_id);
                    quote.product_id = data.orderbook.exchange_symbol;
                    quote.base = data.orderbook.unified_symbol.base;
                    quote.quote = data.orderbook.unified_symbol.quote;
                    quote.best_bid = data.orderbook.best_bid_price;
                    quote.best_ask = data.orderbook.best_ask_price;
                    quote.bid_size = data.orderbook.best_bid_qty;
                    quote.ask_size = data.orderbook.best_ask_qty;
                    quote.sequence = data.orderbook.sequence;
                    quote.bids = data.orderbook.bids;
                    quote.asks = data.orderbook.asks;
                    
                    // Convert nanoseconds to time_point correctly
                    auto ex_duration = std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        std::chrono::nanoseconds(data.orderbook.exchange_timestamp_ns));
                    auto local_duration = std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        std::chrono::nanoseconds(data.orderbook.local_timestamp_ns));
                    quote.exchange_timestamp = std::chrono::system_clock::time_point(ex_duration);
                    quote.local_timestamp = std::chrono::system_clock::time_point(local_duration);
                    
                    storage_->insert_quote(quote);
                    
                    // TODO: Store funding rate data as well
                }
                
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }
    
    // Get cross-exchange view for a symbol
    std::vector<UnifiedMarketData> get_symbol_across_exchanges(const UnifiedSymbol& symbol) const {
        std::lock_guard<std::mutex> lock(cross_view_mutex_);
        
        std::vector<UnifiedMarketData> result;
        auto it = cross_exchange_view_.find(symbol);
        if (it != cross_exchange_view_.end()) {
            for (const auto& [exchange_id, data] : it->second) {
                result.push_back(data);
            }
        }
        return result;
    }
    
    // Find arbitrage opportunities
    std::vector<ArbitrageOpportunity> find_opportunities(
        double min_spread_apy = 20.0,
        double min_correlation = 0.95) const {
        
        std::lock_guard<std::mutex> lock(cross_view_mutex_);
        std::vector<ArbitrageOpportunity> opportunities;
        
        for (const auto& [symbol, exchange_data] : cross_exchange_view_) {
            if (exchange_data.size() < 2) continue;
            
            // Find min and max funding rates
            const UnifiedMarketData* min_funding = nullptr;
            const UnifiedMarketData* max_funding = nullptr;
            
            for (const auto& [ex_id, data] : exchange_data) {
                if (!data.has_funding()) continue;
                
                double funding_annual = data.funding->funding_rate_annual;
                
                if (!min_funding || funding_annual < min_funding->funding->funding_rate_annual) {
                    min_funding = &data;
                }
                if (!max_funding || funding_annual > max_funding->funding->funding_rate_annual) {
                    max_funding = &data;
                }
            }
            
            if (!min_funding || !max_funding) continue;
            
            double spread = max_funding->funding->funding_rate_annual - 
                           min_funding->funding->funding_rate_annual;
            
            if (spread < min_spread_apy) continue;
            
            // Calculate fees
            auto long_fees = ExchangeFees::get(min_funding->orderbook.exchange_id);
            auto short_fees = ExchangeFees::get(max_funding->orderbook.exchange_id);
            double total_fees = (long_fees.maker_fee + long_fees.taker_fee + 
                                short_fees.maker_fee + short_fees.taker_fee) * 100; // Annualized
            
            double net_profit = spread - total_fees;
            
            if (net_profit < min_spread_apy) continue;
            
            // Create opportunity
            ArbitrageOpportunity opp;
            opp.type = ArbitrageOpportunity::Type::FUNDING_RATE;
            opp.symbol = symbol;
            opp.long_exchange = min_funding->orderbook.exchange_id;
            opp.long_symbol = min_funding->orderbook.exchange_symbol;
            opp.long_price = min_funding->orderbook.mid_price();
            opp.long_funding_rate_annual = min_funding->funding->funding_rate_annual;
            opp.short_exchange = max_funding->orderbook.exchange_id;
            opp.short_symbol = max_funding->orderbook.exchange_symbol;
            opp.short_price = max_funding->orderbook.mid_price();
            opp.short_funding_rate_annual = max_funding->funding->funding_rate_annual;
            opp.spread_annual = spread;
            opp.net_profit_apy = net_profit;
            
            // Price difference
            double price_diff = std::abs(opp.long_price - opp.short_price);
            double avg_price = (opp.long_price + opp.short_price) / 2.0;
            opp.price_difference_bps = (price_diff / avg_price) * 10000.0;
            
            // Risk metrics (simplified - would need historical data for correlation)
            opp.correlation = 1.0;  // Assume perfect for same symbol
            opp.liquidity_score = std::min(
                min_funding->orderbook.best_bid_qty + min_funding->orderbook.best_ask_qty,
                max_funding->orderbook.best_bid_qty + max_funding->orderbook.best_ask_qty
            );
            
            opp.detected_timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            opp.num_exchanges_with_symbol = exchange_data.size();
            
            opportunities.push_back(opp);
        }
        
        // Sort by profitability
        std::sort(opportunities.begin(), opportunities.end(),
            [](const ArbitrageOpportunity& a, const ArbitrageOpportunity& b) {
                return a.net_profit_apy > b.net_profit_apy;
            });
        
        return opportunities;
    }
    
    // Print statistics
    void print_stats() const {
        std::cout << "\n=== Multi-Exchange Pipeline Stats ===\n";
        for (const auto& [ex_id, pipeline] : pipelines_) {
            std::cout << exchange_to_string(ex_id) << ":\n";
            std::cout << "  Orderbook queue: " << pipeline->orderbook_queue_size() << "\n";
            std::cout << "  Funding queue: " << pipeline->funding_queue_size() << "\n";
            std::cout << "  Unified queue: " << pipeline->unified_queue_size() << "\n";
        }
        std::cout << "Storage queue: " << storage_queue_.size() << "\n";
        std::cout << "Symbols tracked: " << cross_exchange_view_.size() << "\n";
    }
    
    ~MultiExchangeAggregator() {
        stop();
    }
    
private:
    void update_cross_exchange_view(const UnifiedMarketData& data) {
        std::lock_guard<std::mutex> lock(cross_view_mutex_);
        cross_exchange_view_[data.orderbook.unified_symbol][data.orderbook.exchange_id] = data;
    }
    
    std::map<ExchangeID, std::shared_ptr<ExchangeDataPipeline>> pipelines_;
    
    // Cross-exchange view: symbol -> (exchange -> data)
    mutable std::mutex cross_view_mutex_;
    std::map<UnifiedSymbol, std::map<ExchangeID, UnifiedMarketData>> cross_exchange_view_;
    
    // Consumer thread
    std::atomic<bool> running_{false};
    std::thread consumer_thread_;
    Callback callback_;
    
    // Cold path storage
    std::unique_ptr<storage::MarketDataStore> storage_;
    pipeline::SPSCQueue<UnifiedMarketData> storage_queue_{1024 * 1024};
    std::thread storage_thread_;
};

} // namespace arb
