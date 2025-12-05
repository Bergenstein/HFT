// exchanges/bybit_ws_client.hpp
#pragma once

// Include necessary libraries for WebSocket communication and JSON handling
#include <string>       // For handling string operations
#include <functional>   // For using std::function as a callback
#include <nlohmann/json.hpp> // JSON library for parsing and constructing JSON messages
#include <websocketpp/config/asio_client.hpp> // WebSocket++ configuration for ASIO client
#include <websocketpp/client.hpp> // WebSocket++ client implementation

namespace md {

using json = nlohmann::json; // Alias for nlohmann::json for convenience
typedef websocketpp::client<websocketpp::config::asio_tls_client> client; // Define WebSocket++ client type

class BybitWSClient {
public:
    // Define a callback type for handling messages (symbol and JSON data)
    using MessageCallback = std::function<void(const std::string&, const json&)>;

    // Constructor: Initializes the WebSocket client with an optional exchange name
    BybitWSClient(const std::string& exchange_name = "bybit")
        : exchange_(exchange_name), // Set the exchange name (default: "bybit")
          connected_(false) // Initialize connection status as false
    {
        client_.init_asio(); // Initialize ASIO for the WebSocket++ client

        // Set up TLS (SSL) initialization handler
        client_.set_tls_init_handler([this](websocketpp::connection_hdl) {
            return websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12);
            // Create and return a shared pointer to an SSL context using TLS v1.2
        });
        
        // Set up handler for when the WebSocket connection is successfully opened
        client_.set_open_handler([this](websocketpp::connection_hdl hdl) {
            connected_ = true; // Mark the connection as established
            hdl_ = hdl; // Store the connection handle
            std::cout << "[" << exchange_ << "] Connected\n"; // Log the connection
        });
        
        // Set up handler for incoming messages
        client_.set_message_handler([this](websocketpp::connection_hdl, client::message_ptr msg) {
            on_message(msg->get_payload()); // Process the message payload
        });
        
        // Set up handler for connection failures
        client_.set_fail_handler([this](websocketpp::connection_hdl) {
            connected_ = false; // Mark the connection as failed
            std::cerr << "[" << exchange_ << "] Connection failed\n"; // Log the failure
        });
        
        // Set up handler for when the WebSocket connection is closed
        client_.set_close_handler([this](websocketpp::connection_hdl) {
            connected_ = false; // Mark the connection as closed
            std::cout << "[" << exchange_ << "] Disconnected\n"; // Log the disconnection
        });
    }

    // Set the callback function for processing messages
    void set_message_callback(MessageCallback cb) {
        message_callback_ = cb; // Store the callback function
    }

    // Subscribe to the order book streams for the specified symbols
    void subscribe_orderbook(const std::vector<std::string>& symbols) {
        websocketpp::lib::error_code ec; // Error code for handling connection errors

        // Create a WebSocket connection to the Bybit public spot stream
        client::connection_ptr con = client_.get_connection(
            "wss://stream.bybit.com/v5/public/spot", ec);
        
        if (ec) { // Check for connection errors
            std::cerr << "[" << exchange_ << "] Connection error: " << ec.message() << "\n";
            return; // Exit if there is an error
        }
        
        symbols_ = symbols; // Store the list of symbols to subscribe to
        client_.connect(con); // Initiate the WebSocket connection
        
        // Run the WebSocket client in a separate thread
        std::thread([this]() { client_.run(); }).detach();
        
        // Wait for the connection to be established
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        if (connected_) { // If the connection is established
            send_subscription(); // Send the subscription message
        }
    }

private:
    // Send the subscription message to the Bybit WebSocket server
    void send_subscription() {
        // Bybit V5 subscription format: {"op": "subscribe", "args": ["orderbook.50.BTCUSDT"]}
        json args = json::array(); // Create a JSON array for the subscription arguments
        for (const auto& symbol : symbols_) {
            args.push_back("orderbook.50." + symbol); // Add each symbol to the subscription list
        }
        
        json sub = {{"op", "subscribe"}, {"args", args}}; // Construct the subscription message
        
        // Send the subscription message over the WebSocket
        client_.send(hdl_, sub.dump(), websocketpp::frame::opcode::text);
        
        // Log the subscription
        std::cout << "[" << exchange_ << "] Subscribed to " << symbols_.size() << " symbols\n";
    }

    // Process incoming messages from the WebSocket
    void on_message(const std::string& payload) {
        try {
            auto j = json::parse(payload); // Parse the message payload as JSON
            
            // Bybit sends messages in the format: {"topic": "orderbook.50.BTCUSDT", "type": "snapshot/delta", "data": {...}}
            if (j.contains("topic") && j.contains("data")) {
                std::string topic = j["topic"]; // Extract the topic
                
                // Extract the symbol from the topic (e.g., "orderbook.50.BTCUSDT" → "BTCUSDT")
                size_t last_dot = topic.find_last_of('.');
                if (last_dot != std::string::npos) {
                    std::string symbol = topic.substr(last_dot + 1); // Extract the symbol
                    auto data = j["data"]; // Extract the data
                    
                    if (message_callback_) { // If a message callback is set
                        message_callback_(symbol, data); // Invoke the callback with the symbol and data
                    }
                }
            } else if (j.contains("op") && j["op"] == "subscribe") {
                // Log subscription confirmation messages
                std::cout << "[" << exchange_ << "] Subscription confirmed: " 
                          << j.dump() << "\n";
            }
        } catch (const std::exception& e) {
            // Log any errors during message parsing
            std::cerr << "[" << exchange_ << "] Parse error: " << e.what() << "\n";
        }
    }

    // Member variables
    client client_; // WebSocket++ client instance
    std::string exchange_; // Name of the exchange (e.g., "bybit")
    bool connected_; // Connection status
    websocketpp::connection_hdl hdl_; // WebSocket connection handle
    std::vector<std::string> symbols_; // List of symbols to subscribe to
    MessageCallback message_callback_; // Callback function for processing messages
};

} // namespace md
