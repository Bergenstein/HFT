// md/kraken_ws_client.hpp
#pragma once
#include <string>
#include <functional>
#include <nlohmann/json.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

namespace md {

using json = nlohmann::json;
typedef websocketpp::client<websocketpp::config::asio_tls_client> client;

class KrakenWSClient {
public:
    using MessageCallback = std::function<void(const std::string&, const json&)>;

    KrakenWSClient(const std::string& exchange_name = "kraken")
        : exchange_(exchange_name), connected_(false) {
        client_.init_asio();
        client_.set_tls_init_handler([this](websocketpp::connection_hdl) {
            return websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12);
        });
        
        client_.set_open_handler([this](websocketpp::connection_hdl hdl) {
            connected_ = true;
            hdl_ = hdl;
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
        websocketpp::lib::error_code ec;
        client::connection_ptr con = client_.get_connection("wss://ws.kraken.com", ec);
        if (ec) {
            std::cerr << "[" << exchange_ << "] Connection error: " << ec.message() << "\n";
            return;
        }
        
        symbols_ = symbols;
        client_.connect(con);
        
        // Run in separate thread to avoid blocking
        std::thread([this]() { client_.run(); }).detach();
        
        // Wait for connection
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // Send subscription message
        if (connected_) {
            send_subscription();
        }
    }

private:
    void send_subscription() {
        json sub = {
            {"event", "subscribe"},
            {"pair", symbols_},
            {"subscription", {{"name", "book"}, {"depth", 10}}}
        };
        
        client_.send(hdl_, sub.dump(), websocketpp::frame::opcode::text);
        std::cout << "[" << exchange_ << "] Subscribed to " << symbols_.size() << " symbols\n";
    }

    void on_message(const std::string& payload) {
        try {
            auto j = json::parse(payload);
            
            // Kraken sends arrays for book updates: [channelID, data, "book", "XBT/USD"]
            if (j.is_array() && j.size() >= 4) {
                std::string symbol = j[3].get<std::string>();
                auto data = j[1];
                
                // Normalize symbol (XBT/USD -> XBTUSD)
                std::string normalized = symbol;
                normalized.erase(std::remove(normalized.begin(), normalized.end(), '/'), normalized.end());
                
                if (message_callback_) {
                    message_callback_(normalized, data);
                }
            } else if (j.contains("event")) {
                std::string event = j["event"];
                if (event == "subscriptionStatus") {
                    std::cout << "[" << exchange_ << "] Subscription status: " 
                              << j.dump() << "\n";
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[" << exchange_ << "] Parse error: " << e.what() << "\n";
        }
    }

    client client_;
    std::string exchange_;
    bool connected_;
    websocketpp::connection_hdl hdl_;
    std::vector<std::string> symbols_;
    MessageCallback message_callback_;
};

} // namespace md
