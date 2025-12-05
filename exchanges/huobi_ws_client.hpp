// exchanges/huobi_ws_client.hpp
#pragma once
#include <string>
#include <functional>
#include <nlohmann/json.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <zlib.h>

namespace md {

using json = nlohmann::json;
typedef websocketpp::client<websocketpp::config::asio_tls_client> client;

class HuobiWSClient {
public:
    using MessageCallback = std::function<void(const std::string&, const json&)>;

    HuobiWSClient(const std::string& exchange_name = "huobi")
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
            "wss://api.huobi.pro/ws", ec);
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
        // Huobi format: {"sub": "market.btcusdt.depth.step0", "id": "id1"}
        for (size_t i = 0; i < symbols_.size(); ++i) {
            json sub = {
                {"sub", "market." + symbols_[i] + ".depth.step0"},
                {"id", "id" + std::to_string(i + 1)}
            };
            
            client_.send(hdl_, sub.dump(), websocketpp::frame::opcode::text);
        }
        
        std::cout << "[" << exchange_ << "] Subscribed to " << symbols_.size() << " symbols\n";
    }

    std::string decompress_gzip(const std::string& compressed) {
        // Huobi sends gzip-compressed data
        // Simplified decompression (in production, use proper zlib)
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        
        if (inflateInit2(&zs, MAX_WBITS + 16) != Z_OK) {
            return "";
        }
        
        zs.next_in = (Bytef*)compressed.data();
        zs.avail_in = compressed.size();
        
        std::string output;
        char buffer[32768];
        
        do {
            zs.next_out = reinterpret_cast<Bytef*>(buffer);
            zs.avail_out = sizeof(buffer);
            
            int ret = inflate(&zs, 0);
            
            if (output.size() < zs.total_out) {
                output.append(buffer, zs.total_out - output.size());
            }
            
            if (ret == Z_STREAM_END) break;
        } while (zs.avail_out == 0);
        
        inflateEnd(&zs);
        return output;
    }

    void on_message(const std::string& payload) {
        try {
            // Huobi sends gzip-compressed JSON
            std::string decompressed = decompress_gzip(payload);
            if (decompressed.empty()) {
                decompressed = payload; // Not compressed
            }
            
            auto j = json::parse(decompressed);
            
            // Handle ping/pong
            if (j.contains("ping")) {
                json pong = {{"pong", j["ping"]}};
                client_.send(hdl_, pong.dump(), websocketpp::frame::opcode::text);
                return;
            }
            
            // Huobi market data: {"ch": "market.btcusdt.depth.step0", "tick": {...}}
            if (j.contains("ch") && j.contains("tick")) {
                std::string channel = j["ch"];
                
                // Extract symbol from channel (e.g., "market.btcusdt.depth.step0" -> "btcusdt")
                size_t start = channel.find("market.") + 7;
                size_t end = channel.find(".depth");
                if (start != std::string::npos && end != std::string::npos) {
                    std::string symbol = channel.substr(start, end - start);
                    
                    // Normalize to uppercase
                    std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);
                    
                    auto data = j["tick"];
                    
                    if (message_callback_) {
                        message_callback_(symbol, data);
                    }
                }
            } else if (j.contains("subbed")) {
                std::cout << "[" << exchange_ << "] Subscription confirmed: " 
                          << j["subbed"] << "\n";
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
