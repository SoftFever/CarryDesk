/**
 * @file src/clipboard/connection.h
 * @brief Declarations for carrying a clipboard channel over an upgraded socket.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

// lib includes
#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

// local includes
#include "channel.h"
#include "service.h"

namespace clipboard {
  /**
   * @brief A clipboard channel carried by an asynchronous stream socket.
   * @details All methods run on the socket's executor. The connection keeps itself alive while
   *          operations are pending, writes control frames ahead of DATA, ticks the channel's timers
   *          twice a second, and closes the socket once the channel has nothing more to send.
   *
   * @tparam Socket Asynchronous stream with `lowest_layer()`, such as an SSL stream over TCP.
   */
  template<class Socket>
  class socket_connection_t: public connection_t, public std::enable_shared_from_this<socket_connection_t<Socket>> {
  public:
    /**
     * @brief Wrap an upgraded socket.
     *
     * @param socket Socket taken over from the HTTP server.
     * @param id Channel id.
     * @param settings Host settings for this client.
     * @param delegate Service operations.
     * @param clock Clock for the channel's timers.
     */
    socket_connection_t(std::unique_ptr<Socket> socket, std::uint64_t id, host_settings_t settings, channel_delegate_t &delegate, clock_fn clock):
        socket_ {std::move(socket)},
        channel_ {id, std::move(settings), delegate, std::move(clock)},
        timer_ {socket_->get_executor()} {
    }

    channel_t &channel() override {
      return channel_;
    }

    /**
     * @brief Send the upgrade response, feed bytes that arrived with the request, and start I/O.
     *
     * @param response Complete `101 Switching Protocols` response head.
     * @param early_bytes Bytes the client sent after the request, already read by the HTTP server.
     */
    void start(std::string response, std::string early_bytes) {
      std::weak_ptr<socket_connection_t> weak = this->shared_from_this();
      channel_.set_output_callback([weak]() {
        if (const auto self = weak.lock()) {
          boost::asio::post(self->socket_->get_executor(), [self]() {
            self->write();
          });
        }
      });
      write_buffer_ = std::move(response);
      writing_ = true;
      boost::asio::async_write(*socket_, boost::asio::buffer(write_buffer_), [self = this->shared_from_this(), early = std::move(early_bytes)](const boost::system::error_code &ec, std::size_t) {
        self->writing_ = false;
        if (ec) {
          self->close();
          return;
        }
        if (!early.empty()) {
          self->channel_.feed(early);
        }
        self->read();
        self->tick();
        self->write();
      });
    }

    void close() override {
      const auto self = this->shared_from_this();
      channel_.close();
      shutdown();
    }

  private:
    /**
     * @brief Read the next bytes from the client.
     */
    void read() {
      if (closed_ || channel_.wants_close()) {
        return;
      }
      socket_->async_read_some(boost::asio::buffer(read_buffer_), [self = this->shared_from_this()](const boost::system::error_code &ec, const std::size_t size) {
        if (self->closed_) {
          return;
        }
        if (ec) {
          self->close();
          return;
        }
        self->channel_.feed(std::string_view {self->read_buffer_.data(), size});
        self->read();
        self->write();
      });
    }

    /**
     * @brief Write queued frames, or close the socket when the channel is done.
     */
    void write() {
      if (closed_ || writing_) {
        return;
      }
      write_buffer_ = channel_.take_output();
      if (write_buffer_.empty()) {
        if (channel_.wants_close()) {
          shutdown();
        }
        return;
      }
      writing_ = true;
      boost::asio::async_write(*socket_, boost::asio::buffer(write_buffer_), [self = this->shared_from_this()](const boost::system::error_code &ec, std::size_t) {
        self->writing_ = false;
        if (ec) {
          self->close();
          return;
        }
        self->write();
      });
    }

    /**
     * @brief Run the channel's timers periodically.
     */
    void tick() {
      if (closed_) {
        return;
      }
      timer_.expires_after(std::chrono::milliseconds {500});
      timer_.async_wait([self = this->shared_from_this()](const boost::system::error_code &ec) {
        if (ec || self->closed_) {
          return;
        }
        self->channel_.tick();
        self->write();
        self->tick();
      });
    }

    /**
     * @brief Close the TCP connection; the TLS layer's own shutdown then fails fast.
     */
    void shutdown() {
      if (closed_) {
        return;
      }
      closed_ = true;
      timer_.cancel();
      boost::system::error_code ec;
      socket_->lowest_layer().shutdown(boost::asio::socket_base::shutdown_both, ec);
      socket_->lowest_layer().close(ec);
    }

    std::unique_ptr<Socket> socket_;  ///< Upgraded socket.
    channel_t channel_;  ///< Protocol state machine.
    boost::asio::steady_timer timer_;  ///< Drives the channel's timers.
    std::array<char, 65536> read_buffer_ {};  ///< Receive buffer.
    std::string write_buffer_;  ///< Bytes being written.
    bool writing_ = false;  ///< A write is in flight.
    bool closed_ = false;  ///< The socket was closed.
  };
}  // namespace clipboard
