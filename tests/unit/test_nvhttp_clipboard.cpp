/**
 * @file tests/unit/test_nvhttp_clipboard.cpp
 * @brief Test the clipboard upgrade endpoint on the HTTPS server and its client permission.
 */

#include "../certificate_test_utils.h"
#include "../tests_common.h"
#include "clipboard/clipboard_test_utils.h"

// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

// lib includes
#include <boost/asio/ssl.hpp>

// local includes
#include <src/clipboard/protocol.h>
#include <src/config.h>
#include <src/nvhttp.h>

namespace {
  namespace fs = std::filesystem;
  namespace ssl = boost::asio::ssl;
  using boost::asio::ip::tcp;
  using namespace std::chrono_literals;

  /**
   * @brief Build a client TLS context; SSL streams copy the certificate when they are created.
   *
   * @param credentials Client certificate and key.
   * @return Configured context.
   */
  ssl::context make_client_context(const crypto::creds_t &credentials) {
    ssl::context context {ssl::context::tls_client};
    context.use_certificate(boost::asio::buffer(credentials.x509), ssl::context::pem);
    context.use_private_key(boost::asio::buffer(credentials.pkey), ssl::context::pem);
    context.set_verify_mode(ssl::verify_none);
    return context;
  }

  /**
   * @brief Blocking TLS client speaking HTTP and then clipboard frames.
   */
  class upgrade_client_t {
  public:
    /**
     * @brief Connect and complete the TLS handshake.
     *
     * @param credentials Client certificate and key.
     * @param port Server port.
     */
    upgrade_client_t(const crypto::creds_t &credentials, unsigned short port):
        context {make_client_context(credentials)},
        stream {io, context} {
      stream.next_layer().connect({boost::asio::ip::address_v4::loopback(), port});
      stream.handshake(ssl::stream_base::client);
    }

    /**
     * @brief Send raw bytes.
     *
     * @param bytes Bytes to send.
     */
    void write(const std::string &bytes) {
      boost::asio::write(stream, boost::asio::buffer(bytes));
    }

    /**
     * @brief Read the response head.
     *
     * @return Status line and headers.
     */
    std::string read_head() {
      boost::asio::read_until(stream, buffer, "\r\n\r\n");
      const std::string data {boost::asio::buffers_begin(buffer.data()), boost::asio::buffers_end(buffer.data())};
      const auto end = data.find("\r\n\r\n") + 4;
      buffer.consume(end);
      return data.substr(0, end);
    }

    /**
     * @brief Read one frame.
     *
     * @return Decoded frame.
     */
    clipboard::frame_t read_frame() {
      clipboard::frame_t frame;
      while (true) {
        if (buffer.size() > 0) {
          decoder.feed({static_cast<const char *>(buffer.data().data()), buffer.size()});
          buffer.consume(buffer.size());
        }
        if (decoder.next(frame) == clipboard::frame_decoder_t::result_e::frame) {
          return frame;
        }
        boost::asio::read(stream, buffer, boost::asio::transfer_at_least(1));
      }
    }

    /**
     * @brief Wait until the server closes the connection.
     *
     * @return `true` when the connection ended.
     */
    bool closed() {
      boost::system::error_code ec;
      std::array<char, 1024> scratch {};
      while (!ec) {
        stream.read_some(boost::asio::buffer(scratch), ec);
      }
      return true;
    }

  private:
    boost::asio::io_context io;  ///< Context for blocking I/O.
    ssl::context context;  ///< Client TLS context.
    ssl::stream<tcp::socket> stream;  ///< TLS stream.
    boost::asio::streambuf buffer;  ///< Bytes read ahead.
    clipboard::frame_decoder_t decoder;  ///< Frame decoder.
  };

  /**
   * @brief Request bytes of a valid clipboard upgrade.
   */
  const std::string upgrade_request = "GET /clipboard HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: sunshine-clipboard\r\nSunshine-Clipboard-Version: 1\r\n\r\n";

