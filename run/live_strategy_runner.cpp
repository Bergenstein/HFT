// run/live_strategy_runner.cpp
// Strategy engine that subscribes to ZeroMQ feed and generates signals
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include "../zmq/strategy_subscriber.hpp"
#include "../strats/live_strategy_engine.hpp"

std::atomic<bool> g_running{true};

void signal_handler(int sig) {
    std::cout << "\n[MAIN] Caught signal " << sig << ", shutting down...\n";
    g_running.store(false);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::cout << "========================================\n";
    std::cout << "  LIVE STRATEGY ENGINE\n";
    std::cout << "========================================\n\n";
    
    // Parse command line arguments
    std::string zmq_endpoint = "tcp://localhost:5555";
    if (argc > 1) {
        zmq_endpoint = argv[1];
    }
    
    // ===================================================================
    // STEP 1: Create strategy engine and add strategies
    // ===================================================================
    std::cout << "[ENGINE] Initializing strategy engine...\n";
    
    strats::LiveStrategyEngine engine;
    
    // Add strategy 1: Imbalance on BTC-USD
    engine.add_strategy(
        std::make_unique<strats::LiveImbalanceStrategy>(
            "coinbase", "BTC-USD", 0.6, 150
        )
    );
    
    // Add strategy 2: Imbalance on ETH-USD
    engine.add_strategy(
        std::make_unique<strats::LiveImbalanceStrategy>(
            "coinbase", "ETH-USD", 0.7, 100
        )
    );
    
    // Add strategy 3: Imbalance on SOL-USD
    engine.add_strategy(
        std::make_unique<strats::LiveImbalanceStrategy>(
            "coinbase", "SOL-USD", 0.65, 120
        )
    );
    
    std::cout << "[ENGINE] ✓ Loaded " << engine.num_strategies() << " strategies\n\n";
    
    // ===================================================================
    // STEP 2: Connect to ZeroMQ feed
    // ===================================================================
    std::cout << "[SUBSCRIBER] Connecting to " << zmq_endpoint << "...\n";
    
    hft::StrategySubscriber subscriber(zmq_endpoint);
    
    // Subscribe to all products
    subscriber.subscribe_all();
    
    std::cout << "[SUBSCRIBER] ✓ Connected and subscribed\n\n";
    
    // ===================================================================
    // STEP 3: Register callbacks
    // ===================================================================
    
    // Callback when quote arrives
    subscriber.on_quote([&engine](const pipeline::NormalizedQuote& quote) {
        engine.on_quote(quote);
        
        // Check for signals
        auto signals = engine.get_signals();
        for (const auto& signal : signals) {
            std::cout << signal.to_string() << "\n";
        }
    });
    
    // Callback when trade arrives
    subscriber.on_trade([&engine](const pipeline::NormalizedTrade& trade) {
        engine.on_trade(trade);
    });
    
    std::cout << "========================================\n";
    std::cout << "  STRATEGY ENGINE RUNNING\n";
    std::cout << "========================================\n";
    std::cout << "ZeroMQ endpoint:   " << zmq_endpoint << "\n";
    std::cout << "Active strategies: " << engine.num_strategies() << "\n";
    std::cout << "Press Ctrl+C to stop\n";
    std::cout << "========================================\n\n";
    
    // ===================================================================
    // STEP 4: Run subscriber in background, print stats periodically
    // ===================================================================
    
    subscriber.run_async();
    
    // Statistics monitoring
    auto last_time = std::chrono::steady_clock::now();
    
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_time
        ).count();
        
        auto sub_stats = subscriber.get_stats();
        auto eng_stats = engine.get_stats();
        
        std::cout << "\n========================================\n";
        std::cout << "  STRATEGY ENGINE STATISTICS\n";
        std::cout << "========================================\n";
        std::cout << "Subscriber:\n";
        std::cout << "  Quotes received: " << sub_stats.quotes_received << "\n";
        std::cout << "  Trades received: " << sub_stats.trades_received << "\n";
        std::cout << "  Errors:          " << sub_stats.errors << "\n";
        std::cout << "\nEngine:\n";
        std::cout << "  Quotes processed: " << eng_stats.quotes_processed << "\n";
        std::cout << "  Trades processed: " << eng_stats.trades_processed << "\n";
        std::cout << "  Signals generated: " << eng_stats.signals_generated << "\n";
        std::cout << "========================================\n\n";
        
        last_time = now;
    }
    
    // ===================================================================
    // STEP 5: Cleanup
    // ===================================================================
    
    std::cout << "\n[SHUTDOWN] Stopping subscriber...\n";
    subscriber.stop();
    
    auto final_stats = engine.get_stats();
    
    std::cout << "\n========================================\n";
    std::cout << "  FINAL STATISTICS\n";
    std::cout << "========================================\n";
    std::cout << "Total quotes processed: " << final_stats.quotes_processed << "\n";
    std::cout << "Total trades processed: " << final_stats.trades_processed << "\n";
    std::cout << "Total signals generated: " << final_stats.signals_generated << "\n";
    std::cout << "========================================\n\n";
    
    std::cout << "[MAIN] Shutdown complete\n";
    
    return 0;
}
