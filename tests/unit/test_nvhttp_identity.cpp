/**
 * @file tests/unit/test_nvhttp_identity.cpp
 * @brief Verify that HTTPS requests read the client identity of their own connection.
 */

#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// lib includes
#include <boost/asio/ssl.hpp>

// local includes
#include <src/config.h>
#include <src/nvhttp.h>

namespace {
  namespace fs = std::filesystem;
  namespace ssl = boost::asio::ssl;
  using boost::asio::ip::tcp;
  using namespace std::chrono_literals;

  /**
   * @brief Minimal TLS client that sends GET requests on one keep-alive connection.
   */
  class tls_client_t {
  public:
    /**
     * @brief Create a client presenting the given credentials.
     *
     * @param io I/O context used for blocking socket operations.
     * @param credentials Client certificate and key.
     */
    tls_client_t(boost::asio::io_context &io, const crypto::creds_t &credentials):
        context {make_context(credentials)},
        stream {io, context} {
    }

    /**
     * @brief Get the TCP socket for binding before connecting.
     *
     * @return Underlying TCP socket.
     */
    tcp::socket &socket() {
      return stream.next_layer();
    }

    /**
     * @brief Connect and complete the TLS handshake.
     *
     * @param endpoint Server endpoint.
     */
    void connect(const tcp::endpoint &endpoint) {
      if (!socket().is_open()) {
        socket().open(endpoint.protocol());
      }
      socket().connect(endpoint);
      stream.handshake(ssl::stream_base::client);
    }

    /**
     * @brief Send one GET request and read its response body.
     *
     * @param path Request path.
     * @param extra_headers Additional header lines, each ending in CRLF.
     * @return Response head and body as received.
     */
    std::string get(const std::string &path, const std::string &extra_headers = {}) {
      const auto request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n" + extra_headers + "\r\n";
      boost::asio::write(stream, boost::asio::buffer(request));

      boost::asio::read_until(stream, buffer, "\r\n\r\n");
      const std::string data {boost::asio::buffers_begin(buffer.data()), boost::asio::buffers_end(buffer.data())};
      const auto head_end = data.find("\r\n\r\n") + 4;
      const auto head = data.substr(0, head_end);
      buffer.consume(head_end);

      std::size_t content_length = 0;
      if (const auto pos = head.find("Content-Length: "); pos != std::string::npos) {
        content_length = std::stoul(head.substr(pos + 16));
      }
      if (buffer.size() < content_length) {
        boost::asio::read(stream, buffer, boost::asio::transfer_exactly(content_length - buffer.size()));
      }
      std::string body {boost::asio::buffers_begin(buffer.data()), boost::asio::buffers_begin(buffer.data()) + static_cast<std::ptrdiff_t>(content_length)};
      buffer.consume(content_length);
      return head + body;
    }

  private:
    /**
     * @brief Build a client context; SSL streams copy the certificate when they are created.
     *
     * @param credentials Client certificate and key.
     * @return Configured client context.
     */
    static ssl::context make_context(const crypto::creds_t &credentials) {
      ssl::context context {ssl::context::tls_client};
      context.use_certificate(boost::asio::buffer(credentials.x509), ssl::context::pem);
      context.use_private_key(boost::asio::buffer(credentials.pkey), ssl::context::pem);
      context.set_verify_mode(ssl::verify_none);
      return context;
    }

    ssl::context context;  ///< Client TLS context with the client's certificate.
    ssl::stream<tcp::socket> stream;  ///< TLS stream to the server.
    boost::asio::streambuf buffer;  ///< Bytes read past the previous response.
  };

  /**
   * @brief Run the production verifying HTTPS server on a background I/O thread.
   */
  class HttpsClientIdentityTest: public BaseTest {
  protected:
    /**
     * @brief Isolate paired-client state and create server credentials.
     */
    void SetUp() override {
      BaseTest::SetUp();
      original_state_file = config::nvhttp.file_state;
      original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
      config::nvhttp.file_state = (fs::path {SUNSHINE_TEST_BIN_DIR} / "https_identity_state.json").string();
      config::sunshine.flags[config::flag::FRESH_STATE] = true;
      nvhttp::test_support::reset_client_state();

      const auto credentials = crypto::gen_creds("Sunshine HTTPS identity test", 2048);
      certificate_file = fs::path {SUNSHINE_TEST_BIN_DIR} / "https_identity_cert.pem";
      private_key_file = fs::path {SUNSHINE_TEST_BIN_DIR} / "https_identity_key.pem";
      std::ofstream {certificate_file} << credentials.x509;
      std::ofstream {private_key_file} << credentials.pkey;

      alpha = test_utils::certificates::generate_ca_credentials("Sunshine Identity Alpha");
      beta = test_utils::certificates::generate_ca_credentials("Sunshine Identity Beta");
      ASSERT_FALSE(nvhttp::test_support::add_client("alpha", alpha.x509, true).empty());
      ASSERT_FALSE(nvhttp::test_support::add_client("beta", beta.x509, true).empty());
    }

