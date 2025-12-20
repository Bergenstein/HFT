#pragma once

//==============================================================================
// L2 FETCHER → PIPELINE INTEGRATION
//==============================================================================
// Bridges multi_exchange_l2_fetcher.hpp with multi_exchange_pipeline.hpp

#include "multi_exchange_l2_fetcher.hpp"
#include "multi_exchange_pipeline.hpp"
#include <thread>
#include <atomic>
#include <chrono>

namespace arb {

//==============================================================================
// EXCHANGE FEEDER (Polls REST API → Pushes to pipeline)
//==============================================================================

class ExchangeFeeder {
public:
    using FetchFunc = std::function<std::vector<ExchangeMarketData>(int)>;
    
    ExchangeFeeder(ExchangeID exchange_id,
                   std::shared_ptr<ExchangeDataPipeline> pipeline,
                   FetchFunc fetch_func,
                   int poll_interval_ms = 1000,
                   int l2_depth = 20)
        : exchange_id_(exchange_id),
          pipeline_(pipeline),
          fetch_func_(fetch_func),
          poll_interval_ms_(poll_interval_ms),
          l2_depth_(l2_depth),
          running_(false)
    {}
    
    void start() {
        if (running_.exchange(true)) return;
        
        feeder_thread_ = std::thread([this]() {
            std::cout << "[Feeder] Started for " << exchange_to_string(exchange_id_) << "\n";
            
            while (running_.load(std::memory_order_relaxed)) {
                auto start = std::chrono::steady_clock::now();
                
                try {
                    // Fetch data from exchange
                    auto markets = fetch_func_(l2_depth_);
                    
                    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    
                    // Convert and push to pipeline
                    for (const auto& market : markets) {
                        // Convert orderbook
                        NormalizedOrderbookSnapshot ob_snap;
                        ob_snap.exchange_id = exchange_id_;
                        ob_snap.exchange_symbol = market.orderbook.symbol;
                        ob_snap.unified_symbol = UnifiedSymbol::normalize(market.orderbook.symbol);
                        ob_snap.exchange_timestamp_ns = market.orderbook.exchange_timestamp_ms * 1000000;
                        ob_snap.local_timestamp_ns = now_ns;
                        ob_snap.sequence = 0;  // REST doesn't have sequence
                        
                        if (!market.orderbook.bids.empty()) {
                            ob_snap.best_bid_price = market.orderbook.bids[0].price;
                            ob_snap.best_bid_qty = market.orderbook.bids[0].quantity;
                        }
                        if (!market.orderbook.asks.empty()) {
                            ob_snap.best_ask_price = market.orderbook.asks[0].price;
                            ob_snap.best_ask_qty = market.orderbook.asks[0].quantity;
                        }
                        
                        ob_snap.bids.reserve(market.orderbook.bids.size());
                        for (const auto& level : market.orderbook.bids) {
                            ob_snap.bids.emplace_back(level.price, level.quantity);
                        }
                        
                        ob_snap.asks.reserve(market.orderbook.asks.size());
                        for (const auto& level : market.orderbook.asks) {
                            ob_snap.asks.emplace_back(level.price, level.quantity);
                        }
                        
                        // Push orderbook
                        if (!pipeline_->push_orderbook(ob_snap)) {
                            std::cerr << "[Feeder] Orderbook queue full for " 
                                     << exchange_to_string(exchange_id_) << "\n";
                        }
                        
                        // Convert and push funding rate if available
                        if (market.funding) {
                            FundingRateSnapshot fr_snap;
                            fr_snap.exchange_id = exchange_id_;
                            fr_snap.exchange_symbol = market.funding->symbol;
                            fr_snap.unified_symbol = UnifiedSymbol::normalize(market.funding->symbol);
                            fr_snap.timestamp_ns = market.funding->timestamp_ms * 1000000;
                            fr_snap.next_funding_time_ns = market.funding->next_funding_time_ms * 1000000;
                            fr_snap.funding_rate = market.funding->funding_rate;
                            fr_snap.funding_rate_annual = market.funding->funding_rate_annual;
                            fr_snap.funding_interval_hours = market.funding->funding_interval_hours;
                            fr_snap.mark_price = market.funding->mark_price;
                            fr_snap.index_price = market.funding->index_price;
                            
                            if (!pipeline_->push_funding(fr_snap)) {
                                std::cerr << "[Feeder] Funding queue full for " 
                                         << exchange_to_string(exchange_id_) << "\n";
                            }
                        }
                    }
                    
                    stats_successful_polls_++;
                    stats_last_market_count_ = markets.size();
                    
                } catch (const std::exception& e) {
                    std::cerr << "[Feeder] Error in " << exchange_to_string(exchange_id_) 
                             << ": " << e.what() << "\n";
                    stats_failed_polls_++;
                }
                
                // Sleep for remaining time in interval
                auto elapsed = std::chrono::steady_clock::now() - start;
                auto sleep_time = std::chrono::milliseconds(poll_interval_ms_) - elapsed;
                if (sleep_time > std::chrono::milliseconds(0)) {
                    std::this_thread::sleep_for(sleep_time);
                }
            }
            
            std::cout << "[Feeder] Stopped for " << exchange_to_string(exchange_id_) << "\n";
        });
    }
    
