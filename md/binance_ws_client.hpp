// md/binance_ws_client.hpp
#pragma once
#include <string>
#include <functional>
#include <nlohmann/json.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

namespace md {

using json = nlohmann::json;
typedef websocketpp::client<websocketpp::config::asio_tls_client> client;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> context_ptr;

class BinanceWSClient {
public:
    using MessageCallback = std::function<void(const std::string&, const json&)>;

    BinanceWSClient(const std::string& exchange_name = "binance")
        : exchange_(exchange_name), connected_(false) {
        client_.init_asio();
        client_.set_tls_init_handler([this](websocketpp::connection_hdl) {
            return websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12);
        });
        
        client_.set_open_handler([this](websocketpp::connection_hdl hdl) {
            connected_ = true;
            std::cout << "[" << exchange_ << "] Connected\n";
        });
        
        client_.set_message_handler([this](websocketpp::connection_hdl, client::message_ptr msg) {
            on_message(msg->get_payload());
        });
        
        client_.set_fail_handler([this](websocketpp::connection_hdl) {
            connected_ = false;
            std::cerr << "[" << exchange_ << "] Connection failed\n";
        });
        
        client_.set_close_handler([this](websocketpp::connection_hdl) {
            connected_ = false;
            std::cout << "[" << exchange_ << "] Disconnected\n";
        });
    }

    void set_message_callback(MessageCallback cb) {
        message_callback_ = cb;
    }

    void subscribe_orderbook(const std::vector<std::string>& symbols) {
        // Binance format: btcusdt@depth@100ms
        std::string url = "wss://stream.binance.com:9443/stream?streams=";
        for (size_t i = 0; i < symbols.size(); ++i) {
            std::string lower = symbols[i];
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            url += lower + "@depth@100ms";
            if (i < symbols.size() - 1) url += "/";
        }
        
        websocketpp::lib::error_code ec;
        client::connection_ptr con = client_.get_connection(url, ec);
        if (ec) {
            std::cerr << "[" << exchange_ << "] Connection error: " << ec.message() << "\n";
            return;
        }
        
        client_.connect(con);
        client_.run();
    }

private:
    void on_message(const std::string& payload) {
        try {
            auto j = json::parse(payload);
            
            // Binance streams wrap data in {"stream": "...", "data": {...}}
            if (j.contains("stream") && j.contains("data")) {
                std::string stream = j["stream"];
                auto data = j["data"];
                
                // Extract symbol from stream (e.g., "btcusdt@depth@100ms" -> "btcusdt")
                size_t at_pos = stream.find('@');
                std::string symbol = stream.substr(0, at_pos);
                
                // Normalize to uppercase
                std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
                
                if (message_callback_) {
                    message_callback_(symbol, data);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[" << exchange_ << "] Parse error: " << e.what() << "\n";
        }
    }

    client client_;
    std::string exchange_;
    bool connected_;
    MessageCallback message_callback_;
};

} // namespace md
