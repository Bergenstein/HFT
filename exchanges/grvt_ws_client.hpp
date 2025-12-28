// exchanges/grvt_ws_client.hpp - GRVT WebSocket Client for L2 Order Book Data
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

class GRVTWebSocketClient {
public:
    using MessageCallback = std::function<void(const json&)>;
    
    GRVTWebSocketClient(net::io_context& ioc, ssl::context& ctx)
        : resolver_(net::make_strand(ioc))
        , ws_(net::make_strand(ioc), ctx)
        , host_("grvt.io")
        , port_("443")
    {}
    
    void connect(const std::vector<std::string>& symbols, MessageCallback callback) {
        callback_ = callback;
        symbols_ = symbols;
        
        std::cout << "[GRVT] Connecting to " << host_ << ":" << port_ << "\n";
        
        resolver_.async_resolve(
            host_, port_,
            beast::bind_front_handler(&GRVTWebSocketClient::on_resolve, this));
    }
    
    void close() {
        ws_.async_close(websocket::close_code::normal,
            beast::bind_front_handler(&GRVTWebSocketClient::on_close, this));
    }

private:
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (ec) {
            std::cerr << "[GRVT] Resolve failed: " << ec.message() << "\n";
            return;
        }
        
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
        beast::get_lowest_layer(ws_).async_connect(
            results,
            beast::bind_front_handler(&GRVTWebSocketClient::on_connect, this));
    }
    
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
        if (ec) {
            std::cerr << "[GRVT] Connect failed: " << ec.message() << "\n";
            return;
        }
        
        std::cout << "[GRVT] TCP connected to " << ep << "\n";
        
        beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
        
        if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str())) {
            ec = beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
            std::cerr << "[GRVT] SSL SNI failed: " << ec.message() << "\n";
            return;
        }
        
        ws_.next_layer().async_handshake(
            ssl::stream_base::client,
            beast::bind_front_handler(&GRVTWebSocketClient::on_ssl_handshake, this));
    }
    
    void on_ssl_handshake(beast::error_code ec) {
        if (ec) {
            std::cerr << "[GRVT] SSL handshake failed: " << ec.message() << "\n";
            return;
        }
        
        std::cout << "[GRVT] SSL handshake complete\n";
        
        beast::get_lowest_layer(ws_).expires_never();
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::user_agent, "HFT-System/1.0");
                req.set(http::field::sec_websocket_protocol, "graphql-ws");
            }));
        
        ws_.async_handshake(host_, "/graphql",
            beast::bind_front_handler(&GRVTWebSocketClient::on_handshake, this));
    }
    
    void on_handshake(beast::error_code ec) {
        if (ec) {
            std::cerr << "[GRVT] WebSocket handshake failed: " << ec.message() << "\n";
            return;
        }
        
        std::cout << "[GRVT] WebSocket connected\n";
        
        // Subscribe to L2 order book updates for all symbols
        json sub_msg;
        sub_msg["method"] = "subscribe";
        sub_msg["params"]["channels"] = json::array();
        
        for (const auto& symbol : symbols_) {
            sub_msg["params"]["channels"].push_back("orderbook." + symbol);
        }
        
        std::string sub_str = sub_msg.dump();
        std::cout << "[GRVT] Subscribing: " << sub_str << "\n";
        
        ws_.async_write(
            net::buffer(sub_str),
            beast::bind_front_handler(&GRVTWebSocketClient::on_write, this));
    }
    
    void on_write(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        
        if (ec) {
            std::cerr << "[GRVT] Write failed: " << ec.message() << "\n";
            return;
        }
        
        do_read();
    }
    
    void do_read() {
        ws_.async_read(
            buffer_,
            beast::bind_front_handler(&GRVTWebSocketClient::on_read, this));
    }
    
    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        
        if (ec) {
            std::cerr << "[GRVT] Read failed: " << ec.message() << "\n";
            return;
        }
        
        try {
            std::string msg_str = beast::buffers_to_string(buffer_.data());
            json msg = json::parse(msg_str);
            
            if (callback_) {
                callback_(msg);
            }
        } catch (const std::exception& e) {
            std::cerr << "[GRVT] Parse error: " << e.what() << "\n";
        }
        
        buffer_.consume(buffer_.size());
        do_read();
    }
    
    void on_close(beast::error_code ec) {
        if (ec) {
            std::cerr << "[GRVT] Close failed: " << ec.message() << "\n";
            return;
        }
        std::cout << "[GRVT] Connection closed\n";
    }
    
    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;
    std::string host_;
    std::string port_;
    std::vector<std::string> symbols_;
    MessageCallback callback_;
};

} // namespace exchanges
