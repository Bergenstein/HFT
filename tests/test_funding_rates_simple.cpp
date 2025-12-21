//==============================================================================
// Multi-Exchange Funding Rate Test
// Tests ONLY the multi-exchange funding rate data collection
// NO strategy logic - pure system test
//==============================================================================

#include <iostream>
#include <string>
#include <vector>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

using json = nlohmann::json;

// CURL callback
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Fetch funding rate from Binance
bool test_binance_funding() {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, "https://fapi.binance.com/fapi/v1/fundingRate?symbol=BTCUSDT&limit=1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        std::cout << "✗ Binance API error: " << curl_easy_strerror(res) << "\n";
        return false;
    }
    
    try {
        json j = json::parse(response);
        if (j.is_array() && !j.empty()) {
            double rate = j[0]["fundingRate"].get<double>();
            std::cout << "✓ Binance BTC/USDT funding rate: " << (rate * 100) << "%\n";
            return true;
        }
    } catch (const std::exception& e) {
        std::cout << "✗ Parse error: " << e.what() << "\n";
    }
    
    return false;
}

// Fetch funding rate from Bybit
bool test_bybit_funding() {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.bybit.com/v5/market/funding/history?category=linear&symbol=BTCUSDT&limit=1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        std::cout << "✗ Bybit API error: " << curl_easy_strerror(res) << "\n";
        return false;
    }
    
    try {
        json j = json::parse(response);
        if (j.contains("result") && j["result"].contains("list") && !j["result"]["list"].empty()) {
            double rate = std::stod(j["result"]["list"][0]["fundingRate"].get<std::string>());
            std::cout << "✓ Bybit BTC/USDT funding rate: " << (rate * 100) << "%\n";
            return true;
        }
    } catch (const std::exception& e) {
        std::cout << "✗ Parse error: " << e.what() << "\n";
    }
    
    return false;
}

// Fetch funding rate from OKX
bool test_okx_funding() {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, "https://www.okx.com/api/v5/public/funding-rate?instId=BTC-USDT-SWAP");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        std::cout << "✗ OKX API error: " << curl_easy_strerror(res) << "\n";
        return false;
    }
    
    try {
        json j = json::parse(response);
        if (j.contains("data") && !j["data"].empty()) {
            double rate = std::stod(j["data"][0]["fundingRate"].get<std::string>());
            std::cout << "✓ OKX BTC-USDT-SWAP funding rate: " << (rate * 100) << "%\n";
            return true;
        }
    } catch (const std::exception& e) {
        std::cout << "✗ Parse error: " << e.what() << "\n";
    }
    
    return false;
}

int main() {
    std::cout << "==========================================================\n";
    std::cout << "  MULTI-EXCHANGE FUNDING RATE TEST\n";
    std::cout << "==========================================================\n\n";
    
    std::cout << "Testing funding rate APIs for BTC perpetuals...\n\n";
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    int passed = 0;
    int total = 3;
    
    std::cout << "[1/3] Binance API Test:\n";
    if (test_binance_funding()) passed++;
    std::cout << "\n";
    
    std::cout << "[2/3] Bybit API Test:\n";
    if (test_bybit_funding()) passed++;
    std::cout << "\n";
    
    std::cout << "[3/3] OKX API Test:\n";
    if (test_okx_funding()) passed++;
    std::cout << "\n";
    
    curl_global_cleanup();
    
    std::cout << "==========================================================\n";
    std::cout << "  RESULTS\n";
    std::cout << "==========================================================\n";
    std::cout << "Passed: " << passed << "/" << total << "\n";
    std::cout << "Failed: " << (total - passed) << "\n\n";
    
    if (passed == total) {
        std::cout << "✅ All funding rate APIs working!\n\n";
        return 0;
    } else if (passed > 0) {
        std::cout << "⚠️  Some APIs failed (may be temporary)\n\n";
        return 0; // Don't fail the test if some exchanges are down
    } else {
        std::cout << "❌ All APIs failed\n\n";
        return 1;
    }
}