    void stop() {
        running_.store(false, std::memory_order_relaxed);
        if (feeder_thread_.joinable()) {
            feeder_thread_.join();
        }
    }
    
    // Statistics
    struct Stats {
        uint64_t successful_polls;
        uint64_t failed_polls;
        uint64_t last_market_count;
    };
    
    Stats get_stats() const {
        return {stats_successful_polls_, stats_failed_polls_, stats_last_market_count_};
    }
    
    ~ExchangeFeeder() {
        stop();
    }
    
private:
    ExchangeID exchange_id_;
    std::shared_ptr<ExchangeDataPipeline> pipeline_;
    FetchFunc fetch_func_;
    int poll_interval_ms_;
    int l2_depth_;
    
    std::atomic<bool> running_;
    std::thread feeder_thread_;
    
    std::atomic<uint64_t> stats_successful_polls_{0};
    std::atomic<uint64_t> stats_failed_polls_{0};
    std::atomic<uint64_t> stats_last_market_count_{0};
};

//==============================================================================
// INTEGRATED MULTI-EXCHANGE SYSTEM
//==============================================================================

class MultiExchangeSystem {
public:
    MultiExchangeSystem() : aggregator_() {}
    
    // Add exchange with auto-wiring
    void add_exchange(ExchangeID exchange_id, 
                     MultiExchangeL2Fetcher::FetchFunc fetch_func,
                     int poll_interval_ms = 1000,
                     int l2_depth = 20) {
        
        // Create pipeline for this exchange
        aggregator_.add_exchange(exchange_id);
        auto pipeline = aggregator_.get_pipeline(exchange_id);
        if (!pipeline) {
            throw std::runtime_error("Failed to create pipeline for exchange");
        }
        
        // Create feeder that polls REST API and pushes to pipeline
        auto feeder = std::make_shared<ExchangeFeeder>(
            exchange_id, pipeline, fetch_func, poll_interval_ms, l2_depth);
        
        feeders_[exchange_id] = feeder;
    }
    
    // Start all feeders and aggregator
    void start(bool enable_storage = true, 
               const std::string& db_path = "multi_exchange_data.db",
               int consumer_cpu_affinity = -1) {
        
        if (enable_storage) {
            aggregator_.enable_storage(db_path);
        }
        
        aggregator_.start(consumer_cpu_affinity);
        
        for (auto& [ex_id, feeder] : feeders_) {
            feeder->start();
        }
        
        std::cout << "[System] Started " << feeders_.size() << " exchange feeders\n";
    }
    
    void stop() {
        for (auto& [ex_id, feeder] : feeders_) {
            feeder->stop();
        }
        aggregator_.stop();
        std::cout << "[System] Stopped all feeders and aggregator\n";
    }
    
    // Register callback for market data
    void on_market_data(MultiExchangeAggregator::Callback callback) {
        aggregator_.on_market_data(callback);
    }
    
    // Get arbitrage opportunities
    std::vector<ArbitrageOpportunity> find_opportunities(
        double min_spread_apy = 20.0,
        double min_correlation = 0.95) {
        return aggregator_.find_opportunities(min_spread_apy, min_correlation);
    }
    
    // Get cross-exchange view
    std::vector<UnifiedMarketData> get_symbol_across_exchanges(const UnifiedSymbol& symbol) {
        return aggregator_.get_symbol_across_exchanges(symbol);
    }
    