    /**
     * @brief Stop the server and restore configuration.
     */
    void TearDown() override {
      if (server) {
        server->stop();
        work.reset();
        server->io_service->stop();
      }
      if (io_thread.joinable()) {
        io_thread.join();
      }
      server.reset();

      nvhttp::test_support::reset_client_state();
      std::error_code cleanup_error;
      fs::remove(certificate_file, cleanup_error);
      fs::remove(private_key_file, cleanup_error);
      config::nvhttp.file_state = original_state_file;
      config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
      BaseTest::TearDown();
    }

    /**
     * @brief Start the server with an endpoint that echoes the request's client identity.
     *
     * @param address Listening address.
     */
    void start_server(const std::string &address) {
      server = nvhttp::test_support::make_verified_https_server(certificate_file.string(), private_key_file.string());
      server->io_service = std::make_shared<boost::asio::io_context>();
      work.emplace(boost::asio::make_work_guard(*server->io_service));
      server->config.address = address;
      server->config.port = 0;
      server->resource["^/identity$"]["GET"] = [this](auto response, auto request) {
        const auto client = SimpleWeb::Server<nvhttp::SunshineHTTPS>::verified_client(*request);
        {
          std::lock_guard lock {stored_request_mutex};
          stored_request = request;
        }
        const auto remote = request->remote_endpoint();
        response->write((client ? client->name + "|" + client->cert.substr(0, 27) : std::string {"<none>"}) + "|" + remote.address().to_string() + ":" + std::to_string(remote.port()));
      };
      server->start([this](unsigned short assigned_port) {
        port = assigned_port;
      });
      server->io_service->poll();
      ASSERT_NE(port, 0);
      io_thread = std::thread([this]() {
        server->io_service->run();
      });
    }

    /**
     * @brief Extract the response body after the header.
     *
     * @param response Full response text.
     * @return Response body.
     */
    static std::string body_of(const std::string &response) {
      return response.substr(response.find("\r\n\r\n") + 4);
    }

    /**
     * @brief Extract the client name echoed by the identity endpoint.
     *
     * @param response Full response text.
     * @return Echoed client name.
     */
    static std::string name_of(const std::string &response) {
      const auto body = body_of(response);
      return body.substr(0, body.find('|'));
    }

    /**
     * @brief Extract the remote endpoint echoed by the identity endpoint.
     *
     * @param response Full response text.
     * @return Echoed remote endpoint.
     */
    static std::string remote_of(const std::string &response) {
      const auto body = body_of(response);
      return body.substr(body.rfind('|') + 1);
    }

    std::unique_ptr<SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>> server;  ///< Server under test.
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work;  ///< Keeps the I/O thread running.
    std::thread io_thread;  ///< Thread running the server's I/O context.
    unsigned short port = 0;  ///< Assigned listening port.
    crypto::creds_t alpha;  ///< First paired client.
    crypto::creds_t beta;  ///< Second paired client.

    /**
     * @brief Get the last request seen by the identity endpoint.
     *
     * @return Stored request, or null when the endpoint was never reached.
     */
    std::shared_ptr<SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>::Request> last_request() {
      std::lock_guard lock {stored_request_mutex};
      return stored_request;
    }

  private:
    std::mutex stored_request_mutex;  ///< Guards the stored request across the I/O and test threads.
    std::shared_ptr<SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>::Request> stored_request;  ///< Last request seen by the identity endpoint.
    fs::path certificate_file;  ///< Generated server certificate.
    fs::path private_key_file;  ///< Generated server private key.
    std::string original_state_file;  ///< State-file setting restored after each test.
    bool original_fresh_state = false;  ///< Fresh-state flag restored after each test.
  };

