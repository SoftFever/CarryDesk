/**
 * @file tests/unit/clipboard/test_clipboard_service.cpp
 * @brief Test src/clipboard/service.*
 */

// test includes
#include "../../tests_common.h"
#include "clipboard_test_utils.h"

// standard includes
#include <filesystem>
#include <fstream>
#include <thread>

// lib includes
#include <boost/asio/io_context.hpp>

// local includes
#include <src/clipboard/file_list.h>

using namespace clipboard;
using namespace std::chrono_literals;

namespace {
  constexpr std::uint64_t mib = 1048576;
  const std::string text_mime {mime::text};
  const std::string html_mime {mime::html};
  const std::string png_mime {mime::png};

  /**
   * @brief Connection whose transport is driven directly by the test.
   */
  struct memory_connection_t: connection_t {
    /**
     * @brief Create the connection's channel.
     *
     * @param id Channel id.
     * @param settings Host settings.
     * @param delegate Service.
     * @param clock Clock.
     */
    memory_connection_t(std::uint64_t id, host_settings_t settings, channel_delegate_t &delegate, clock_fn clock):
        endpoint {id, std::move(settings), delegate, std::move(clock)} {
    }

    channel_t &channel() override {
      return endpoint;
    }

    void close() override {
      ++closes;
      endpoint.close();
    }

    /**
     * @brief Feed one client frame.
     *
     * @param type Message type.
     * @param payload Payload.
     * @param stream Stream id.
     * @param flags Frame flags.
     */
    void send(msg_e type, const std::string &payload, std::uint32_t stream = 0, std::uint16_t flags = 0) {
      endpoint.feed(encode_frame(type, flags, stream, payload));
    }

    /**
     * @brief Take the frames the host sent.
     *
     * @return Decoded frames.
     */
    std::vector<frame_t> frames() {
      decoder.feed(endpoint.take_output(std::numeric_limits<std::size_t>::max()));
      std::vector<frame_t> result;
      frame_t frame;
      while (decoder.next(frame) == frame_decoder_t::result_e::frame) {
        result.push_back(frame);
      }
      return result;
    }

    /**
     * @brief Complete the handshake.
     *
     * @param policy Client policy.
     * @return Frames sent in answer.
     */
    std::vector<frame_t> hello(const policy_t &policy = {true, true, true}) {
      send(msg_e::hello, serialize(hello_t {1, "macos", {text_mime, html_mime, std::string {mime::rtf}, png_mime, std::string {mime::files}}, policy, std::nullopt, std::nullopt}));
      return frames();
    }

    channel_t endpoint;  ///< Channel under test.
    frame_decoder_t decoder;  ///< Decoder of the host's output.
    int closes = 0;  ///< Calls of `close`.
  };

  /**
   * @brief Find frames of one type.
   *
   * @param frames Frames to filter.
   * @param type Wanted type.
   * @return Matching frames.
   */
  std::vector<frame_t> of_type(const std::vector<frame_t> &frames, msg_e type) {
    std::vector<frame_t> result;
    for (const auto &frame : frames) {
      if (frame.type == static_cast<std::uint16_t>(type)) {
        result.push_back(frame);
      }
    }
    return result;
  }

  /**
   * @brief Run a service against a fake backend on one deterministic I/O context.
   */
  class ClipboardServiceTest: public testing::Test {
  protected:
    /**
     * @brief Create and start the service.
     */
    void SetUp() override {
      scratch = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "clipboard_service";
      std::filesystem::remove_all(scratch);
      std::filesystem::create_directories(scratch);
      config.staging_area = scratch / "staging";
      make_service();
    }

    /**
     * @brief Stop the service and remove scratch files.
     */
    void TearDown() override {
      if (service) {
        run();
        service.reset();
      }
      std::error_code ec;
      std::filesystem::remove_all(scratch, ec);
    }

    /**
     * @brief Replace the service.
     *
     * @param start Whether to start it.
     */
    void make_service(bool start = true) {
      auto fake = std::make_unique<clipboard_test::fake_backend_t>();
      backend = fake.get();
      if (backend_files) {
        backend->supported.emplace_back(mime::files);
      }
      service = std::make_shared<service_t>(config, std::move(fake), io.get_executor(), io.get_executor(), [this]() {
        return now;
      },
                                            "linux");
      if (start) {
        ASSERT_TRUE(service->start());
      }
    }

    /**
     * @brief Run every ready handler.
     */
    void run() {
      io.restart();
      while (io.poll() > 0) {
      }
    }

    /**
     * @brief Admit a client.
     *
     * @param cert Client certificate.
     * @param allowed Per-client permission.
     * @return Admission outcome and connection.
     */
    std::pair<admission_e, std::shared_ptr<memory_connection_t>> admit(const std::string &cert, bool allowed = true) {
      auto admission = service->admit(cert, allowed, [this](std::uint64_t id, host_settings_t settings, channel_delegate_t &delegate) {
        return std::make_shared<memory_connection_t>(id, std::move(settings), delegate, [this]() {
          return now;
        });
      });
      return {admission.result, std::static_pointer_cast<memory_connection_t>(admission.connection)};
    }

