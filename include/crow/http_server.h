#pragma once

#ifdef CROW_USE_BOOST
#include <boost/asio.hpp>
#ifdef CROW_ENABLE_SSL
#include <boost/asio/ssl.hpp>
#endif
#else
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio.hpp>
#ifdef CROW_ENABLE_SSL
#include <asio/ssl.hpp>
#endif
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <vector>

#include "crow/version.h"
#include "crow/http_connection.h"
#include "crow/logging.h"
#include "crow/task_timer.h"


namespace crow // NOTE: Already documented in "crow/app.h"
{
#ifdef CROW_USE_BOOST
    namespace asio = boost::asio;
    using error_code = boost::system::error_code;
#else
    using error_code = asio::error_code;
#endif
    using tcp = asio::ip::tcp;

    template<typename Handler, typename Adaptor = SocketAdaptor, typename... Middlewares>
    class Server
    {
    public:
        Server(Handler* handler,
               const tcp::endpoint& endpoint,
               std::string server_name = std::string("Crow/") + VERSION,
               std::tuple<Middlewares...>* middlewares = nullptr,
               uint16_t concurrency = 1,
               uint8_t timeout = 5,
               typename Adaptor::context* adaptor_ctx = nullptr):
          io_context_(concurrency),
          acceptor_(io_context_, endpoint),
          signals_(io_context_),
          tick_timer_(io_context_.get_executor()),
          handler_(handler),
          concurrency_(concurrency),
          timeout_(timeout),
          server_name_(server_name),
          middlewares_(middlewares),
          adaptor_ctx_(adaptor_ctx)
        {}

        void set_tick_function(std::chrono::milliseconds d, std::function<void()> f)
        {
            tick_interval_ = d;
            tick_function_ = f;
        }

        asio::awaitable<void> on_tick()
        {
            asio::error_code ec;
            asio::steady_timer tick_timer(co_await asio::this_coro::executor);
            for (;;)
            {
                tick_timer.expires_after(tick_interval_);
                co_await tick_timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                if (ec)
                    co_return;

                tick_function_();
            }
        }

        void run()
        {

            if (tick_function_ && tick_interval_.count() > 0)
            {
                asio::co_spawn(io_context_, on_tick(), asio::detached);
            }

            handler_->port(acceptor_.local_endpoint().port());


            CROW_LOG_INFO << server_name_
                          << " server is running at " << (handler_->ssl_used() ? "https://" : "http://")
                          << acceptor_.local_endpoint().address() << ":" << acceptor_.local_endpoint().port() << " using " << concurrency_ << " threads";
            CROW_LOG_INFO << "Call `app.loglevel(crow::LogLevel::Warning)` to hide Info level logs.";

            signals_.async_wait(
              [&](const error_code& /*error*/, int /*signal_number*/) {
                  stop();
              });

            asio::co_spawn(io_context_, do_accept(), asio::detached);
            notify_start();
            io_context_.wait();
            CROW_LOG_INFO << "Exiting.";
        }

        void stop()
        {
            shutting_down_ = true; // Prevent the acceptor from taking new connections
            CROW_LOG_INFO << "Closing main IO service (" << &io_context_ << ')';
            io_context_.stop(); // Close main io_service
        }

        uint16_t port() const
        {
            return acceptor_.local_endpoint().port();
        }

        /// Wait until the server has properly started or until timeout
        std::cv_status wait_for_start(std::chrono::steady_clock::time_point wait_until)
        {
            std::unique_lock<std::mutex> lock(start_mutex_);

            std::cv_status status = std::cv_status::no_timeout;
            while (!server_started_ && (status == std::cv_status::no_timeout))
                status = cv_started_.wait_until(lock, wait_until);
            return status;
        }

        void signal_clear()
        {
            signals_.clear();
        }

        void signal_add(int signal_number)
        {
            signals_.add(signal_number);
        }

    private:
        asio::awaitable<void> do_accept()
        {
            while (!shutting_down_)
            {
                // initializing task timers
                auto task_timer = std::make_shared<detail::task_timer>(io_context_.get_executor());
                task_timer->set_default_timeout(timeout_);

                auto p = std::make_shared<Connection<Adaptor, Handler, Middlewares...>>(
                  io_context_.get_executor(), handler_, server_name_, middlewares_, task_timer, adaptor_ctx_);

                asio::error_code ec;
                co_await acceptor_.async_accept(p->socket(), asio::redirect_error(asio::use_awaitable, ec));
                if (ec)
                    co_return;

                p->start();
            }
        }

        /// Notify anything using `wait_for_start()` to proceed
        void notify_start()
        {
            std::unique_lock<std::mutex> lock(start_mutex_);
            server_started_ = true;
            cv_started_.notify_all();
        }

    private:
        asio::thread_pool io_context_;
        tcp::acceptor acceptor_;
        bool shutting_down_ = false;
        bool server_started_{false};
        std::condition_variable cv_started_;
        std::mutex start_mutex_;
        asio::signal_set signals_;

        asio::steady_timer tick_timer_;

        Handler* handler_;
        uint16_t concurrency_{2};
        std::uint8_t timeout_;
        std::string server_name_;

        std::chrono::milliseconds tick_interval_;
        std::function<void()> tick_function_;

        std::tuple<Middlewares...>* middlewares_;

        typename Adaptor::context* adaptor_ctx_;
    };
} // namespace crow