  TEST_F(HttpsClientIdentityTest, InterleavedHandshakesKeepEachConnectionsIdentity) {
    start_server("127.0.0.1");
    boost::asio::io_context client_io;
    tls_client_t first {client_io, alpha};
    tls_client_t second {client_io, beta};
    const tcp::endpoint endpoint {boost::asio::ip::address_v4::loopback(), port};

    first.connect(endpoint);
    second.connect(endpoint);

    EXPECT_EQ(name_of(first.get("/identity")), "alpha");
    EXPECT_EQ(name_of(second.get("/identity")), "beta");
    EXPECT_EQ(name_of(first.get("/identity")), "alpha");
    EXPECT_EQ(name_of(first.get("/identity")), "alpha");
    EXPECT_EQ(name_of(second.get("/identity")), "beta");
  }

  TEST_F(HttpsClientIdentityTest, SameRemoteEndpointOnTwoLocalAddressesKeepsIdentities) {
    start_server("0.0.0.0");
    boost::asio::io_context client_io;
    tls_client_t first {client_io, alpha};
    tls_client_t second {client_io, beta};

    boost::system::error_code ec;
    first.socket().open(tcp::v4());
    first.socket().set_option(boost::asio::socket_base::reuse_address(true));
    first.socket().bind({boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto shared_source = first.socket().local_endpoint();
    second.socket().open(tcp::v4());
    second.socket().set_option(boost::asio::socket_base::reuse_address(true));
    second.socket().bind(shared_source, ec);
    if (ec) {
      GTEST_SKIP() << "Cannot bind two sockets to one source endpoint: " << ec.message();
    }

    first.connect({boost::asio::ip::make_address("127.0.0.1"), port});
    try {
      second.connect({boost::asio::ip::make_address("127.0.0.2"), port});
    } catch (const boost::system::system_error &error) {
      GTEST_SKIP() << "127.0.0.2 is not reachable: " << error.what();
    }

    const auto first_response = first.get("/identity");
    const auto second_response = second.get("/identity");
    EXPECT_EQ(remote_of(first_response), remote_of(second_response));
    EXPECT_EQ(name_of(first_response), "alpha");
    EXPECT_EQ(name_of(second_response), "beta");
    EXPECT_EQ(name_of(first.get("/identity")), "alpha");
  }

  TEST_F(HttpsClientIdentityTest, DeniedClientIsAnsweredWithoutIdentity) {
    start_server("127.0.0.1");
    boost::asio::io_context client_io;
    const auto unknown = crypto::gen_creds("Sunshine Identity Unknown", 2048);
    tls_client_t client {client_io, unknown};
    client.connect({boost::asio::ip::address_v4::loopback(), port});

    const auto response = client.get("/identity");
    EXPECT_NE(response.find("status_code=\"401\""), std::string::npos);
    EXPECT_EQ(last_request(), nullptr);
  }

  TEST_F(HttpsClientIdentityTest, StoredRequestLosesIdentityWithItsConnection) {
    start_server("127.0.0.1");
    {
      boost::asio::io_context client_io;
      tls_client_t client {client_io, alpha};
      client.connect({boost::asio::ip::address_v4::loopback(), port});
      EXPECT_EQ(name_of(client.get("/identity", "Connection: close\r\n")), "alpha");
    }
    const auto request = last_request();
    ASSERT_NE(request, nullptr);

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (SimpleWeb::Server<nvhttp::SunshineHTTPS>::verified_client(*request) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(10ms);
    }
    EXPECT_FALSE(SimpleWeb::Server<nvhttp::SunshineHTTPS>::verified_client(*request));
  }

  TEST(SunshineHttpsIdentityTest, VerifiedClientIsSetOnce) {
    boost::asio::io_context io;
    ssl::context context {ssl::context::tls_server};
    nvhttp::SunshineHTTPS socket {io, context};

    EXPECT_FALSE(socket.verified_client());
    EXPECT_TRUE(socket.set_verified_client({"first-cert", "first"}));
    EXPECT_FALSE(socket.set_verified_client({"second-cert", "second"}));
    ASSERT_TRUE(socket.verified_client());
    EXPECT_EQ(socket.verified_client()->name, "first");
    EXPECT_EQ(socket.verified_client()->cert, "first-cert");
  }
}  // namespace