    /**
     * @brief Record a running session and admit an open channel for it.
     *
     * @param cert Client certificate.
     * @param session Session id.
     * @return Open connection.
     */
    std::shared_ptr<memory_connection_t> open_client(const std::string &cert, std::uint64_t session) {
      service->session_started(session, cert, []() {
        return true;
      });
      auto [result, connection] = admit(cert);
      EXPECT_EQ(result, admission_e::admitted);
      connection->hello();
      return connection;
    }

    /**
     * @brief Answer every GET the host sent with bytes from a map, ending each response.
     *
     * @param client Client connection.
     * @param frames Frames the host sent.
     * @param bodies Body per mime.
     */
    static void answer_gets(memory_connection_t &client, const std::vector<frame_t> &frames, const std::map<std::string, std::string> &bodies) {
      for (const auto &frame : of_type(frames, msg_e::get)) {
        const auto get = parse_get(frame.payload);
        client.send(msg_e::data, bodies.at(get->mime), frame.stream_id, flag_end);
      }
    }

    /**
     * @brief Get the status of the last ANNOUNCE_ACK in a frame list.
     *
     * @param frames Frames the host sent.
     * @return ACK status, or empty when there is no ACK.
     */
    static std::string last_ack(const std::vector<frame_t> &frames) {
      const auto acks = of_type(frames, msg_e::announce_ack);
      return acks.empty() ? std::string {} : parse_announce_ack(acks.back().payload)->status;
    }

    std::filesystem::path scratch;  ///< Scratch directory with the staging area.
    boost::asio::io_context io;  ///< Runs channels and worker jobs.
    time_point now = time_point {} + 1h;  ///< Fake clock.
    service_config_t config {{true, true, true}, {64 * mib, 4096 * mib}};  ///< Service settings.
    clipboard_test::fake_backend_t *backend = nullptr;  ///< Backend owned by the service.
    bool backend_files = false;  ///< New backends also support files.
    std::shared_ptr<service_t> service;  ///< Service under test.
  };
}  // namespace

TEST_F(ClipboardServiceTest, StartNeedsSyncEnabledAndAWorkingBackend) {
  EXPECT_TRUE(service->available());
  EXPECT_TRUE(service->feature_enabled(true));
  EXPECT_FALSE(service->feature_enabled(false));

  config.policy = {false, false, true};
  make_service(false);
  EXPECT_FALSE(service->start());
  EXPECT_FALSE(service->available());

  config.policy = {true, false, false};
  make_service(false);
  backend->start_result = false;
  EXPECT_FALSE(service->start());
  EXPECT_FALSE(service->feature_enabled(true));

  service = std::make_shared<service_t>(config, nullptr, io.get_executor(), io.get_executor(), [this]() {
    return now;
  },
                                        "linux");
  EXPECT_FALSE(service->start());
  EXPECT_EQ(service->admit("cert", true, nullptr).result, admission_e::disabled);
}

TEST_F(ClipboardServiceTest, AdmissionNeedsPermissionAndARunningSession) {
  EXPECT_EQ(admit("a").first, admission_e::no_session);
  service->session_started(1, "a", []() {
    return true;
  });
  EXPECT_EQ(service->session_count("a"), 1);
  EXPECT_EQ(admit("a", false).first, admission_e::not_permitted);
  const auto [result, connection] = admit("a");
  EXPECT_EQ(result, admission_e::admitted);
  ASSERT_TRUE(connection);
  EXPECT_EQ(service->channel_count(), 1);
}

TEST_F(ClipboardServiceTest, SessionStoppedBeforeItStartedIsIgnored) {
  service->session_stopped(5);
  service->session_started(5, "a", []() {
    return true;
  });
  EXPECT_EQ(service->session_count("a"), 0);
  EXPECT_EQ(admit("a").first, admission_e::no_session);

  service->session_started(6, "a", []() {
    return false;
  });
  EXPECT_EQ(service->session_count("a"), 0);

  for (std::uint64_t session = 100; session < 200; ++session) {
    service->session_stopped(session);
  }
  service->session_started(5, "a", []() {
    return true;
  });
  EXPECT_EQ(service->session_count("a"), 1);
}

TEST_F(ClipboardServiceTest, ChannelClosesWhenTheClientsLastSessionStops) {
  service->session_started(1, "a", []() {
    return true;
  });
  service->session_started(2, "a", []() {
    return true;
  });
  const auto client = admit("a").second;
  client->hello();
  service->session_stopped(1);
  run();
  EXPECT_EQ(client->closes, 0);
  service->session_stopped(2);
  run();
  EXPECT_EQ(client->closes, 1);
  EXPECT_EQ(service->channel_count(), 0);
  EXPECT_EQ(admit("a").first, admission_e::no_session);
}

TEST_F(ClipboardServiceTest, NewUpgradeReplacesTheCertificatesChannel) {
  const auto first = open_client("a", 1);
  const auto [result, second] = admit("a");
  EXPECT_EQ(result, admission_e::admitted);
  EXPECT_EQ(service->channel_count(), 1);
  run();
  EXPECT_EQ(first->closes, 1);
  EXPECT_EQ(second->closes, 0);
  EXPECT_EQ(service->channel_count(), 1);
}

