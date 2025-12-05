#pragma once

// Include necessary Boost Beast libraries for WebSocket and SSL communication
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

// Include JSON library for parsing and constructing JSON messages
#include <nlohmann/json.hpp>
#include <iostream> // For logging and debugging
#include <string>   // For handling string operations
#include <vector>   // For storing multiple symbols

// Namespace aliases for convenience
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

// Binance WebSocket L2 Data Client
class BinanceWSClient {
public:
    // Constructor: Initializes the WebSocket client with a list of symbols
    BinanceWSClient(const std::vector<std::string>& symbols)
        : resolver_(ioc_), // Resolver for DNS lookup
          ctx_(ssl::context::tlsv12_client), // SSL context for secure communication
          ws_(ioc_, ctx_), // WebSocket stream with SSL
          symbols_(symbols) // List of symbols to subscribe to
    {
        // Configure SSL context to use default certificate paths
        ctx_.set_default_verify_paths();
        
        // Disable SSL certificate verification for simplicity (not recommended for production)
        ctx_.set_verify_mode(ssl::verify_none);
    }

    // Connect to the Binance WebSocket server
    void connect() {
        try {
            // Define the Binance WebSocket server host and port
            std::string host = "stream.binance.com";
            std::string port = "9443";
            std::string path = "/ws"; // Base path for WebSocket streams

            // Resolve the host and port to get a list of endpoints
            auto const results = resolver_.resolve(host, port);
            
            // Establish a TCP connection to one of the resolved endpoints
            auto ep = net::connect(get_lowest_layer(ws_), results);
            
            // Perform an SSL handshake to establish a secure connection
            ws_.next_layer().handshake(ssl::stream_base::client);
            
            // Perform a WebSocket handshake to upgrade the connection
            ws_.handshake(host + ":" + std::to_string(ep.port()), path);
            
            // Log successful connection
            std::cout << "Connected to Binance WebSocket" << std::endl;
            
            // Subscribe to the specified symbols
            subscribe();
            
        } catch (std::exception const& e) {
            // Log any connection errors
            std::cerr << "Connect error: " << e.what() << std::endl;
            throw; // Rethrow the exception for higher-level handling
        }
    }

    // Subscribe to the depth streams for the specified symbols
    void subscribe() {
        // Construct the subscription message in JSON format
        json sub_msg;
        sub_msg["method"] = "SUBSCRIBE"; // Binance subscription method
        sub_msg["id"] = 1; // Unique ID for the subscription request
        
        // Create a list of stream names for the symbols
        std::vector<std::string> streams;
        for (const auto& symbol : symbols_) {
            // Normalize the symbol to Binance's format (e.g., BTC-USD → btcusdt@depth20@100ms)
            std::string binance_symbol = normalize_symbol(symbol);
            streams.push_back(binance_symbol + "@depth20@100ms"); // Depth updates every 100ms
        }
        sub_msg["params"] = streams; // Add the streams to the subscription message
        
        // Serialize the JSON message to a string
        std::string msg_str = sub_msg.dump();
        
        // Send the subscription message over the WebSocket
        ws_.write(net::buffer(msg_str));
        
        // Log the subscription
        std::cout << "Subscribed to " << streams.size() << " Binance streams" << std::endl;
    }

    // Read a message from the WebSocket
    std::string read_message() {
        beast::flat_buffer buffer; // Buffer to store the incoming message
        ws_.read(buffer); // Blocking read from the WebSocket
        return beast::buffers_to_string(buffer.data()); // Convert buffer to string and return
    }

    // Close the WebSocket connection gracefully
    void close() {
        try {
            ws_.close(websocket::close_code::normal); // Send a normal close frame
        } catch (...) {
            // Ignore any errors during close
        }
    }

private:
    // Normalize a symbol to Binance's format
    std::string normalize_symbol(const std::string& symbol) {
        // Start with the input symbol (e.g., BTC-USD)
        std::string result = symbol;
        
        // Remove hyphens from the symbol (e.g., BTC-USD → BTCUSD)
        result.erase(std::remove(result.begin(), result.end(), '-'), result.end());
        
        // Convert the symbol to lowercase (e.g., BTCUSD → btcusd)
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        
        // Replace "usd" with "usdt" if "usdt" is not already present
        if (result.find("usd") != std::string::npos && result.find("usdt") == std::string::npos) {
            size_t pos = result.find("usd"); // Find the position of "usd"
            result.insert(pos + 3, "t"); // Insert "t" after "usd" (e.g., btcusd → btcusdt)
        }
        
        return result; // Return the normalized symbol
    }

    // Member variables
    net::io_context ioc_; // IO context for managing asynchronous operations
    tcp::resolver resolver_; // Resolver for DNS lookups
    ssl::context ctx_; // SSL context for secure communication
    websocket::stream<beast::ssl_stream<tcp::socket>> ws_; // WebSocket stream with SSL
    std::vector<std::string> symbols_; // List of symbols to subscribe to
};
