#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string>
#include <stdexcept>

namespace http = boost::beast::http;
namespace beast = boost::beast;
namespace net   = boost::asio;
namespace ssl   = net::ssl;
using tcp = net::ip::tcp;

struct HttpClient {
  static std::string get(const std::string& host, const std::string& target, const std::string& port="443") {
    net::io_context ioc;
    ssl::context ctx{ssl::context::tls_client};
    ctx.set_verify_mode(ssl::verify_none);
    tcp::resolver resolver{ioc};
    beast::ssl_stream<tcp::socket> stream{ioc, ctx};
    auto const results = resolver.resolve(host, port);
    net::connect(stream.next_layer(), results.begin(), results.end());
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
      beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
      throw beast::system_error{ec, "SNI failed"};
    }
    stream.handshake(ssl::stream_base::client);
    http::request<http::string_body> req{http::verb::get, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "hft-coinbase/1.0");
    req.set(http::field::accept, "application/json");
    http::write(stream, req);
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    beast::error_code ec;
    stream.shutdown(ec);
    if (res.result() != http::status::ok) {
      throw std::runtime_error("HTTP " + std::to_string(static_cast<unsigned>(res.result())) + " for " + target);
    }
    return std::move(res.body());
  }
};