TEST_F(ClipboardServiceTest, AtMostEightChannels) {
  for (int i = 0; i < 8; ++i) {
    open_client("cert" + std::to_string(i), i + 1);
  }
  service->session_started(50, "ninth", []() {
    return true;
  });
  EXPECT_EQ(admit("ninth").first, admission_e::too_many);
  EXPECT_EQ(admit("cert3").first, admission_e::admitted);
}

TEST_F(ClipboardServiceTest, RevokeClosesChannels) {
  const auto a = open_client("a", 1);
  const auto b = open_client("b", 2);
  service->revoke("unknown");
  service->revoke("a");
  run();
  EXPECT_EQ(a->closes, 1);
  EXPECT_EQ(b->closes, 0);
  service->revoke_all();
  run();
  EXPECT_EQ(b->closes, 1);
  EXPECT_EQ(service->channel_count(), 0);
}

TEST_F(ClipboardServiceTest, HostChangesAreAnnouncedAndServed) {
  const auto client = open_client("a", 1);
  backend->change({text_mime, "image/jpeg"}, {{text_mime, "hello"}});
  run();
  const auto announces = of_type(client->frames(), msg_e::announce);
  ASSERT_EQ(announces.size(), 1);
  const auto announce = parse_announce(announces[0].payload);
  EXPECT_EQ(announce->formats, (std::vector<format_t> {{text_mime, std::nullopt}}));

  client->send(msg_e::get, serialize(get_t {announce->generation, text_mime}), 1);
  run();
  const auto data = of_type(client->frames(), msg_e::data);
  ASSERT_EQ(data.size(), 1);
  EXPECT_EQ(data[0].payload, "hello");
  EXPECT_TRUE(data[0].end());
}

TEST_F(ClipboardServiceTest, HostClipboardIsAnnouncedAtHello) {
  backend->change({html_mime}, {{html_mime, "<b>x</b>"}});
  run();
  service->session_started(1, "a", []() {
    return true;
  });
  const auto client = admit("a").second;
  const auto frames = client->hello();
  ASSERT_EQ(frames.size(), 2);
  EXPECT_TRUE(parse_hello_reply(frames[0].payload)->host_clipboard);
  EXPECT_EQ(frames[1].type, static_cast<std::uint16_t>(msg_e::announce));
}

TEST_F(ClipboardServiceTest, BackendReadErrorsReachTheClient) {
  const auto client = open_client("a", 1);
  backend->change({text_mime, png_mime}, {{png_mime, std::string(100, 'p')}});
  run();
  const auto generation = parse_announce(of_type(client->frames(), msg_e::announce).at(0).payload)->generation;
  client->send(msg_e::get, serialize(get_t {generation, text_mime}), 1);
  run();
  ++backend->revision;
  client->send(msg_e::get, serialize(get_t {generation, png_mime}), 3);
  run();
  const auto errors = of_type(client->frames(), msg_e::error);
  ASSERT_EQ(errors.size(), 2);
  EXPECT_EQ(parse_error(errors[0].payload)->code, "io_error");
  EXPECT_EQ(parse_error(errors[1].payload)->code, "stale_generation");
}

TEST_F(ClipboardServiceTest, OversizedHostItemIsRefused) {
  config.limits.max_item_size = 10;
  make_service();
  const auto client = open_client("a", 1);
  backend->change({png_mime}, {{png_mime, std::string(11, 'p')}});
  run();
  const auto generation = parse_announce(of_type(client->frames(), msg_e::announce).at(0).payload)->generation;
  client->send(msg_e::get, serialize(get_t {generation, png_mime}), 1);
  run();
  EXPECT_EQ(parse_error(of_type(client->frames(), msg_e::error).at(0).payload)->code, "too_large");
}

TEST_F(ClipboardServiceTest, ClientAnnouncementIsPulledInstalledAndAcknowledged) {
  const auto client = open_client("a", 1);
  const auto other = open_client("b", 2);
  backend->change({text_mime}, {{text_mime, "host"}});
  run();
  other->frames();
  client->frames();

  client->send(msg_e::announce, serialize(announce_t {1, {{text_mime, std::nullopt}, {html_mime, std::nullopt}}}));
  run();
  const auto requests = client->frames();
  EXPECT_EQ(of_type(requests, msg_e::get).size(), 2);
  answer_gets(*client, requests, {{text_mime, "client text"}, {html_mime, "<i>client</i>"}});
  run();

  ASSERT_EQ(backend->installs.size(), 1);
  EXPECT_EQ(backend->installs[0].at(text_mime), "client text");
  EXPECT_EQ(backend->installs[0].at(html_mime), "<i>client</i>");
  EXPECT_EQ(backend->ownership_revisions, std::vector<std::uint64_t> {1});
  EXPECT_EQ(last_ack(client->frames()), "installed");
  EXPECT_EQ(service->owner(), client->endpoint.id());

  const auto other_frames = other->frames();
  ASSERT_EQ(of_type(other_frames, msg_e::clear).size(), 1);
  EXPECT_FALSE(service->current_host_clipboard(other->endpoint));

  other->send(msg_e::get, serialize(get_t {1, text_mime}), 1);
  EXPECT_EQ(parse_error(of_type(other->frames(), msg_e::error).at(0).payload)->code, "stale_generation");
}

