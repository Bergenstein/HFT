#pragma once

// Boost 1.89 compatibility for WebSocketPP
#define BOOST_ASIO_HAS_STD_CHRONO
#define BOOST_ASIO_DISABLE_BOOST_CHRONO

// Map old Boost.Asio names to new ones
#include <boost/asio.hpp>
#include <boost/beast/ssl.hpp>

namespace boost {
namespace asio {
    // Compatibility typedef for io_service -> io_context
    // Provide a wrapper class named io_service that forwards to io_context
    class io_service : public io_context {
    public:
        using io_context::io_context;

        // Nested types expected by websocketpp
        using strand = boost::asio::strand<boost::asio::io_context::executor_type>;
        using work = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
        using steady_timer = boost::asio::steady_timer;

        template <typename Function>
        void post(Function&& f) {
            io_context::post(std::forward<Function>(f));
        }
    };

} // namespace asio
} // namespace boost

