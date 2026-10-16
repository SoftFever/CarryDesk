/**
 * @file tests/unit/test_rtsp.cpp
 * @brief Verify RTSP response handlers through their common dispatcher signature.
 */

// standard includes
#include <array>
#include <filesystem>
#include <string>
#include <utility>

// lib includes
#include <boost/asio.hpp>
extern "C" {
#include <moonlight-common-c/src/Rtsp.h>
}

// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"
#include "clipboard/clipboard_test_utils.h"

// local includes
#include <src/clipboard/service.h>
#include <src/config.h>
#include <src/nvhttp.h>
#include <src/rtsp.h>

/**
 * @brief Const RTSP wire headers decode network-order lengths independently of encryption flags.
 */
TEST(RtspHeaderTest, ConstAccessorsPreserveTheSerializedHeader) {
  using rtsp_stream::encrypted_rtsp_header_t;
  for (const auto length : {0U, 1U, 0x7fffffffU}) {
    for (const bool encrypted : {false, true}) {
      const auto flags = encrypted ? encrypted_rtsp_header_t::ENCRYPTED_MESSAGE_TYPE_BIT : 0U;
      const encrypted_rtsp_header_t header {util::endian::big(length | flags), 0, {}};
      EXPECT_EQ(header.payload_length(), length);
      EXPECT_EQ(header.is_encrypted(), encrypted);
      EXPECT_EQ(header.typeAndLength, util::endian::big(length | flags));
    }
  }
}

namespace rtsp_stream {
  class rtsp_server_t;

  /**
   * @brief Release a parsed RTSP message and its allocation.
   * @param message Message to release.
   */
  void free_msg(PRTSP_MESSAGE message);

  /**
   * @brief Owning RTSP message type used by the dispatcher.
   */
  using msg_t = util::safe_ptr<RTSP_MESSAGE, &free_msg>;

  /**
   * @brief Respond to OPTIONS through the common dispatcher signature.
   * @param sock Connected client socket.
   * @param session Launch session.
   * @param req Request to handle.
   */
  void cmd_option(rtsp_server_t *, boost::asio::ip::tcp::socket &sock, launch_session_t &session, msg_t &&req);

  /**
   * @brief Respond to DESCRIBE through the common dispatcher signature.
   * @param sock Connected client socket.
   * @param session Launch session.
   * @param req Request to handle.
   */
  void cmd_describe(rtsp_server_t *, boost::asio::ip::tcp::socket &sock, launch_session_t &session, msg_t &&req);

  /**
   * @brief Respond to PLAY through the common dispatcher signature.
   * @param sock Connected client socket.
   * @param session Launch session.
   * @param req Request to handle.
   */
  void cmd_play(rtsp_server_t *, boost::asio::ip::tcp::socket &sock, launch_session_t &session, msg_t &&req);
}  // namespace rtsp_stream

/**
 * @brief All three handlers respond with the request's sequence even without a server instance.
 */
TEST(RtspResponseTest, CommonDispatcherPreservesResponsesWithoutServerContext) {
  using boost::asio::ip::tcp;
  const std::array handlers {
    &rtsp_stream::cmd_option,
    &rtsp_stream::cmd_describe,
    &rtsp_stream::cmd_play,
  };

  for (const auto handler : handlers) {
    boost::asio::io_context io;
    tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
    tcp::socket client {io};
    client.connect(acceptor.local_endpoint());
    auto server_socket = acceptor.accept();

    rtsp_stream::launch_session_t session {};
    rtsp_stream::msg_t request {new RTSP_MESSAGE {}};
    request->sequenceNumber = 42;
    handler(nullptr, server_socket, session, std::move(request));
    server_socket.shutdown(tcp::socket::shutdown_send);

    std::string response;
    boost::system::error_code read_error;
    boost::asio::read(client, boost::asio::dynamic_buffer(response), read_error);
    ASSERT_EQ(read_error, boost::asio::error::eof);
    EXPECT_TRUE(response.starts_with("RTSP/1.0 200 OK\r\n"));
    EXPECT_NE(response.find("CSeq: 42\r\n"), std::string::npos);

    if (handler == &rtsp_stream::cmd_describe) {
      EXPECT_NE(response.find("a=x-ss-general.featureFlags:"), std::string::npos);
      EXPECT_NE(response.find("a=x-ss-general.encryptionSupported:"), std::string::npos);
      EXPECT_NE(response.find("a=fmtp:97 surround-params="), std::string::npos);
    } else {
      EXPECT_TRUE(response.ends_with("\r\n\r\n"));
    }
  }
}

namespace {
  /**
   * @brief Send DESCRIBE for a session and read the advertised feature flags.
   *
   * @param cert Client certificate of the launch session.
   * @return Value of `x-ss-general.featureFlags`.
   */
  std::uint32_t described_feature_flags(const std::string &cert) {
    using boost::asio::ip::tcp;
    boost::asio::io_context io;
    tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
    tcp::socket client {io};
    client.connect(acceptor.local_endpoint());
    auto server_socket = acceptor.accept();

    rtsp_stream::launch_session_t session {};
    session.client_cert = cert;
    rtsp_stream::msg_t request {new RTSP_MESSAGE {}};
    request->sequenceNumber = 1;
    rtsp_stream::cmd_describe(nullptr, server_socket, session, std::move(request));
    server_socket.shutdown(tcp::socket::shutdown_send);

    std::string response;
    boost::system::error_code read_error;
    boost::asio::read(client, boost::asio::dynamic_buffer(response), read_error);
    const std::string key = "a=x-ss-general.featureFlags:";
    const auto position = response.find(key);
    EXPECT_NE(position, std::string::npos);
    return static_cast<std::uint32_t>(std::stoul(response.substr(position + key.size())));
  }
}  // namespace

/**
 * @brief DESCRIBE advertises the clipboard only to permitted clients while the service runs.
 */
TEST(RtspClipboardFeatureTest, FlagFollowsServiceAndClientPermission) {
  const auto original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
  config::sunshine.flags[config::flag::FRESH_STATE] = true;
  nvhttp::test_support::reset_client_state();
  const auto credentials = test_utils::certificates::generate_ca_credentials("Sunshine RTSP Clipboard Client");
  const auto uuid = nvhttp::test_support::add_client("mac", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  EXPECT_FALSE(described_feature_flags(credentials.x509) & clipboard::feature_flag);

  boost::asio::io_context io;
  const auto make_service = [&](bool backend_starts) {
    auto backend = std::make_unique<clipboard_test::fake_backend_t>();
    backend->start_result = backend_starts;
    auto service = std::make_shared<clipboard::service_t>(clipboard::service_config_t {{true, true, true}, {1, 1}, std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "rtsp_clipboard_staging"}, std::move(backend), io.get_executor(), io.get_executor(), std::chrono::steady_clock::now, "linux");
    service->start();
    return service;
  };

  clipboard::set_service(make_service(true));
  EXPECT_TRUE(described_feature_flags(credentials.x509) & clipboard::feature_flag);
  EXPECT_FALSE(described_feature_flags("not a paired certificate") & clipboard::feature_flag);
  ASSERT_TRUE(nvhttp::set_client_allow_clipboard(uuid, false));
  EXPECT_FALSE(described_feature_flags(credentials.x509) & clipboard::feature_flag);
  ASSERT_TRUE(nvhttp::set_client_allow_clipboard(uuid, true));

  clipboard::set_service(make_service(false));
  EXPECT_FALSE(described_feature_flags(credentials.x509) & clipboard::feature_flag);

  clipboard::set_service(nullptr);
  nvhttp::test_support::reset_client_state();
  config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
}