TEST_F(ClipboardServiceTest, EmptyAnnouncementInstallsAnEmptyClipboard) {
  const auto client = open_client("a", 1);
  client->send(msg_e::announce, serialize(announce_t {1, {}}));
  run();
  ASSERT_EQ(backend->installs.size(), 1);
  EXPECT_TRUE(backend->installs[0].empty());
  EXPECT_EQ(last_ack(client->frames()), "installed");
}

TEST_F(ClipboardServiceTest, OversizedClientFormatsAreDropped) {
  config.limits.max_item_size = 10;
  make_service();
  const auto client = open_client("a", 1);
  client->send(msg_e::announce, serialize(announce_t {1, {{text_mime, std::nullopt}, {png_mime, std::nullopt}}}));
  run();
  answer_gets(*client, client->frames(), {{text_mime, "short"}, {png_mime, std::string(11, 'p')}});
  run();
  ASSERT_EQ(backend->installs.size(), 1);
  EXPECT_EQ(backend->installs[0].size(), 1);
  EXPECT_EQ(backend->installs[0].at(text_mime), "short");
  EXPECT_EQ(last_ack(client->frames()), "installed");

  client->send(msg_e::announce, serialize(announce_t {2, {{png_mime, std::nullopt}}}));
  run();
  answer_gets(*client, client->frames(), {{png_mime, std::string(11, 'p')}});
  run();
  EXPECT_EQ(backend->installs.size(), 1);
  EXPECT_EQ(last_ack(client->frames()), "too_large");
}

TEST_F(ClipboardServiceTest, PullFailuresAreAcknowledgedWithTheirStatus) {
  const auto client = open_client("a", 1);
  const std::vector<std::pair<std::string, std::string>> cases {
    {"stale_generation", "stale_generation"},
    {"not_permitted", "not_permitted"},
    {"unsupported_format", "io_error"},
  };
  std::uint64_t generation = 0;
  for (const auto &[code, status] : cases) {
    client->send(msg_e::announce, serialize(announce_t {++generation, {{text_mime, std::nullopt}}}));
    run();
    const auto get = of_type(client->frames(), msg_e::get).at(0);
    client->send(msg_e::error, serialize(error_msg_t {code, "x"}), get.stream_id, flag_end);
    run();
    EXPECT_EQ(last_ack(client->frames()), status) << code;
  }
  EXPECT_TRUE(backend->installs.empty());
}

TEST_F(ClipboardServiceTest, HostChangeDuringAPullWins) {
  const auto client = open_client("a", 1);
  client->send(msg_e::announce, serialize(announce_t {1, {{text_mime, std::nullopt}}}));
  run();
  const auto get = of_type(client->frames(), msg_e::get).at(0);
  backend->change({}, {});
  run();
  const auto frames = client->frames();
  EXPECT_EQ(last_ack(frames), "stale_generation");
  ASSERT_EQ(of_type(frames, msg_e::cancel).size(), 1);
  EXPECT_EQ(of_type(frames, msg_e::cancel)[0].stream_id, get.stream_id);
  client->send(msg_e::data, "late", get.stream_id, flag_end);
  run();
  EXPECT_TRUE(backend->installs.empty());
}

TEST_F(ClipboardServiceTest, LaterAnnouncementWinsAcrossChannels) {
  const auto first = open_client("a", 1);
  const auto second = open_client("b", 2);
  first->send(msg_e::announce, serialize(announce_t {1, {{text_mime, std::nullopt}}}));
  run();
  const auto first_requests = first->frames();
  second->send(msg_e::announce, serialize(announce_t {1, {{text_mime, std::nullopt}}}));
  run();
  answer_gets(*second, second->frames(), {{text_mime, "second"}});
  run();
  answer_gets(*first, first_requests, {{text_mime, "first"}});
  run();
  ASSERT_EQ(backend->installs.size(), 1);
  EXPECT_EQ(backend->installs[0].at(text_mime), "second");
  EXPECT_EQ(last_ack(second->frames()), "installed");
  const auto first_frames = first->frames();
  EXPECT_EQ(last_ack(first_frames), "stale_generation");
  EXPECT_EQ(service->owner(), second->endpoint.id());
}

TEST_F(ClipboardServiceTest, NewerAnnouncementFromTheSameClientCancelsTheOlderPull) {
  const auto client = open_client("a", 1);
  client->send(msg_e::announce, serialize(announce_t {1, {{text_mime, std::nullopt}}}));
  run();
  const auto old_get = of_type(client->frames(), msg_e::get).at(0);
  client->send(msg_e::announce, serialize(announce_t {2, {{text_mime, std::nullopt}}}));
  run();
  const auto frames = client->frames();
  EXPECT_EQ(last_ack(frames), "stale_generation");
  EXPECT_EQ(of_type(frames, msg_e::cancel).at(0).stream_id, old_get.stream_id);
  answer_gets(*client, frames, {{text_mime, "newer"}});
  run();
  ASSERT_EQ(backend->installs.size(), 1);
  EXPECT_EQ(backend->installs[0].at(text_mime), "newer");
}

