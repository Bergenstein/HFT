//==============================================================================
// LOAD FUNDING RATE CSV DATA INTO SQLITE
//==============================================================================
// Loads funding_history_*.csv files into SQLite database for backtesting

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sqlite3.h>

struct FundingRecord {
    std::string exchange;
    std::string symbol;
    double funding_rate;
    int64_t funding_time_ms;
};

bool load_csv(const std::string& filepath, std::vector<FundingRecord>& records) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open: " << filepath << "\n";
        return false;
    }
    
    std::string line;
    std::getline(file, line); // Skip header
    
    int count = 0;
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string exchange, symbol, rate_str, time_str;
        
        std::getline(ss, exchange, ',');
        std::getline(ss, symbol, ',');
        std::getline(ss, rate_str, ',');
        std::getline(ss, time_str, ',');
        
        if (exchange.empty() || symbol.empty()) continue;
        
        FundingRecord rec;
        rec.exchange = exchange;
        rec.symbol = symbol;
        rec.funding_rate = std::stod(rate_str);
        rec.funding_time_ms = std::stoll(time_str);
        
        records.push_back(rec);
        count++;
    }
    
    std::cout << "Loaded " << count << " records from " << filepath << "\n";
    return true;
}

int main(int argc, char* argv[]) {
    std::string db_path = argc > 1 ? argv[1] : "db/funding_rates.db";
    
    std::cout << "\n=== Loading Funding Rate Data into SQLite ===\n\n";
    
    // Open/create database
    sqlite3* db;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Failed to open database: " << db_path << "\n";
        return 1;
    }
    
    std::cout << "Database: " << db_path << "\n\n";
    
    // Create table
    const char* create_sql = R"(
        CREATE TABLE IF NOT EXISTS funding_rates (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp INTEGER NOT NULL,
            exchange TEXT NOT NULL,
            symbol TEXT NOT NULL,
            funding_rate REAL NOT NULL,
            mark_price REAL DEFAULT 0,
            index_price REAL DEFAULT 0,
            open_interest REAL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_funding_time ON funding_rates(timestamp);
        CREATE INDEX IF NOT EXISTS idx_funding_exchange_symbol ON funding_rates(exchange, symbol);
    )";
    
    char* err_msg;
    if (sqlite3_exec(db, create_sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Failed to create table: " << err_msg << "\n";
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return 1;
    }
    
    std::cout << "[1/4] Table created\n";
    
    // Clear existing data
    sqlite3_exec(db, "DELETE FROM funding_rates", nullptr, nullptr, nullptr);
    std::cout << "[2/4] Cleared existing data\n";
    
    // Load CSV files
    std::vector<FundingRecord> all_records;
    std::vector<std::string> csv_files = {
        "data/funding_history_BTCUSDT.csv",
        "data/funding_history_ETHUSDT.csv",
        "data/funding_history_SOLUSDT.csv"
    };
    
    std::cout << "[3/4] Loading CSV files:\n";
    for (const auto& csv_file : csv_files) {
        load_csv(csv_file, all_records);
    }
    
    std::cout << "\n[4/4] Inserting " << all_records.size() << " records into database...\n";
    
    // Begin transaction for speed
    sqlite3_exec(db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
    
    const char* insert_sql = R"(
        INSERT INTO funding_rates (timestamp, exchange, symbol, funding_rate)
        VALUES (?, ?, ?, ?)
    )";
    
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr);
    
    int inserted = 0;
    for (const auto& rec : all_records) {
        sqlite3_bind_int64(stmt, 1, rec.funding_time_ms);
        sqlite3_bind_text(stmt, 2, rec.exchange.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, rec.symbol.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 4, rec.funding_rate);
        
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            inserted++;
        }
        
        sqlite3_reset(stmt);
    }
    
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    
    std::cout << "✓ Inserted " << inserted << " records\n\n";
    
    // Verify data
    const char* verify_sql = R"(
        SELECT 
            COUNT(*) as total,
            COUNT(DISTINCT exchange) as exchanges,
            COUNT(DISTINCT symbol) as symbols,
            MIN(timestamp) as min_ts,
            MAX(timestamp) as max_ts
        FROM funding_rates
    )";
    
    sqlite3_prepare_v2(db, verify_sql, -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int total = sqlite3_column_int(stmt, 0);
        int exchanges = sqlite3_column_int(stmt, 1);
        int symbols = sqlite3_column_int(stmt, 2);
        int64_t min_ts = sqlite3_column_int64(stmt, 3);
        int64_t max_ts = sqlite3_column_int64(stmt, 4);
        
        std::cout << "=== Database Summary ===\n";
        std::cout << "Total records: " << total << "\n";
        std::cout << "Exchanges: " << exchanges << "\n";
        std::cout << "Symbols: " << symbols << "\n";
        std::cout << "Time range: " << min_ts << " to " << max_ts << "\n";
        
        int64_t duration_ms = max_ts - min_ts;
        double days = duration_ms / (1000.0 * 3600.0 * 24.0);
        std::cout << "Duration: " << std::fixed << std::setprecision(1) << days << " days\n";
    }
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    
    std::cout << "\n✅ Successfully loaded funding rate data into: " << db_path << "\n\n";
    
    return 0;
}
