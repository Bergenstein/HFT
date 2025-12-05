// ============================================================================
// core/config_loader.hpp - JSON Configuration Loader
// ============================================================================
// 
// Loads system configuration from JSON files, allowing runtime configuration
// of exchanges, strategies, CPU pinning, and more.
//
// Usage:
//   auto config = core::ConfigLoader::load("config/system_config.json");
//   if (config.cpu_pinning.enabled) {
//       core::pin_thread_to_core(config.cpu_pinning.market_data_core);
//   }
//
// ============================================================================

#pragma once
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace core {

using json = nlohmann::json;

// ============================================================================
// Configuration Structures
// ============================================================================

struct CPUPinningConfig {
    bool enabled = false;
    int market_data_core = 0;
    int strategy_core = 1;
    int order_execution_core = 2;
    int logging_core = 3;
    bool reserve_cores_for_os = true;
};

struct ExchangeCredentials {
    std::string api_key;
    std::string api_secret;
    std::string passphrase;
    
    bool has_credentials() const {
        return !api_key.empty() && !api_secret.empty();
    }
};

struct ExchangeConfig {
    bool enabled = false;
    std::string name;
    std::string ws_url;
    std::string rest_url;
    ExchangeCredentials credentials;
    bool sandbox = false;
    double maker_fee_bps = 0.0;
    double taker_fee_bps = 0.0;
    int rate_limit_per_sec = 10;
    std::vector<std::string> products;
};

struct SQLiteConfig {
    bool enabled = true;
    std::string database_path = "./market_data.db";
    int batch_size = 100;
    int flush_interval_ms = 1000;
};

struct InMemoryCacheConfig {
    bool enabled = true;
    int max_quotes_per_product = 1000;
    int max_trades_per_product = 5000;
};

struct StorageConfig {
    SQLiteConfig sqlite;
    InMemoryCacheConfig cache;
};

struct ZeroMQConfig {
    bool enabled = true;
    std::string publisher_endpoint = "tcp://*:5555";
    int subscriber_timeout_ms = 100;
};

struct StrategyConfig {
    bool enabled = false;
    std::map<std::string, double> params;
};

struct RiskConfig {
    double max_daily_loss_usd = 1000.0;
    double max_position_per_product_usd = 10000.0;
    double max_total_position_usd = 50000.0;
    double stop_loss_percent = 2.0;
};

struct BacktestConfig {
    double initial_capital = 50000.0;
    std::string data_source = "sqlite";
    double commission_bps = 10.0;
    double slippage_bps = 5.0;
};

struct SystemConfig {
    std::string mode = "demo";
    std::string logging_level = "INFO";
    bool enable_cpu_pinning = false;
    int num_worker_threads = 4;
    std::string data_directory = "./data";
    
    CPUPinningConfig cpu_pinning;
    std::map<std::string, ExchangeConfig> exchanges;
    StorageConfig storage;
    ZeroMQConfig zeromq;
    std::map<std::string, StrategyConfig> strategies;
    RiskConfig risk;
    BacktestConfig backtest;
};

// ============================================================================
// ConfigLoader Class
// ============================================================================

class ConfigLoader {
public:
    /**
     * Load configuration from JSON file
     * 
     * @param config_path Path to the JSON config file
     * @return SystemConfig with loaded values
     */
    static SystemConfig load(const std::string& config_path) {
        SystemConfig config;
        
        std::ifstream file(config_path);
        if (!file.is_open()) {
            std::cerr << "[CONFIG] Warning: Could not open " << config_path 
                      << ", using defaults\n";
            return config;
        }
        
        try {
            json j = json::parse(file);
            
            // System settings
            if (j.contains("system")) {
                auto& sys = j["system"];
                config.mode = sys.value("mode", config.mode);
                config.logging_level = sys.value("logging_level", config.logging_level);
                config.enable_cpu_pinning = sys.value("enable_cpu_pinning", config.enable_cpu_pinning);
                config.num_worker_threads = sys.value("num_worker_threads", config.num_worker_threads);
                config.data_directory = sys.value("data_directory", config.data_directory);
            }
            
            // CPU Pinning
            if (j.contains("cpu_pinning")) {
                auto& cpu = j["cpu_pinning"];
                config.cpu_pinning.enabled = cpu.value("enabled", false);
                config.cpu_pinning.market_data_core = cpu.value("market_data_core", 0);
                config.cpu_pinning.strategy_core = cpu.value("strategy_core", 1);
                config.cpu_pinning.order_execution_core = cpu.value("order_execution_core", 2);
                config.cpu_pinning.logging_core = cpu.value("logging_core", 3);
                config.cpu_pinning.reserve_cores_for_os = cpu.value("reserve_cores_for_os", true);
            }
            
            // Exchanges
            if (j.contains("exchanges")) {
                for (auto& [name, ex] : j["exchanges"].items()) {
                    ExchangeConfig ec;
                    ec.name = name;
                    ec.enabled = ex.value("enabled", false);
                    ec.ws_url = ex.value("ws_url", "");
                    ec.rest_url = ex.value("rest_url", "");
                    ec.sandbox = ex.value("sandbox", false);
                    ec.maker_fee_bps = ex.value("maker_fee_bps", 0.0);
                    ec.taker_fee_bps = ex.value("taker_fee_bps", 0.0);
                    ec.rate_limit_per_sec = ex.value("rate_limit_per_sec", 10);
                    
                    // Resolve environment variables for credentials
                    ec.credentials.api_key = resolve_env_var(ex.value("api_key", ""));
                    ec.credentials.api_secret = resolve_env_var(ex.value("api_secret", ""));
                    ec.credentials.passphrase = resolve_env_var(ex.value("passphrase", ""));
                    
                    if (ex.contains("products") && ex["products"].is_array()) {
                        for (const auto& p : ex["products"]) {
                            ec.products.push_back(p.get<std::string>());
                        }
                    }
                    
                    config.exchanges[name] = ec;
                }
            }
            
            // Storage
            if (j.contains("storage")) {
                auto& st = j["storage"];
                if (st.contains("sqlite")) {
                    config.storage.sqlite.enabled = st["sqlite"].value("enabled", true);
                    config.storage.sqlite.database_path = st["sqlite"].value("database_path", "./market_data.db");
                    config.storage.sqlite.batch_size = st["sqlite"].value("batch_size", 100);
                    config.storage.sqlite.flush_interval_ms = st["sqlite"].value("flush_interval_ms", 1000);
                }
                if (st.contains("in_memory_cache")) {
                    config.storage.cache.enabled = st["in_memory_cache"].value("enabled", true);
                    config.storage.cache.max_quotes_per_product = st["in_memory_cache"].value("max_quotes_per_product", 1000);
                    config.storage.cache.max_trades_per_product = st["in_memory_cache"].value("max_trades_per_product", 5000);
                }
            }
            
            // ZeroMQ
            if (j.contains("zeromq")) {
                config.zeromq.enabled = j["zeromq"].value("enabled", true);
                config.zeromq.publisher_endpoint = j["zeromq"].value("publisher_endpoint", "tcp://*:5555");
                config.zeromq.subscriber_timeout_ms = j["zeromq"].value("subscriber_timeout_ms", 100);
            }
            
            // Risk
            if (j.contains("risk_management")) {
                auto& rm = j["risk_management"];
                config.risk.max_daily_loss_usd = rm.value("max_daily_loss_usd", 1000.0);
                config.risk.max_position_per_product_usd = rm.value("max_position_per_product_usd", 10000.0);
                config.risk.max_total_position_usd = rm.value("max_total_position_usd", 50000.0);
                config.risk.stop_loss_percent = rm.value("stop_loss_percent", 2.0);
            }
            
            // Backtesting
            if (j.contains("backtesting")) {
                auto& bt = j["backtesting"];
                config.backtest.initial_capital = bt.value("initial_capital", 50000.0);
                config.backtest.data_source = bt.value("data_source", "sqlite");
                config.backtest.commission_bps = bt.value("commission_bps", 10.0);
                config.backtest.slippage_bps = bt.value("slippage_bps", 5.0);
            }
            
            std::cout << "[CONFIG] ✓ Loaded configuration from " << config_path << "\n";
            
        } catch (const json::parse_error& e) {
            std::cerr << "[CONFIG] Error parsing " << config_path << ": " << e.what() << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[CONFIG] Error loading " << config_path << ": " << e.what() << "\n";
        }
        
        return config;
    }
    
    /**
     * Get list of enabled exchanges
     */
    static std::vector<std::string> get_enabled_exchanges(const SystemConfig& config) {
        std::vector<std::string> enabled;
        for (const auto& [name, ec] : config.exchanges) {
            if (ec.enabled) {
                enabled.push_back(name);
            }
        }
        return enabled;
    }
    
    /**
     * Get products for a specific exchange
     */
    static std::vector<std::string> get_exchange_products(const SystemConfig& config, 
                                                          const std::string& exchange) {
        auto it = config.exchanges.find(exchange);
        if (it != config.exchanges.end()) {
            return it->second.products;
        }
        return {};
    }
    
    /**
     * Print configuration summary
     */
    static void print_summary(const SystemConfig& config) {
        std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                    SYSTEM CONFIGURATION                       ║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ Mode: " << config.mode << "\n";
        std::cout << "║ CPU Pinning: " << (config.cpu_pinning.enabled ? "ENABLED" : "DISABLED") << "\n";
        std::cout << "║ Worker Threads: " << config.num_worker_threads << "\n";
        std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ EXCHANGES:\n";
        for (const auto& [name, ec] : config.exchanges) {
            std::cout << "║   " << name << ": " << (ec.enabled ? "✓" : "✗") 
                      << " (" << ec.products.size() << " products)\n";
        }
        std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ STORAGE:\n";
        std::cout << "║   SQLite: " << (config.storage.sqlite.enabled ? "✓" : "✗") 
                  << " → " << config.storage.sqlite.database_path << "\n";
        std::cout << "║   In-Memory Cache: " << (config.storage.cache.enabled ? "✓" : "✗") << "\n";
        std::cout << "║   ZeroMQ: " << (config.zeromq.enabled ? "✓" : "✗") 
                  << " → " << config.zeromq.publisher_endpoint << "\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
    }
    
private:
    /**
     * Resolve environment variable placeholders like ${VAR_NAME}
     */
    static std::string resolve_env_var(const std::string& value) {
        if (value.size() >= 4 && value.substr(0, 2) == "${" && value.back() == '}') {
            std::string var_name = value.substr(2, value.size() - 3);
            const char* env_value = std::getenv(var_name.c_str());
            return env_value ? std::string(env_value) : "";
        }
        return value;
    }
};

} // namespace core