TEST_F(ClipboardServiceTest, ClientClearCancelsThePull) {
  const auto client = open_client("a", 1);
  client->send(msg_e::announce, serialize(announce_t {1, {{text_mime, std::nullopt}}}));
  run();
  const auto get = of_type(client->frames(), msg_e::get).at(0);
  client->send(msg_e::clear, serialize_single("generation", 1));
  run();
  const auto frames = client->frames();
  EXPECT_EQ(last_ack(frames), "stale_generation");
  EXPECT_EQ(of_type(frames, msg_e::cancel).at(0).stream_id, get.stream_id);
  EXPECT_TRUE(backend->installs.empty());
}

TEST_F(ClipboardServiceTest, BackendRefusalIsAcknowledged) {
  const auto client = open_client("a", 1);
  backend->ownership_result = platf::clipboard_result_e::io_error;
  client->send(msg_e::announce, serialize(announce_t {1, {}}));
  run();
  EXPECT_EQ(last_ack(client->frames()), "io_error");
  EXPECT_FALSE(service->owner());

  backend->ownership_result = platf::clipboard_result_e::ok;
  backend->revision = 7;
  client->send(msg_e::announce, serialize(announce_t {2, {}}));
  run();
  EXPECT_EQ(last_ack(client->frames()), "stale_generation");
}

TEST_F(ClipboardServiceTest, OwnerCloseReleasesTheHostClipboard) {
  const auto client = open_client("a", 1);
  client->send(msg_e::announce, serialize(announce_t {1, {}}));
  run();
  ASSERT_EQ(service->owner(), client->endpoint.id());
  client->close();
  run();
  EXPECT_EQ(backend->releases, 1);
  EXPECT_FALSE(service->owner());
  EXPECT_EQ(service->channel_count(), 0);
}

TEST_F(ClipboardServiceTest, CommitFinishingAfterTheChannelClosedIsReleased) {
  const auto client = open_client("a", 1);
  client->send(msg_e::announce, serialize(announce_t {1, {}}));
  client->close();
  run();
  EXPECT_EQ(backend->installs.size(), 1);
  EXPECT_EQ(backend->releases, 1);
  EXPECT_FALSE(service->owner());
}

TEST_F(ClipboardServiceTest, HostCopyRacingACommitKeepsTheHostClipboard) {
  const auto client = open_client("a", 1);
  backend->after_ownership = [this]() {
    backend->after_ownership = nullptr;
    backend->change({text_mime}, {{text_mime, "copied on the host"}});
  };
  client->send(msg_e::announce, serialize(announce_t {1, {}}));
  run();
  EXPECT_EQ(last_ack(client->frames()), "stale_generation");
  EXPECT_FALSE(service->owner());
  const auto host = service->current_host_clipboard(client->endpoint);
  ASSERT_TRUE(host);
  EXPECT_EQ(host->formats, std::vector<std::string> {text_mime});
  EXPECT_EQ(backend->releases, 1);

  service->session_started(2, "b", []() {
    return true;
  });
  const auto later = admit("b").second;
  const auto frames = later->hello();
  EXPECT_TRUE(parse_hello_reply(frames.at(0).payload)->host_clipboard);
}

TEST_F(ClipboardServiceTest, ClearFromTheOwnerEmptiesTheHostClipboard) {
  const auto client = open_client("a", 1);
  client->send(msg_e::announce, serialize(announce_t {1, {}}));
  run();
  ASSERT_EQ(service->owner(), client->endpoint.id());
  const auto other = open_client("b", 2);
  other->send(msg_e::clear, serialize_single("generation", 0));
  run();
  EXPECT_EQ(service->owner(), client->endpoint.id());
  client->send(msg_e::clear, serialize_single("generation", 1));
  run();
  EXPECT_FALSE(service->owner());
  EXPECT_EQ(backend->releases, 1);
}

TEST_F(ClipboardServiceTest, ForeignChangeAfterInstallMakesTheHostOwner) {
  const auto client = open_client("a", 1);
  client->send(msg_e::announce, serialize(announce_t {1, {}}));
  run();
  client->frames();
  backend->change({text_mime}, {{text_mime, "host again"}});
  run();
  EXPECT_FALSE(service->owner());
  EXPECT_EQ(of_type(client->frames(), msg_e::announce).size(), 1);
}

TEST_F(ClipboardServiceTest, StopClosesChannelsAndStopsTheBackend) {
  const auto client = open_client("a", 1);
  std::jthread io_thread {[this]() {
    auto work = boost::asio::make_work_guard(io);
    io.restart();
    io.run_for(2s);
  }};
  service->stop(2s);
  io.stop();
  io_thread.join();
  EXPECT_EQ(client->closes, 1);
  EXPECT_EQ(backend->stops, 1);
  EXPECT_FALSE(service->available());
  EXPECT_EQ(admit("a").first, admission_e::disabled);
  service->stop(0ms);
  EXPECT_EQ(backend->stops, 1);
}

TEST_F(ClipboardServiceTest, StopGivesUpWaitingForChannels) {
  open_client("a", 1);
  service->stop(10ms);
  EXPECT_EQ(backend->stops, 1);
  run();
  EXPECT_EQ(service->channel_count(), 0);
}

TEST_F(ClipboardServiceTest, ProcessWideServiceCanBeSetAndCleared) {
  set_service(service);
  EXPECT_EQ(get_service(), service);
  set_service(nullptr);
  EXPECT_EQ(get_service(), nullptr);
}

