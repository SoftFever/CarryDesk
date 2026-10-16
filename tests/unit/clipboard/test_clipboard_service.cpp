/**
 * @file tests/unit/clipboard/test_clipboard_service.cpp
 * @brief Test src/clipboard/service.*
 */

// test includes
#include "../../tests_common.h"
#include "clipboard_test_utils.h"

// standard includes
#include <thread>

// lib includes
#include <boost/asio/io_context.hpp>

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
      make_service();
    }

    /**
     * @brief Stop the service.
     */
    void TearDown() override {
      if (service) {
        run();
        service.reset();
      }
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

TEST_F(ClipboardServiceTest, FilesAreNotServedYet) {
  backend_files = true;
  make_service();
  const auto client = open_client("a", 1);
  backend->change({std::string {mime::files}}, {});
  run();
  const auto generation = parse_announce(of_type(client->frames(), msg_e::announce).at(0).payload)->generation;
  client->send(msg_e::lock, serialize_single("generation", generation), 1);
  run();
  EXPECT_EQ(parse_error(of_type(client->frames(), msg_e::error).at(0).payload)->code, "io_error");
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
