// exchanges/coinbase_ws_client.hpp - REAL Coinbase WebSocket Client
#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <functional>
#include <iostream>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

namespace exchanges {

class CoinbaseWebSocketClient {
public:
    using MessageCallback = std::function<void(const json&)>;
    
    CoinbaseWebSocketClient(net::io_context& ioc, ssl::context& ctx)
        : resolver_(net::make_strand(ioc))
        , ws_(net::make_strand(ioc), ctx)
        , host_("ws-feed.exchange.coinbase.com")
        , port_("443")
    {}
    
    void connect(const std::vector<std::string>& product_ids, MessageCallback callback) {
        callback_ = callback;
        product_ids_ = product_ids;
        
        std::cout << "[Coinbase] Connecting to " << host_ << ":" << port_ << "\n";
        
        resolver_.async_resolve(
            host_, port_,
            beast::bind_front_handler(&CoinbaseWebSocketClient::on_resolve, this));
    }
    
    void close() {
        ws_.async_close(websocket::close_code::normal,
            beast::bind_front_handler(&CoinbaseWebSocketClient::on_close, this));
    }

private:
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) {
            std::cerr << "[Coinbase] Resolve failed: " << ec.message() << "\n";
            return;
        }
        
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
        beast::get_lowest_layer(ws_).async_connect(
            results,
            beast::bind_front_handler(&CoinbaseWebSocketClient::on_connect, this));
    }
    
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
        if (ec) {
            std::cerr << "[Coinbase] Connect failed: " << ec.message() << "\n";
            return;
        }
        
        std::cout << "[Coinbase] TCP connected to " << ep << "\n";
        
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
        
        if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str())) {
            ec = beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
            std::cerr << "[Coinbase] SSL SNI failed: " << ec.message() << "\n";
            return;
        }
        
        ws_.next_layer().async_handshake(
            ssl::stream_base::client,
            beast::bind_front_handler(&CoinbaseWebSocketClient::on_ssl_handshake, this));
    }
    
    void on_ssl_handshake(beast::error_code ec) {
        if (ec) {
            std::cerr << "[Coinbase] SSL handshake failed: " << ec.message() << "\n";
            return;
        }
        
        std::cout << "[Coinbase] SSL handshake complete\n";
        
        beast::get_lowest_layer(ws_).expires_never();
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::user_agent, "HFT-System/1.0");
            }));
        
        ws_.async_handshake(host_, "/",
            beast::bind_front_handler(&CoinbaseWebSocketClient::on_handshake, this));
    }
    
    void on_handshake(beast::error_code ec) {
        if (ec) {
            std::cerr << "[Coinbase] WebSocket handshake failed: " << ec.message() << "\n";
            return;
        }
        
        std::cout << "[Coinbase] WebSocket connected\n";
        
        // Subscribe to level2 channel
        json subscribe_msg = {
            {"type", "subscribe"},
            {"product_ids", product_ids_},
            {"channels", {
                {
                    {"name", "level2"},
                    {"product_ids", product_ids_}
                }
            }}
        };
        
        std::string msg_str = subscribe_msg.dump();
        std::cout << "[Coinbase] Subscribing: " << msg_str << "\n";
        
        ws_.async_write(
            net::buffer(msg_str),
            beast::bind_front_handler(&CoinbaseWebSocketClient::on_write, this));
    }
    
    void on_write(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        
        if (ec) {
            std::cerr << "[Coinbase] Write failed: " << ec.message() << "\n";
            return;
        }
        
        std::cout << "[Coinbase] Subscription sent, waiting for messages...\n";
        do_read();
    }
    
    void do_read() {
        ws_.async_read(
            buffer_,
            beast::bind_front_handler(&CoinbaseWebSocketClient::on_read, this));
    }
    
    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        
        if (ec) {
            std::cerr << "[Coinbase] Read failed: " << ec.message() << "\n";
            return;
        }
        
        try {
            std::string msg = beast::buffers_to_string(buffer_.data());
            json j = json::parse(msg);
            
            if (callback_) {
                callback_(j);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Coinbase] JSON parse error: " << e.what() << "\n";
        }
        
        buffer_.consume(buffer_.size());
        do_read();
    }
    
    void on_close(beast::error_code ec) {
        if (ec) {
            std::cerr << "[Coinbase] Close failed: " << ec.message() << "\n";
        }
        std::cout << "[Coinbase] Connection closed\n";
    }
    
    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;
    std::string host_;
    std::string port_;
    std::vector<std::string> product_ids_;
    MessageCallback callback_;
};

} // namespace exchanges