namespace {
  /**
   * @brief Write a file with content.
   *
   * @param path File path.
   * @param content Content.
   */
  void write_file(const std::filesystem::path &path, const std::string &content) {
    std::ofstream {path, std::ios::binary} << content;
  }

  /**
   * @brief Read a whole file.
   *
   * @param path File path.
   * @return Content.
   */
  std::string read_file(const std::filesystem::path &path) {
    std::ifstream stream {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {stream}, {}};
  }

  /**
   * @brief Concatenate the DATA payloads of one stream.
   *
   * @param frames Frames the host sent.
   * @param stream Stream id.
   * @return Payload bytes and whether END was seen.
   */
  std::pair<std::string, bool> body_of(const std::vector<frame_t> &frames, std::uint32_t stream) {
    std::string body;
    bool ended = false;
    for (const auto &frame : frames) {
      if (frame.stream_id == stream && frame.type == static_cast<std::uint16_t>(msg_e::data)) {
        body += frame.payload;
        ended = ended || frame.end();
      }
    }
    return {body, ended};
  }

  /**
   * @brief Count the staged roots in an area.
   *
   * @param area Staging area.
   * @return Directories in the area.
   */
  std::size_t staged_roots(const std::filesystem::path &area) {
    std::size_t count = 0;
    if (std::filesystem::exists(area)) {
      for (const auto &entry : std::filesystem::directory_iterator {area}) {
        count += entry.is_directory() ? 1 : 0;
      }
    }
    return count;
  }
}  // namespace

#ifndef _WIN32
TEST_F(ClipboardServiceTest, HostFilesAreServedThroughALease) {
  backend_files = true;
  make_service();
  std::filesystem::create_directories(scratch / "host" / "tree");
  write_file(scratch / "host" / "tree" / "a.txt", "hello");
  const std::string big(100 * 1024, 'b');
  write_file(scratch / "host" / "b.bin", big);
  backend->paths = {(scratch / "host" / "tree").string(), (scratch / "host" / "b.bin").string()};

  const auto client = open_client("a", 1);
  backend->change({std::string {mime::files}}, {});
  run();
  const auto generation = parse_announce(of_type(client->frames(), msg_e::announce).at(0).payload)->generation;

  client->send(msg_e::lock, serialize_single("generation", generation), 1);
  run();
  EXPECT_EQ(backend->files_max_size, limit::max_manifest_bytes);
  const auto reply = parse_lock_reply(of_type(client->frames(), msg_e::lock_reply).at(0).payload);
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->count, 3);
  EXPECT_EQ(reply->total_bytes, 5 + big.size());

  client->send(msg_e::get_file_list, serialize_single("lease", reply->lease), 3);
  run();
  const auto [manifest, listed] = body_of(client->frames(), 3);
  EXPECT_TRUE(listed);
  const auto entries = files::parse_manifest(manifest);
  ASSERT_TRUE(entries);
  ASSERT_EQ(entries->size(), 3);
  EXPECT_EQ((*entries)[2].path, "b.bin");

  client->send(msg_e::get_file, serialize(get_file_t {reply->lease, 2, 0, big.size()}), 5);
  run();
  const auto [content, ended] = body_of(client->frames(), 5);
  EXPECT_TRUE(ended);
  EXPECT_EQ(content, big);

  client->send(msg_e::get_file, serialize(get_file_t {reply->lease, 1, 1, 3}), 7);
  run();
  EXPECT_EQ(body_of(client->frames(), 7).first, "ell");

  write_file(scratch / "host" / "replacement", "other");
  std::filesystem::rename(scratch / "host" / "replacement", scratch / "host" / "tree" / "a.txt");
  client->send(msg_e::get_file, serialize(get_file_t {reply->lease, 1, 0, 5}), 9);
  run();
  EXPECT_EQ(parse_error(of_type(client->frames(), msg_e::error).at(0).payload)->code, "io_error");
}

TEST_F(ClipboardServiceTest, HostFilesBeyondTheLimitAreRefused) {
  backend_files = true;
  config.limits.max_transfer_size = 4;
  make_service();
  write_file(scratch / "five.txt", "12345");
  backend->paths = {(scratch / "five.txt").string()};
  const auto client = open_client("a", 1);
  backend->change({std::string {mime::files}}, {});
  run();
  const auto generation = parse_announce(of_type(client->frames(), msg_e::announce).at(0).payload)->generation;
  client->send(msg_e::lock, serialize_single("generation", generation), 1);
  run();
  EXPECT_EQ(parse_error(of_type(client->frames(), msg_e::error).at(0).payload)->code, "too_large");

  ++backend->revision;
  client->send(msg_e::lock, serialize_single("generation", generation), 3);
  run();
  EXPECT_EQ(parse_error(of_type(client->frames(), msg_e::error).at(0).payload)->code, "stale_generation");
}

/**
 * @brief Drives a client that announced files through the host's staging requests.
 */