  /**
   * @brief Run the production HTTPS server with the production upgrade handler and a test service.
   */
  class ClipboardUpgradeTest: public BaseTest {
  protected:
    /**
     * @brief Create credentials, a paired client and the service.
     */
    void SetUp() override {
      BaseTest::SetUp();
      original_state_file = config::nvhttp.file_state;
      original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
      config::nvhttp.file_state = (fs::path {SUNSHINE_TEST_BIN_DIR} / "clipboard_upgrade_state.json").string();
      config::sunshine.flags[config::flag::FRESH_STATE] = true;
      nvhttp::test_support::reset_client_state();

      const auto credentials = crypto::gen_creds("Sunshine clipboard upgrade test", 2048);
      certificate_file = fs::path {SUNSHINE_TEST_BIN_DIR} / "clipboard_upgrade_cert.pem";
      private_key_file = fs::path {SUNSHINE_TEST_BIN_DIR} / "clipboard_upgrade_key.pem";
      std::ofstream {certificate_file} << credentials.x509;
      std::ofstream {private_key_file} << credentials.pkey;

      client = test_utils::certificates::generate_ca_credentials("Sunshine Clipboard Client");
      client_uuid = nvhttp::test_support::add_client("mac", client.x509, true);
      ASSERT_FALSE(client_uuid.empty());

      io = std::make_shared<boost::asio::io_context>();
      auto fake = std::make_unique<clipboard_test::fake_backend_t>();
      backend = fake.get();
      service = std::make_shared<clipboard::service_t>(clipboard::service_config_t {{true, true, true}, {1048576, 1048576}, fs::path {SUNSHINE_TEST_BIN_DIR} / "clipboard_upgrade_staging"}, std::move(fake), io->get_executor(), io->get_executor(), std::chrono::steady_clock::now, "linux");
      ASSERT_TRUE(service->start());
      clipboard::set_service(service);

      server = nvhttp::test_support::make_verified_https_server(certificate_file.string(), private_key_file.string());
      server->io_service = io;
      work.emplace(boost::asio::make_work_guard(*io));
      server->config.address = "127.0.0.1";
      server->config.port = 0;
      server->on_upgrade = [](std::unique_ptr<nvhttp::SunshineHTTPS> &socket, auto request) {
        nvhttp::test_support::clipboard_upgrade(socket, request);
      };
      server->start([this](unsigned short assigned_port) {
        port = assigned_port;
      });
      io->poll();
      ASSERT_NE(port, 0);
      io_thread = std::thread([this]() {
        io->run();
      });
    }

    /**
     * @brief Stop everything and restore configuration.
     */
    void TearDown() override {
      clipboard::set_service(nullptr);
      if (service) {
        service->stop(2s);
      }
      if (server) {
        server->stop();
      }
      work.reset();
      if (io) {
        io->stop();
      }
      if (io_thread.joinable()) {
        io_thread.join();
      }
      server.reset();
      service.reset();
      nvhttp::test_support::reset_client_state();
      std::error_code cleanup_error;
      fs::remove(certificate_file, cleanup_error);
      fs::remove(private_key_file, cleanup_error);
      config::nvhttp.file_state = original_state_file;
      config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
      BaseTest::TearDown();
    }

    /**
     * @brief Send a request and read the status line.
     *
     * @param request Raw request.
     * @param credentials Client credentials.
     * @return Response head.
     */
    std::string status_of(const std::string &request, const crypto::creds_t &credentials) {
      upgrade_client_t upgrade {credentials, port};
      upgrade.write(request);
      return upgrade.read_head();
    }

    /**
     * @brief Record a running stream session for the paired client.
     */
    void start_session() {
      service->session_started(1, client.x509, []() {
        return true;
      });
    }

    std::shared_ptr<boost::asio::io_context> io;  ///< Runs the server and the channels.
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work;  ///< Keeps `io` running.
    std::thread io_thread;  ///< Runs `io`.
    std::unique_ptr<SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>> server;  ///< Server under test.
    std::shared_ptr<clipboard::service_t> service;  ///< Clipboard service.
    clipboard_test::fake_backend_t *backend = nullptr;  ///< Backend owned by the service.
    crypto::creds_t client;  ///< Paired client.
    std::string client_uuid;  ///< Paired client's UUID.
    unsigned short port = 0;  ///< Server port.
    fs::path certificate_file;  ///< Server certificate.
    fs::path private_key_file;  ///< Server key.
    std::string original_state_file;  ///< Restored state-file setting.
    bool original_fresh_state = false;  ///< Restored fresh-state flag.
  };
}  // namespace