    // Print statistics
    void print_stats() {
        std::cout << "\n=== Exchange Feeder Stats ===\n";
        for (const auto& [ex_id, feeder] : feeders_) {
            auto stats = feeder->get_stats();
            std::cout << exchange_to_string(ex_id) << ":\n";
            std::cout << "  Successful polls: " << stats.successful_polls << "\n";
            std::cout << "  Failed polls: " << stats.failed_polls << "\n";
            std::cout << "  Last market count: " << stats.last_market_count << "\n";
        }
        aggregator_.print_stats();
    }
    
    ~MultiExchangeSystem() {
        stop();
    }
    
private:
    MultiExchangeAggregator aggregator_;
    std::map<ExchangeID, std::shared_ptr<ExchangeFeeder>> feeders_;
};

//==============================================================================
// CONVENIENCE BUILDER
//==============================================================================

class MultiExchangeSystemBuilder {
public:
    MultiExchangeSystemBuilder& with_binance(int interval_ms = 1000) {
        exchanges_.push_back({ExchangeID::BINANCE, 
                             MultiExchangeL2Fetcher::fetch_binance, interval_ms});
        return *this;
    }
    
    MultiExchangeSystemBuilder& with_bybit(int interval_ms = 1000) {
        exchanges_.push_back({ExchangeID::BYBIT, 
                             MultiExchangeL2Fetcher::fetch_bybit, interval_ms});
        return *this;
    }
    
    MultiExchangeSystemBuilder& with_okx(int interval_ms = 2000) {
        exchanges_.push_back({ExchangeID::OKX, 
                             MultiExchangeL2Fetcher::fetch_okx, interval_ms});
        return *this;
    }
    
    MultiExchangeSystemBuilder& with_gateio(int interval_ms = 1000) {
        exchanges_.push_back({ExchangeID::GATEIO, 
                             MultiExchangeL2Fetcher::fetch_gateio, interval_ms});
        return *this;
    }
    
    MultiExchangeSystemBuilder& with_mexc(int interval_ms = 1000) {
        exchanges_.push_back({ExchangeID::MEXC, 
                             MultiExchangeL2Fetcher::fetch_mexc, interval_ms});
        return *this;
    }
    
    MultiExchangeSystemBuilder& with_kucoin(int interval_ms = 1000) {
        exchanges_.push_back({ExchangeID::KUCOIN, 
                             MultiExchangeL2Fetcher::fetch_kucoin, interval_ms});
        return *this;
    }
    
    MultiExchangeSystemBuilder& with_kraken(int interval_ms = 1000) {
        exchanges_.push_back({ExchangeID::KRAKEN, 
                             MultiExchangeL2Fetcher::fetch_kraken, interval_ms});
        return *this;
    }
    
    MultiExchangeSystemBuilder& with_bitget(int interval_ms = 1000) {
        exchanges_.push_back({ExchangeID::BITGET, 
                             MultiExchangeL2Fetcher::fetch_bitget, interval_ms});
        return *this;
    }
    
    MultiExchangeSystemBuilder& with_htx(int interval_ms = 1000) {
        exchanges_.push_back({ExchangeID::HTX, 
                             MultiExchangeL2Fetcher::fetch_htx, interval_ms});
        return *this;
    }
    
    MultiExchangeSystemBuilder& with_bingx(int interval_ms = 1000) {
        exchanges_.push_back({ExchangeID::BINGX, 
                             MultiExchangeL2Fetcher::fetch_bingx, interval_ms});
        return *this;
    }
    
    MultiExchangeSystemBuilder& with_all_exchanges() {
        return with_binance()
               .with_bybit()
               .with_okx()
               .with_gateio()
               .with_mexc()
               .with_kucoin()
               .with_kraken()
               .with_bitget()
               .with_htx()
               .with_bingx();
    }
    
    std::unique_ptr<MultiExchangeSystem> build() {
        auto system = std::make_unique<MultiExchangeSystem>();
        
        for (const auto& [ex_id, fetch_func, interval_ms] : exchanges_) {
            system->add_exchange(ex_id, fetch_func, interval_ms);
        }
        
        return system;
    }
    
private:
    struct ExchangeConfig {
        ExchangeID exchange_id;
        MultiExchangeL2Fetcher::FetchFunc fetch_func;
        int interval_ms;
    };
    
    std::vector<ExchangeConfig> exchanges_;
};

} // namespace arb