class ClipboardStagingFlowTest: public ClipboardServiceTest {
protected:
  /**
   * @brief Announce files and answer the LOCK.
   *
   * @param entries Manifest the client will serve.
   * @param extra Additional announced formats.
   * @return Frames sent after the LOCK reply.
   */
  std::vector<frame_t> announce_files(const std::vector<file_entry_t> &entries, const std::vector<format_t> &extra = {}) {
    backend_files = true;
    make_service();
    client = open_client("a", 1);
    std::vector<format_t> formats {{std::string {mime::files}, std::nullopt}};
    formats.insert(formats.end(), extra.begin(), extra.end());
    client->send(msg_e::announce, serialize(announce_t {1, formats}));
    run();
    auto frames = client->frames();
    lock_stream = of_type(frames, msg_e::lock).at(0).stream_id;
    std::uint64_t total = 0;
    for (const auto &entry : entries) {
      total += entry.size;
    }
    client->send(msg_e::lock_reply, serialize(lock_reply_t {7, entries.size(), total}), lock_stream, flag_end);
    run();
    auto more = client->frames();
    frames.insert(frames.end(), more.begin(), more.end());
    return frames;
  }

  /**
   * @brief Serve the manifest the host requested.
   *
   * @param frames Frames containing the GET_FILE_LIST.
   * @param entries Manifest entries.
   * @return Frames sent afterwards.
   */
  std::vector<frame_t> serve_manifest(const std::vector<frame_t> &frames, const std::vector<file_entry_t> &entries) {
    std::string manifest;
    for (const auto &entry : entries) {
      manifest += serialize(entry);
    }
    client->send(msg_e::data, manifest, of_type(frames, msg_e::get_file_list).at(0).stream_id, flag_end);
    run();
    return client->frames();
  }

  /**
   * @brief Answer GET_FILE requests until the host stops asking.
   *
   * @param frames Frames containing the first GET_FILE.
   * @param bodies File bodies by manifest index.
   * @return Frames sent after the last file.
   */
  std::vector<frame_t> serve_files(std::vector<frame_t> frames, const std::map<std::uint64_t, std::string> &bodies) {
    while (true) {
      const auto requests = of_type(frames, msg_e::get_file);
      if (requests.empty()) {
        return frames;
      }
      for (const auto &request : requests) {
        const auto get = parse_get_file(request.payload);
        client->send(msg_e::data, bodies.at(get->index).substr(get->offset, get->length), request.stream_id, flag_end);
      }
      run();
      frames = client->frames();
    }
  }

  std::shared_ptr<memory_connection_t> client;  ///< Announcing client.
  std::uint32_t lock_stream = 0;  ///< Stream of the host's LOCK.
};

TEST_F(ClipboardStagingFlowTest, ClientFilesAreStagedThenInstalled) {
  const std::vector<file_entry_t> entries {
    {0, "dir", 0, 1000, true},
    {1, "dir/f.txt", 4, 1700000000, false},
    {2, "DIR", 0, 1000, true},
    {3, "top.txt", 2, 1000, false},
    {4, "empty", 0, 1000, false},
  };
  auto frames = announce_files(entries, {{text_mime, std::nullopt}});
  answer_gets(*client, frames, {{text_mime, "the paths"}});
  frames = serve_manifest(frames, entries);
  frames = serve_files(frames, {{1, "data"}, {3, "hi"}});
  run();
  const auto more = client->frames();
  frames.insert(frames.end(), more.begin(), more.end());

  const auto unlocks = of_type(frames, msg_e::unlock);
  ASSERT_EQ(unlocks.size(), 1);
  EXPECT_EQ(parse_single(unlocks[0].payload, "lease"), 7u);
  EXPECT_EQ(last_ack(frames), "installed");
  ASSERT_EQ(backend->installs.size(), 1);
  EXPECT_EQ(backend->installs[0].at(text_mime), "the paths");
  const auto &paths = backend->installed_paths.at(0);
  ASSERT_EQ(paths.size(), 4);
  const std::filesystem::path root = std::filesystem::path {paths[0]}.parent_path();
  EXPECT_EQ(root.parent_path(), config.staging_area);
  EXPECT_EQ(std::filesystem::path {paths[1]}.filename(), "DIR (1)");
  EXPECT_EQ(read_file(root / "dir" / "f.txt"), "data");
  EXPECT_EQ(read_file(root / "top.txt"), "hi");
  EXPECT_TRUE(std::filesystem::exists(root / "empty"));
  EXPECT_EQ(std::filesystem::file_size(root / "empty"), 0);
  EXPECT_EQ(service->current_root()->path(), root);

  client->send(msg_e::announce, serialize(announce_t {2, {}}));
  run();
  EXPECT_EQ(backend->installs.size(), 2);
  EXPECT_FALSE(service->current_root());
  EXPECT_TRUE(std::filesystem::exists(root));
  EXPECT_TRUE(std::filesystem::exists(root.string() + ".released"));
}

TEST_F(ClipboardStagingFlowTest, ClearFromTheOwnerReleasesTheStagedRoot) {
  const std::vector<file_entry_t> entries {{0, "f", 1, 0, false}};
  auto frames = announce_files(entries);
  frames = serve_manifest(frames, entries);
  serve_files(frames, {{0, "x"}});
  run();
  const auto root = service->current_root();
  ASSERT_TRUE(root);
  client->send(msg_e::clear, serialize_single("generation", 1));
  run();
  EXPECT_FALSE(service->current_root());
  EXPECT_TRUE(std::filesystem::exists(root->path().string() + ".released"));
}

