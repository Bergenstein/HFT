// md/okx_ws_client.hpp
#pragma once
#include <string>
#include <functional>
#include <nlohmann/json.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

namespace md {

using json = nlohmann::json;
typedef websocketpp::client<websocketpp::config::asio_tls_client> client;

class OKXWSClient {
public:
    using MessageCallback = std::function<void(const std::string&, const json&)>;

    OKXWSClient(const std::string& exchange_name = "okx")
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
        client::connection_ptr con = client_.get_connection(
            "wss://ws.okx.com:8443/ws/v5/public", ec);
        if (ec) {
            std::cerr << "[" << exchange_ << "] Connection error: " << ec.message() << "\n";
            return;
        }
        
        symbols_ = symbols;
        client_.connect(con);
        
        std::thread([this]() { client_.run(); }).detach();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        if (connected_) {
            send_subscription();
        }
    }

private:
    void send_subscription() {
        // OKX format: {"op": "subscribe", "args": [{"channel": "books", "instId": "BTC-USDT"}]}
        json args = json::array();
        for (const auto& symbol : symbols_) {
            args.push_back({{"channel", "books"}, {"instId", symbol}});
        }
        
        json sub = {{"op", "subscribe"}, {"args", args}};
        
        client_.send(hdl_, sub.dump(), websocketpp::frame::opcode::text);
        std::cout << "[" << exchange_ << "] Subscribed to " << symbols_.size() << " symbols\n";
    }

    void on_message(const std::string& payload) {
        try {
            auto j = json::parse(payload);
            
            // OKX sends: {"arg": {"channel": "books", "instId": "BTC-USDT"}, "data": [...]}
            if (j.contains("arg") && j.contains("data")) {
                std::string symbol = j["arg"]["instId"];
                auto data = j["data"];
                
                if (message_callback_ && !data.empty()) {
                    message_callback_(symbol, data[0]); // OKX wraps in array
                }
            } else if (j.contains("event")) {
                std::cout << "[" << exchange_ << "] Event: " << j.dump() << "\n";
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