TEST_F(ClipboardUpgradeTest, AcceptedUpgradeCarriesTheProtocol) {
  start_session();
  upgrade_client_t upgrade {client, port};
  upgrade.write(upgrade_request);
  const auto head = upgrade.read_head();
  EXPECT_TRUE(head.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
  EXPECT_NE(head.find("Upgrade: sunshine-clipboard\r\n"), std::string::npos);
  EXPECT_NE(head.find("Connection: Upgrade\r\n"), std::string::npos);
  EXPECT_NE(head.find("Sunshine-Clipboard-Version: 1\r\n"), std::string::npos);

  upgrade.write(clipboard::encode_frame(clipboard::msg_e::hello, 0, 0, clipboard::serialize(clipboard::hello_t {1, "macos", {std::string {clipboard::mime::text}}, {true, true, true}, std::nullopt, std::nullopt})));
  const auto reply = upgrade.read_frame();
  ASSERT_EQ(reply.type, static_cast<std::uint16_t>(clipboard::msg_e::hello_reply));
  const auto parsed = clipboard::parse_hello_reply(reply.payload);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->os, "linux");
  EXPECT_EQ(parsed->policy, (clipboard::policy_t {true, true, true}));
  EXPECT_EQ(service->channel_count(), 1);
}

TEST_F(ClipboardUpgradeTest, BytesSentWithTheRequestAreNotLost) {
  start_session();
  upgrade_client_t upgrade {client, port};
  upgrade.write(upgrade_request + clipboard::encode_frame(clipboard::msg_e::hello, 0, 0, R"({"version":1})"));
  EXPECT_TRUE(upgrade.read_head().starts_with("HTTP/1.1 101"));
  EXPECT_EQ(upgrade.read_frame().type, static_cast<std::uint16_t>(clipboard::msg_e::hello_reply));
}

TEST_F(ClipboardUpgradeTest, NoRunningStreamIsAConflict) {
  EXPECT_TRUE(status_of(upgrade_request, client).starts_with("HTTP/1.1 409 Conflict\r\n"));
}