TEST_F(ClipboardStagingFlowTest, OwnerCloseReleasesTheStagedRoot) {
  const std::vector<file_entry_t> entries {{0, "f", 1, 0, false}};
  auto frames = announce_files(entries);
  frames = serve_manifest(frames, entries);
  serve_files(frames, {{0, "x"}});
  run();
  const auto root = service->current_root();
  ASSERT_TRUE(root);
  client->close();
  run();
  EXPECT_FALSE(service->current_root());
  EXPECT_TRUE(std::filesystem::exists(root->path().string() + ".released"));
}

TEST_F(ClipboardStagingFlowTest, HostCopyReleasesTheStagedRoot) {
  const std::vector<file_entry_t> entries {{0, "f", 1, 0, false}};
  auto frames = announce_files(entries);
  frames = serve_manifest(frames, entries);
  serve_files(frames, {{0, "x"}});
  run();
  const auto root = service->current_root();
  ASSERT_TRUE(root);
  backend->change({text_mime}, {{text_mime, "host"}});
  run();
  EXPECT_FALSE(service->current_root());
  EXPECT_TRUE(std::filesystem::exists(root->path().string() + ".released"));
}

TEST_F(ClipboardStagingFlowTest, TransferBeyondTheLimitIsRefused) {
  config.limits.max_transfer_size = 3;
  const auto frames = announce_files({{0, "f", 4, 0, false}});
  EXPECT_EQ(of_type(frames, msg_e::unlock).size(), 1);
  EXPECT_EQ(last_ack(frames), "too_large");
  EXPECT_EQ(staged_roots(config.staging_area), 0);
}

TEST_F(ClipboardStagingFlowTest, ManifestThatDisagreesWithTheLockIsRefused) {
  const std::vector<file_entry_t> entries {{0, "f", 1, 0, false}};
  const auto frames = announce_files(entries);
  const auto after = serve_manifest(frames, {{0, "f", 1, 0, false}, {1, "g", 1, 0, false}});
  run();
  const auto all = client->frames();
  EXPECT_EQ(last_ack(after.empty() ? all : after), "io_error");
  run();
  EXPECT_EQ(staged_roots(config.staging_area), 0);
  EXPECT_TRUE(backend->installs.empty());
}

TEST_F(ClipboardStagingFlowTest, UnsafeManifestPathsAreRefused) {
  const std::vector<file_entry_t> entries {{0, "../escape", 1, 0, false}};
  const auto frames = announce_files(entries);
  EXPECT_EQ(last_ack(serve_manifest(frames, entries)), "io_error");
  run();
  EXPECT_EQ(staged_roots(config.staging_area), 0);
  EXPECT_FALSE(std::filesystem::exists(config.staging_area.parent_path() / "escape"));
}

TEST_F(ClipboardStagingFlowTest, ShortFileDataFailsTheInstall) {
  const std::vector<file_entry_t> entries {{0, "f", 4, 0, false}};
  auto frames = announce_files(entries);
  frames = serve_manifest(frames, entries);
  client->send(msg_e::data, "da", of_type(frames, msg_e::get_file).at(0).stream_id, flag_end);
  run();
  frames = client->frames();
  EXPECT_EQ(last_ack(frames), "io_error");
  EXPECT_EQ(of_type(frames, msg_e::unlock).size(), 1);
  run();
  EXPECT_EQ(staged_roots(config.staging_area), 0);
}

TEST_F(ClipboardStagingFlowTest, LockRefusalIsAcknowledged) {
  backend_files = true;
  make_service();
  client = open_client("a", 1);
  client->send(msg_e::announce, serialize(announce_t {1, {{std::string {mime::files}, std::nullopt}}}));
  run();
  const auto lock = of_type(client->frames(), msg_e::lock).at(0);
  client->send(msg_e::error, serialize(error_msg_t {"not_permitted", "denied"}), lock.stream_id, flag_end);
  run();
  EXPECT_EQ(last_ack(client->frames()), "not_permitted");
}

TEST_F(ClipboardStagingFlowTest, NewerAnnouncementDiscardsAStagingInProgress) {
  const std::vector<file_entry_t> entries {{0, "f", 4, 0, false}};
  auto frames = announce_files(entries);
  frames = serve_manifest(frames, entries);
  ASSERT_EQ(staged_roots(config.staging_area), 1);
  client->send(msg_e::announce, serialize(announce_t {2, {}}));
  run();
  frames = client->frames();
  EXPECT_EQ(of_type(frames, msg_e::unlock).size(), 1);
  EXPECT_EQ(of_type(frames, msg_e::cancel).size(), 1);
  EXPECT_EQ(staged_roots(config.staging_area), 0);
  EXPECT_EQ(backend->installs.size(), 1);
  EXPECT_TRUE(backend->installed_paths[0].empty());
}

TEST_F(ClipboardStagingFlowTest, StartSweepsRootsOfAPreviousRun) {
  std::filesystem::create_directories(config.staging_area / std::string(32, 'c'));
  make_service();
  EXPECT_TRUE(std::filesystem::exists(config.staging_area / (std::string(32, 'c') + ".released")));
}
#endif