TEST_F(ClipboardUpgradeTest, InvalidRequestsAreRejected) {
  start_session();
  EXPECT_TRUE(status_of("GET /other HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: sunshine-clipboard\r\nSunshine-Clipboard-Version: 1\r\n\r\n", client).starts_with("HTTP/1.1 404"));
  EXPECT_TRUE(status_of("GET /clipboard HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSunshine-Clipboard-Version: 1\r\n\r\n", client).starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(status_of("GET /clipboard HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nUpgrade: sunshine-clipboard\r\nSunshine-Clipboard-Version: 1\r\n\r\n", client).starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(status_of("GET /clipboard HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: sunshine-clipboard\r\n\r\n", client).starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(status_of("POST /clipboard HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: sunshine-clipboard\r\nSunshine-Clipboard-Version: 1\r\n\r\n", client).starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(status_of("GET /clipboard HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: sunshine-clipboard\r\nSunshine-Clipboard-Version: 1\r\nContent-Length: 2\r\n\r\nab", client).starts_with("HTTP/1.1 400"));
  EXPECT_TRUE(status_of("GET /clipboard HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: sunshine-clipboard\r\nSunshine-Clipboard-Version: 1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", client).starts_with("HTTP/1.1 400"));
  const auto version = status_of("GET /clipboard HTTP/1.1\r\nHost: x\r\nConnection: keep-alive, Upgrade\r\nUpgrade: Sunshine-Clipboard\r\nSunshine-Clipboard-Version: 2\r\n\r\n", client);
  EXPECT_TRUE(version.starts_with("HTTP/1.1 426"));
  EXPECT_NE(version.find("Sunshine-Clipboard-Version: 1\r\n"), std::string::npos);
  EXPECT_TRUE(status_of("GET /clipboard HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: sunshine-clipboard\r\nSunshine-Clipboard-Version: 1\r\nContent-Length: 0\r\n\r\n", client).starts_with("HTTP/1.1 101"));
}

TEST_F(ClipboardUpgradeTest, DisallowedClientIsForbidden) {
  start_session();
  ASSERT_TRUE(nvhttp::set_client_allow_clipboard(client_uuid, false));
  EXPECT_FALSE(nvhttp::client_allows_clipboard(client.x509));
  EXPECT_TRUE(status_of(upgrade_request, client).starts_with("HTTP/1.1 403"));
  ASSERT_TRUE(nvhttp::set_client_allow_clipboard(client_uuid, true));
  EXPECT_TRUE(nvhttp::client_allows_clipboard(client.x509));
  EXPECT_FALSE(nvhttp::set_client_allow_clipboard("unknown", true));
}

TEST_F(ClipboardUpgradeTest, WithoutAServiceTheUpgradeIsForbidden) {
  start_session();
  clipboard::set_service(nullptr);
  EXPECT_TRUE(status_of(upgrade_request, client).starts_with("HTTP/1.1 403"));
}

TEST_F(ClipboardUpgradeTest, UnpairedClientNeverReachesTheUpgrade) {
  const auto stranger = crypto::gen_creds("Sunshine Clipboard Stranger", 2048);
  const auto head = status_of(upgrade_request, stranger);
  EXPECT_FALSE(head.starts_with("HTTP/1.1 101"));
  EXPECT_EQ(service->channel_count(), 0);
}

TEST_F(ClipboardUpgradeTest, RevokingThePermissionClosesTheChannel) {
  start_session();
  upgrade_client_t upgrade {client, port};
  upgrade.write(upgrade_request);
  ASSERT_TRUE(upgrade.read_head().starts_with("HTTP/1.1 101"));
  ASSERT_TRUE(nvhttp::set_client_allow_clipboard(client_uuid, false));
  EXPECT_TRUE(upgrade.closed());
}

TEST_F(ClipboardUpgradeTest, DisablingOrUnpairingTheClientClosesTheChannel) {
  start_session();
  {
    upgrade_client_t upgrade {client, port};
    upgrade.write(upgrade_request);
    ASSERT_TRUE(upgrade.read_head().starts_with("HTTP/1.1 101"));
    ASSERT_TRUE(nvhttp::set_client_enabled(client_uuid, false));
    EXPECT_TRUE(upgrade.closed());
  }
  ASSERT_TRUE(nvhttp::set_client_enabled(client_uuid, true));
  {
    upgrade_client_t upgrade {client, port};
    upgrade.write(upgrade_request);
    ASSERT_TRUE(upgrade.read_head().starts_with("HTTP/1.1 101"));
    ASSERT_TRUE(nvhttp::unpair_client(client_uuid));
    EXPECT_TRUE(upgrade.closed());
  }
}

TEST_F(ClipboardUpgradeTest, ErasingAllClientsClosesEveryChannel) {
  start_session();
  upgrade_client_t upgrade {client, port};
  upgrade.write(upgrade_request);
  ASSERT_TRUE(upgrade.read_head().starts_with("HTTP/1.1 101"));
  nvhttp::erase_all_clients();
  EXPECT_TRUE(upgrade.closed());
}

TEST_F(ClipboardUpgradeTest, ServiceShutdownClosesUpgradedChannels) {
  start_session();
  upgrade_client_t upgrade {client, port};
  upgrade.write(upgrade_request);
  ASSERT_TRUE(upgrade.read_head().starts_with("HTTP/1.1 101"));
  service->stop(2s);
  EXPECT_TRUE(upgrade.closed());
  EXPECT_EQ(service->channel_count(), 0);
}

TEST_F(ClipboardUpgradeTest, ClientListReportsTheClipboardPermission) {
  auto clients = nvhttp::get_all_clients();
  ASSERT_EQ(clients.size(), 1);
  EXPECT_TRUE(clients[0].at("allow_clipboard").get<bool>());
  ASSERT_TRUE(nvhttp::set_client_allow_clipboard(client_uuid, false));
  clients = nvhttp::get_all_clients();
  EXPECT_FALSE(clients[0].at("allow_clipboard").get<bool>());
}
