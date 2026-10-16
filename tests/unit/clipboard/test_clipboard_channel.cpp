/**
 * @file tests/unit/clipboard/test_clipboard_channel.cpp
 * @brief Test src/clipboard/channel.*
 */

// test includes
#include "../../tests_common.h"

// local includes
#include <src/clipboard/channel.h>

using namespace clipboard;
using namespace std::chrono_literals;

namespace {
  constexpr std::uint64_t mib = 1048576;

  /**
   * @brief Delegate that records every call.
   */
  struct fake_delegate_t: channel_delegate_t {
    /**
     * @brief Recorded GET.
     */
    struct get_call_t {
      std::uint32_t stream;  ///< Response stream.
      std::uint64_t revision;  ///< Native revision.
      std::string mime;  ///< Requested format.
    };

    /**
     * @brief Recorded GET_FILE.
     */
    struct file_call_t {
      std::uint32_t stream;  ///< Response stream.
      std::uint64_t index;  ///< Manifest index.
      std::uint64_t offset;  ///< First byte.
      std::uint64_t length;  ///< Byte count.
    };

    std::optional<host_clipboard_t> current_host_clipboard(channel_t &) override {
      ++host_queries;
      return host;
    }

    void on_open(channel_t &) override {
      ++opened;
    }

    void on_client_announce(channel_t &, std::uint64_t generation, const std::vector<format_t> &formats) override {
      announces.emplace_back(generation, formats);
    }

    void on_client_clear(channel_t &, std::uint64_t generation) override {
      clears.push_back(generation);
    }

    void serve_get(channel_t &, std::uint32_t stream, std::uint64_t revision, const std::string &mime) override {
      gets.push_back({stream, revision, mime});
    }

    void serve_lock(channel_t &, std::uint32_t stream, std::uint64_t revision) override {
      locks.emplace_back(stream, revision);
    }

    void serve_file(channel_t &, std::uint32_t stream, const std::shared_ptr<const manifest_t> &, std::uint64_t index, std::uint64_t offset, std::uint64_t length) override {
      files.push_back({stream, index, offset, length});
    }

    void cancel_serving(channel_t &, std::uint32_t stream) override {
      cancelled.push_back(stream);
    }

    void on_lease_released(channel_t &, const std::shared_ptr<const manifest_t> &manifest) override {
      released.push_back(manifest);
    }

    void on_closed(channel_t &) override {
      ++closed;
    }

    std::optional<host_clipboard_t> host;  ///< Host clipboard reported at HELLO.
    int host_queries = 0;  ///< Calls of `current_host_clipboard`.
    int opened = 0;  ///< Calls of `on_open`.
    int closed = 0;  ///< Calls of `on_closed`.
    std::vector<std::pair<std::uint64_t, std::vector<format_t>>> announces;  ///< Client announcements.
    std::vector<std::uint64_t> clears;  ///< Client clears.
    std::vector<get_call_t> gets;  ///< Served GETs.
    std::vector<std::pair<std::uint32_t, std::uint64_t>> locks;  ///< Served LOCKs.
    std::vector<file_call_t> files;  ///< Served GET_FILEs.
    std::vector<std::uint32_t> cancelled;  ///< Cancelled served streams.
    std::vector<std::shared_ptr<const manifest_t>> released;  ///< Released leases.
  };

  /**
   * @brief Drive one channel through an in-memory transport.
   */
  class ClipboardChannelTest: public testing::Test {
  protected:
    /**
     * @brief Create the channel with default settings.
     */
    void SetUp() override {
      settings.policy = {true, true, true};
      settings.limits = {64 * mib, 4096 * mib};
      settings.formats = {std::string {mime::text}, std::string {mime::html}, std::string {mime::rtf}, std::string {mime::png}, std::string {mime::files}};
      settings.os = "linux";
      make_channel();
    }

    /**
     * @brief Replace the channel with a new one using the current settings.
     */
    void make_channel() {
      channel = std::make_unique<channel_t>(7, settings, delegate, [this]() {
        return now;
      });
      channel->set_output_callback([this]() {
        ++output_signals;
      });
      decoder = {};
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
      channel->feed(encode_frame(type, flags, stream, payload));
    }

    /**
     * @brief Complete the handshake.
     *
     * @param policy Client policy.
     * @param formats Client formats; all canonical formats when empty.
     * @return Frames the host sent in answer.
     */
    std::vector<frame_t> open(const policy_t &policy = {true, true, true}, std::vector<std::string> formats = {}) {
      if (formats.empty()) {
        formats = settings.formats;
      }
      send(msg_e::hello, serialize(hello_t {1, "macos", formats, policy, std::nullopt, std::nullopt}));
      return output();
    }

    /**
     * @brief Take and decode everything the channel queued.
     *
     * @return Decoded frames.
     */
    std::vector<frame_t> output() {
      decoder.feed(channel->take_output(std::numeric_limits<std::size_t>::max()));
      std::vector<frame_t> frames;
      frame_t frame;
      while (decoder.next(frame) == frame_decoder_t::result_e::frame) {
        frames.push_back(frame);
      }
      return frames;
    }

    /**
     * @brief Advance the clock while the client keeps the channel alive.
     *
     * @param duration Time to advance.
     */
    void advance_alive(std::chrono::seconds duration) {
      while (duration > 0s) {
        const auto step = std::min<std::chrono::seconds>(duration, 5s);
        now += step;
        duration -= step;
        send(msg_e::pong, R"({"nonce":1})");
        channel->tick();
      }
    }

    /**
     * @brief Announce host content and drop the resulting frames.
     *
     * @param formats Host formats.
     * @param revision Native revision.
     * @return Host generation.
     */
    std::uint64_t announce(std::vector<std::string> formats, std::uint64_t revision = 11) {
      const auto generation = channel->announce_host({revision, std::move(formats)});
      output();
      return generation;
    }

    /**
     * @brief Build a manifest.
     *
     * @return Manifest with a directory and two files.
     */
    static std::shared_ptr<manifest_t> make_manifest() {
      auto manifest = std::make_shared<manifest_t>();
      manifest->entries = {{0, "d", 0, 1, true}, {1, "d/a.txt", 5, 2, false}, {2, "big.bin", 3 * mib, 3, false}};
      manifest->total_bytes = 5 + 3 * mib;
      return manifest;
    }

    /**
     * @brief Parse an ERROR frame's code.
     *
     * @param frame ERROR frame.
     * @return Error code name.
     */
    static std::string error_code(const frame_t &frame) {
      EXPECT_EQ(frame.type, static_cast<std::uint16_t>(msg_e::error));
      const auto error = parse_error(frame.payload);
      return error ? error->code : std::string {};
    }

    /**
     * @brief Check that the channel failed with a fatal error.
     *
     * @param code Expected error code.
     */
    void expect_fatal(std::string_view code) {
      const auto frames = output();
      ASSERT_FALSE(frames.empty());
      EXPECT_EQ(frames.back().stream_id, 0);
      EXPECT_EQ(error_code(frames.back()), code);
      EXPECT_EQ(channel->state(), channel_t::state_e::closing);
      EXPECT_TRUE(channel->wants_close());
      EXPECT_EQ(delegate.closed, 1);
    }

    time_point now = time_point {} + 1h;  ///< Fake clock.
    host_settings_t settings;  ///< Host settings for new channels.
    fake_delegate_t delegate;  ///< Recording delegate.
    std::unique_ptr<channel_t> channel;  ///< Channel under test.
    frame_decoder_t decoder;  ///< Decoder of the host's output.
    int output_signals = 0;  ///< Output callback count.
  };
}  // namespace

TEST_F(ClipboardChannelTest, HelloNegotiatesPolicyFormatsAndLimits) {
  send(msg_e::hello, R"({"version":1,"os":"macos","formats":["image/png","image/jpeg","text/plain;charset=utf-8"],"policy":{"host_to_client":true,"client_to_host":true},"limits":{"max_item_size":1000}})");
  const auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  ASSERT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::hello_reply));
  const auto reply = parse_hello_reply(frames[0].payload);
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->os, "linux");
  EXPECT_EQ(reply->formats, (std::vector<std::string> {std::string {mime::text}, std::string {mime::png}}));
  EXPECT_EQ(reply->policy, (policy_t {true, true, false}));
  EXPECT_EQ(reply->limits.max_item_size, 1000);
  EXPECT_EQ(reply->limits.max_transfer_size, 4096 * mib);
  EXPECT_FALSE(reply->host_clipboard);
  EXPECT_EQ(channel->state(), channel_t::state_e::open);
  EXPECT_EQ(channel->client_os(), "macos");
  EXPECT_EQ(channel->policy(), (policy_t {true, true, false}));
  EXPECT_EQ(channel->limits().max_item_size, 1000);
  EXPECT_EQ(channel->formats().size(), 2);
  EXPECT_EQ(delegate.opened, 1);
  EXPECT_GT(output_signals, 0);
}

TEST_F(ClipboardChannelTest, HelloLimitsNeverExceedHostOrHardCaps) {
  settings.limits = {limit::max_item_size * 2, limit::max_transfer_size * 2};
  settings.policy = {true, false, true};
  make_channel();
  send(msg_e::hello, serialize(hello_t {1, "macos", {std::string {mime::text}}, {true, true, true}, limit::max_json_integer, limit::max_json_integer}));
  const auto reply = parse_hello_reply(output().at(0).payload);
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->limits.max_item_size, limit::max_item_size);
  EXPECT_EQ(reply->limits.max_transfer_size, limit::max_transfer_size);
  EXPECT_EQ(reply->policy, (policy_t {true, false, true}));
}

TEST_F(ClipboardChannelTest, HostClipboardIsTheNextFrameAfterHelloReply) {
  delegate.host = host_clipboard_t {5, {std::string {mime::files}, std::string {mime::text}, "image/jpeg"}};
  const auto frames = open({true, true, false});
  ASSERT_EQ(frames.size(), 2);
  EXPECT_TRUE(parse_hello_reply(frames[0].payload)->host_clipboard);
  ASSERT_EQ(frames[1].type, static_cast<std::uint16_t>(msg_e::announce));
  const auto announce = parse_announce(frames[1].payload);
  ASSERT_TRUE(announce);
  EXPECT_EQ(announce->generation, 1);
  EXPECT_EQ(announce->formats, (std::vector<format_t> {{std::string {mime::text}, std::nullopt}}));
  EXPECT_EQ(channel->host_generation(), 1);
}

TEST_F(ClipboardChannelTest, UntransferableHostClipboardIsNotAnnouncedAtOpen) {
  delegate.host = host_clipboard_t {5, {"image/jpeg"}};
  auto frames = open();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_FALSE(parse_hello_reply(frames[0].payload)->host_clipboard);

  make_channel();
  delegate.host_queries = 0;
  delegate.host = host_clipboard_t {5, {std::string {mime::text}}};
  frames = open({false, true, true});
  ASSERT_EQ(frames.size(), 1);
  EXPECT_FALSE(parse_hello_reply(frames[0].payload)->host_clipboard);
  EXPECT_EQ(delegate.host_queries, 0);
}

TEST_F(ClipboardChannelTest, AnyFrameBeforeHelloIsFatal) {
  send(msg_e::ping, R"({"nonce":1})");
  expect_fatal("bad_request");
  send(msg_e::hello, serialize(hello_t {1, "macos", {}, {}, std::nullopt, std::nullopt}));
  EXPECT_TRUE(output().empty());
}

TEST_F(ClipboardChannelTest, SecondHelloIsFatal) {
  open();
  send(msg_e::hello, serialize(hello_t {1, "macos", {}, {}, std::nullopt, std::nullopt}));
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, HelloWithOtherVersionIsFatal) {
  send(msg_e::hello, R"({"version":2})");
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, MalformedHelloIsFatal) {
  send(msg_e::hello, R"({"version":1,"policy":{"files":1}})");
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, HelloMustArriveWithinTenSeconds) {
  now += 9s;
  channel->tick();
  EXPECT_TRUE(output().empty());
  now += 1s;
  channel->tick();
  expect_fatal("bad_request");
  channel->tick();
  EXPECT_TRUE(output().empty());
}

TEST_F(ClipboardChannelTest, ChannelMessageOnAStreamIsFatal) {
  open();
  send(msg_e::announce, R"({"generation":1,"formats":[]})", 3);
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, InvalidFrameLengthIsFatal) {
  open();
  std::string header(12, '\0');
  header[0] = 3;
  channel->feed(header);
  expect_fatal("limit_exceeded");
}

TEST_F(ClipboardChannelTest, PingIsAnsweredAndPongIsAccepted) {
  open();
  send(msg_e::ping, R"({"nonce":42})");
  send(msg_e::pong, R"({"nonce":1})");
  const auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::pong));
  EXPECT_EQ(parse_single(frames[0].payload, "nonce"), 42u);
}

TEST_F(ClipboardChannelTest, MalformedPingIsFatal) {
  open();
  send(msg_e::ping, R"({"nonce":-1})");
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, MalformedPongIsFatal) {
  open();
  send(msg_e::pong, "{}");
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, UnknownTypesAndHostOnlyTypesAreIgnored) {
  open();
  channel->feed(encode_frame(static_cast<msg_e>(99), 0, 0, "whatever"));
  send(msg_e::hello_reply, "{}");
  send(msg_e::announce_ack, R"({"generation":1,"status":"installed","extra":true})");
  EXPECT_TRUE(output().empty());
  EXPECT_EQ(channel->state(), channel_t::state_e::open);
}

TEST_F(ClipboardChannelTest, MalformedAnnounceAckIsFatal) {
  open();
  send(msg_e::announce_ack, R"({"generation":1})");
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, HostPingsEveryTenSecondsAndClosesWhenIdle) {
  open();
  now += 10s;
  channel->tick();
  auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::ping));
  now += 19s;
  channel->tick();
  EXPECT_EQ(output().size(), 1);
  now += 1s;
  channel->tick();
  EXPECT_EQ(channel->state(), channel_t::state_e::closed);
  EXPECT_TRUE(output().empty());
  EXPECT_EQ(delegate.closed, 1);
}

TEST_F(ClipboardChannelTest, ClientAnnounceIsFilteredAndAcknowledgedOnce) {
  open({true, true, false});
  send(msg_e::announce, R"({"generation":4,"formats":[{"mime":"text/plain;charset=utf-8","size":5},{"mime":"image/jpeg"},{"mime":"files"},{"mime":"text/plain;charset=utf-8"}]})");
  ASSERT_EQ(delegate.announces.size(), 1);
  EXPECT_EQ(delegate.announces[0].first, 4);
  EXPECT_EQ(delegate.announces[0].second, (std::vector<format_t> {{std::string {mime::text}, 5}}));
  EXPECT_TRUE(output().empty());
  EXPECT_TRUE(channel->client_generation_current(4));

  channel->ack_client(4, status_installed);
  channel->ack_client(4, status_installed);
  const auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  const auto ack = parse_announce_ack(frames[0].payload);
  EXPECT_EQ(ack->generation, 4);
  EXPECT_EQ(ack->status, "installed");
}

TEST_F(ClipboardChannelTest, NewerAnnounceAnswersPendingGenerationsStale) {
  open();
  send(msg_e::announce, R"({"generation":1,"formats":[]})");
  send(msg_e::announce, R"({"generation":3,"formats":[{"mime":"image/png"}]})");
  auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(parse_announce_ack(frames[0].payload)->status, "stale_generation");
  EXPECT_EQ(parse_announce_ack(frames[0].payload)->generation, 1);
  EXPECT_TRUE(delegate.announces[0].second.empty());
  EXPECT_FALSE(channel->client_generation_current(1));

  channel->ack_client(1, status_installed);
  EXPECT_TRUE(output().empty());
  channel->ack_client(3, to_string(error_e::io_error));
  frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(parse_announce_ack(frames[0].payload)->status, "io_error");
}

TEST_F(ClipboardChannelTest, AnnounceRejectedOutright) {
  open({true, true, true});
  std::string many = R"({"generation":1,"formats":[)";
  for (int i = 0; i < 33; ++i) {
    many += std::string {i ? "," : ""} + R"({"mime":"text/html"})";
  }
  send(msg_e::announce, many + "]}");
  send(msg_e::announce, R"({"generation":2,"formats":[{"mime":")" + std::string(129, 'x') + R"("}]})");
  send(msg_e::announce, R"({"generation":2,"formats":[]})");
  const auto frames = output();
  ASSERT_EQ(frames.size(), 3);
  EXPECT_EQ(parse_announce_ack(frames[0].payload)->status, "bad_request");
  EXPECT_EQ(parse_announce_ack(frames[1].payload)->status, "bad_request");
  EXPECT_EQ(parse_announce_ack(frames[2].payload)->status, "bad_request");
  EXPECT_TRUE(delegate.announces.empty());
  EXPECT_EQ(channel->client_generation(), 2);

  make_channel();
  open({true, false, true});
  send(msg_e::announce, R"({"generation":1,"formats":[]})");
  EXPECT_EQ(parse_announce_ack(output().at(0).payload)->status, "not_permitted");
  EXPECT_TRUE(delegate.announces.empty());
}

TEST_F(ClipboardChannelTest, MalformedAnnounceIsFatal) {
  open();
  send(msg_e::announce, R"({"generation":1})");
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, ClientClearOfCurrentGenerationOnly) {
  open();
  send(msg_e::clear, R"({"generation":0})");
  send(msg_e::announce, R"({"generation":3,"formats":[]})");
  send(msg_e::clear, R"({"generation":2})");
  send(msg_e::clear, R"({"generation":4})");
  EXPECT_TRUE(delegate.clears.empty());
  send(msg_e::clear, R"({"generation":3})");
  send(msg_e::clear, R"({"generation":3})");
  EXPECT_EQ(delegate.clears, std::vector<std::uint64_t> {3});
  EXPECT_FALSE(channel->client_generation_current(3));
  const auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(parse_announce_ack(frames[0].payload)->status, "stale_generation");
}

TEST_F(ClipboardChannelTest, MalformedClearIsFatal) {
  open();
  send(msg_e::clear, R"({"generation":"1"})");
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, HostAnnouncementsAndClears) {
  open({true, true, false});
  EXPECT_EQ(channel->announce_host({1, {std::string {mime::text}}}), 1);
  EXPECT_EQ(channel->announce_host({2, {std::string {mime::png}, std::string {mime::files}}}), 2);
  auto frames = output();
  ASSERT_EQ(frames.size(), 2);
  EXPECT_EQ(parse_announce(frames[1].payload)->formats, (std::vector<format_t> {{std::string {mime::png}, std::nullopt}}));

  EXPECT_EQ(channel->announce_host({3, {std::string {mime::files}}}), 0);
  frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::clear));
  EXPECT_EQ(parse_single(frames[0].payload, "generation"), 2u);
  EXPECT_EQ(channel->announce_host({4, {}}), 0);
  EXPECT_TRUE(output().empty());

  announce({std::string {mime::text}});
  channel->host_superseded();
  EXPECT_EQ(channel->announce_host({6, {}}), 0);
  EXPECT_TRUE(output().empty());

  channel->clear_host();
  frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(parse_single(frames[0].payload, "generation"), 3u);
}

TEST_F(ClipboardChannelTest, HostClipboardStaysOnHostWithoutPolicy) {
  open({false, true, true});
  EXPECT_EQ(channel->announce_host({1, {std::string {mime::text}}}), 0);
  EXPECT_TRUE(output().empty());
  channel->clear_host();
  EXPECT_EQ(parse_single(output().at(0).payload, "generation"), 0u);
}

TEST_F(ClipboardChannelTest, HostOperationsBeforeOpenAreIgnored) {
  EXPECT_EQ(channel->announce_host({1, {std::string {mime::text}}}), 0);
  channel->clear_host();
  channel->ack_client(1, status_installed);
  channel->unlock(1);
  EXPECT_EQ(channel->request(msg_e::get, "{}", {}, {}), 0);
  EXPECT_TRUE(output().empty());
}

TEST_F(ClipboardChannelTest, GetIsServedThroughTheDelegate) {
  open();
  const auto generation = announce({std::string {mime::text}, std::string {mime::html}}, 77);
  send(msg_e::get, serialize(get_t {generation, std::string {mime::html}}), 1);
  ASSERT_EQ(delegate.gets.size(), 1);
  EXPECT_EQ(delegate.gets[0].stream, 1);
  EXPECT_EQ(delegate.gets[0].revision, 77);
  EXPECT_EQ(delegate.gets[0].mime, mime::html);
  channel->respond_bytes(1, "<b>x</b>");
  channel->respond_bytes(1, "again");
  const auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::data));
  EXPECT_EQ(frames[0].stream_id, 1);
  EXPECT_TRUE(frames[0].end());
  EXPECT_EQ(frames[0].payload, "<b>x</b>");
}

TEST_F(ClipboardChannelTest, GetErrorsEndOnlyTheirStream) {
  open();
  const auto generation = announce({std::string {mime::text}, std::string {mime::files}});
  send(msg_e::get, serialize(get_t {generation + 1, std::string {mime::text}}), 1);
  send(msg_e::get, serialize(get_t {generation, std::string {mime::files}}), 3);
  send(msg_e::get, serialize(get_t {generation, std::string {mime::png}}), 5);
  send(msg_e::get, R"({"generation":1})", 7);
  const auto frames = output();
  ASSERT_EQ(frames.size(), 4);
  EXPECT_EQ(error_code(frames[0]), "stale_generation");
  EXPECT_EQ(error_code(frames[1]), "unsupported_format");
  EXPECT_EQ(error_code(frames[2]), "unsupported_format");
  EXPECT_EQ(error_code(frames[3]), "bad_request");
  for (const auto &frame : frames) {
    EXPECT_TRUE(frame.end());
  }
  EXPECT_EQ(frames[3].stream_id, 7);
  EXPECT_EQ(channel->state(), channel_t::state_e::open);
  EXPECT_TRUE(delegate.gets.empty());
}

TEST_F(ClipboardChannelTest, GetAfterClearOrWithoutPolicyIsRefused) {
  open();
  const auto generation = announce({std::string {mime::text}});
  channel->clear_host();
  output();
  send(msg_e::get, serialize(get_t {generation, std::string {mime::text}}), 1);
  EXPECT_EQ(error_code(output().at(0)), "stale_generation");

  make_channel();
  open({false, true, true});
  send(msg_e::get, serialize(get_t {0, std::string {mime::text}}), 1);
  EXPECT_EQ(error_code(output().at(0)), "not_permitted");
}

TEST_F(ClipboardChannelTest, RequestStreamIdsMustBeOddAndNew) {
  open();
  const auto generation = announce({std::string {mime::text}});
  for (const std::uint32_t stream : {5u, 3u, 1u, 9u, 7u, 13u, 11u, 15u}) {
    send(msg_e::get, serialize(get_t {generation, std::string {mime::text}}), stream);
    channel->respond_bytes(stream, "x");
  }
  EXPECT_EQ(channel->state(), channel_t::state_e::open);
  EXPECT_EQ(delegate.gets.size(), 8);
  output();
  send(msg_e::get, serialize(get_t {generation, std::string {mime::text}}), 9);
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, RequestOnAnEvenStreamIsFatal) {
  open();
  send(msg_e::get_file_list, R"({"lease":1})", 2);
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, NinthConcurrentRequestIsBusy) {
  open();
  const auto generation = announce({std::string {mime::text}});
  for (std::uint32_t stream = 1; stream <= 17; stream += 2) {
    send(msg_e::get, serialize(get_t {generation, std::string {mime::text}}), stream);
  }
  EXPECT_EQ(delegate.gets.size(), 8);
  const auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].stream_id, 17);
  EXPECT_EQ(error_code(frames[0]), "busy");
}

TEST_F(ClipboardChannelTest, LargeBodyRespectsCreditAndQueueCap) {
  open();
  const auto generation = announce({std::string {mime::png}});
  send(msg_e::get, serialize(get_t {generation, std::string {mime::png}}), 1);
  channel->respond_bytes(1, std::string(3 * mib + 10, 'p'));

  auto frames = output();
  std::uint64_t received = 0;
  for (const auto &frame : frames) {
    received += frame.payload.size();
    EXPECT_FALSE(frame.end());
    EXPECT_LE(frame.payload.size(), limit::max_payload);
  }
  EXPECT_EQ(received, mib);

  send(msg_e::credit, R"({"bytes":2097152})", 1);
  frames = output();
  for (const auto &frame : frames) {
    received += frame.payload.size();
  }
  EXPECT_EQ(received, 3 * mib);

  send(msg_e::credit, R"({"bytes":1000})", 1);
  frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].payload.size(), 10);
  EXPECT_TRUE(frames[0].end());
}

TEST_F(ClipboardChannelTest, ControlFramesGoAheadOfQueuedData) {
  open();
  const auto generation = announce({std::string {mime::png}});
  send(msg_e::get, serialize(get_t {generation, std::string {mime::png}}), 1);
  channel->respond_bytes(1, std::string(mib, 'p'));
  send(msg_e::ping, R"({"nonce":5})");
  const auto bytes = channel->take_output(100);
  frame_decoder_t first;
  first.feed(bytes);
  frame_t frame;
  ASSERT_EQ(first.next(frame), frame_decoder_t::result_e::frame);
  EXPECT_EQ(frame.type, static_cast<std::uint16_t>(msg_e::pong));
  EXPECT_EQ(first.next(frame), frame_decoder_t::result_e::need_more);
}

TEST_F(ClipboardChannelTest, EmptyBodyIsAnEmptyEnd) {
  open();
  const auto generation = announce({std::string {mime::text}});
  send(msg_e::get, serialize(get_t {generation, std::string {mime::text}}), 1);
  channel->respond_bytes(1, "");
  const auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_TRUE(frames[0].payload.empty());
  EXPECT_TRUE(frames[0].end());
}

TEST_F(ClipboardChannelTest, CancelStopsAServedStream) {
  open();
  const auto generation = announce({std::string {mime::text}});
  send(msg_e::get, serialize(get_t {generation, std::string {mime::text}}), 1);
  send(msg_e::cancel, "{}", 1);
  send(msg_e::cancel, "{}", 1);
  send(msg_e::credit, R"({"bytes":5})", 1);
  EXPECT_EQ(delegate.cancelled, std::vector<std::uint32_t> {1});
  channel->respond_bytes(1, "late");
  channel->respond_error(1, error_e::io_error);
  EXPECT_TRUE(output().empty());
}

TEST_F(ClipboardChannelTest, MalformedCreditIsFatal) {
  open();
  send(msg_e::credit, R"({"bytes":1.5})", 1);
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, StalledServedStreamFailsAfterThirtySeconds) {
  open();
  const auto generation = announce({std::string {mime::text}});
  send(msg_e::get, serialize(get_t {generation, std::string {mime::text}}), 1);
  advance_alive(25s);
  output();
  EXPECT_TRUE(delegate.cancelled.empty());
  advance_alive(5s);
  const auto frames = output();
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(error_code(frames.back()), "io_error");
  EXPECT_EQ(frames.back().stream_id, 1);
  EXPECT_EQ(delegate.cancelled, std::vector<std::uint32_t> {1});
}

TEST_F(ClipboardChannelTest, LockGrantsALeaseServingTheManifest) {
  open();
  const auto generation = announce({std::string {mime::files}}, 9);
  send(msg_e::lock, serialize_single("generation", generation), 1);
  ASSERT_EQ(delegate.locks.size(), 1);
  EXPECT_EQ(delegate.locks[0], (std::pair<std::uint32_t, std::uint64_t> {1, 9}));
  const auto manifest = make_manifest();
  channel->grant_lease(1, manifest);
  channel->grant_lease(1, manifest);
  auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::lock_reply));
  EXPECT_TRUE(frames[0].end());
  const auto reply = parse_lock_reply(frames[0].payload);
  EXPECT_EQ(reply->lease, 1);
  EXPECT_EQ(reply->count, 3);
  EXPECT_EQ(reply->total_bytes, manifest->total_bytes);
  EXPECT_EQ(channel->lease_count(), 1);

  send(msg_e::get_file_list, R"({"lease":1})", 3);
  frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_TRUE(frames[0].end());
  std::string expected;
  for (const auto &entry : manifest->entries) {
    expected += serialize(entry);
  }
  EXPECT_EQ(frames[0].payload, expected);
}

TEST_F(ClipboardChannelTest, LockIsRefusedWhenItCannotBeServed) {
  open({true, true, false});
  auto generation = announce({std::string {mime::text}});
  send(msg_e::lock, serialize_single("generation", generation), 1);
  EXPECT_EQ(error_code(output().at(0)), "not_permitted");

  make_channel();
  open();
  generation = announce({std::string {mime::text}});
  send(msg_e::lock, serialize_single("generation", generation), 1);
  send(msg_e::lock, serialize_single("generation", generation + 5), 3);
  send(msg_e::lock, "{}", 5);
  const auto frames = output();
  ASSERT_EQ(frames.size(), 3);
  EXPECT_EQ(error_code(frames[0]), "unsupported_format");
  EXPECT_EQ(error_code(frames[1]), "stale_generation");
  EXPECT_EQ(error_code(frames[2]), "bad_request");
  EXPECT_TRUE(delegate.locks.empty());
}

TEST_F(ClipboardChannelTest, AtMostFourLeases) {
  open();
  const auto generation = announce({std::string {mime::files}});
  for (std::uint32_t stream = 1; stream <= 7; stream += 2) {
    send(msg_e::lock, serialize_single("generation", generation), stream);
    channel->grant_lease(stream, make_manifest());
  }
  EXPECT_EQ(channel->lease_count(), 4);
  output();
  send(msg_e::lock, serialize_single("generation", generation), 9);
  EXPECT_EQ(error_code(output().at(0)), "busy");

  send(msg_e::unlock, R"({"lease":4})");
  send(msg_e::lock, serialize_single("generation", generation), 11);
  send(msg_e::lock, serialize_single("generation", generation), 13);
  channel->grant_lease(11, make_manifest());
  channel->grant_lease(13, make_manifest());
  const auto frames = output();
  ASSERT_EQ(frames.size(), 2);
  EXPECT_EQ(parse_lock_reply(frames[0].payload)->lease, 5);
  EXPECT_EQ(error_code(frames[1]), "busy");
}

TEST_F(ClipboardChannelTest, GetFileValidatesLeaseIndexAndRange) {
  open();
  const auto generation = announce({std::string {mime::files}});
  send(msg_e::lock, serialize_single("generation", generation), 1);
  channel->grant_lease(1, make_manifest());
  output();
  send(msg_e::get_file, serialize(get_file_t {2, 1, 0, 5}), 3);
  send(msg_e::get_file, serialize(get_file_t {1, 3, 0, 5}), 5);
  send(msg_e::get_file, serialize(get_file_t {1, 0, 0, 0}), 7);
  send(msg_e::get_file, serialize(get_file_t {1, 1, 3, 3}), 9);
  send(msg_e::get_file, serialize(get_file_t {1, 1, 6, 0}), 11);
  send(msg_e::get_file, R"({"lease":1})", 13);
  send(msg_e::get_file_list, R"({"lease":9})", 15);
  send(msg_e::get_file_list, R"({})", 17);
  send(msg_e::get_file, serialize(get_file_t {1, 1, 5, 0}), 19);
  const auto frames = output();
  ASSERT_EQ(frames.size(), 9);
  EXPECT_EQ(error_code(frames[0]), "unknown_lease");
  EXPECT_EQ(error_code(frames[1]), "unknown_lease");
  EXPECT_EQ(error_code(frames[2]), "bad_request");
  EXPECT_EQ(error_code(frames[3]), "bad_request");
  EXPECT_EQ(error_code(frames[4]), "bad_request");
  EXPECT_EQ(error_code(frames[5]), "bad_request");
  EXPECT_EQ(error_code(frames[6]), "unknown_lease");
  EXPECT_EQ(error_code(frames[7]), "bad_request");
  EXPECT_EQ(frames[8].type, static_cast<std::uint16_t>(msg_e::data));
  EXPECT_TRUE(frames[8].end());
  EXPECT_TRUE(frames[8].payload.empty());
  EXPECT_TRUE(delegate.files.empty());
}

TEST_F(ClipboardChannelTest, GetFileStreamsExactlyTheRequestedLength) {
  open();
  const auto generation = announce({std::string {mime::files}});
  send(msg_e::lock, serialize_single("generation", generation), 1);
  channel->grant_lease(1, make_manifest());
  output();
  const std::uint64_t length = 2 * mib;
  send(msg_e::get_file, serialize(get_file_t {1, 2, 100, length}), 3);
  ASSERT_EQ(delegate.files.size(), 1);
  EXPECT_EQ(delegate.files[0].index, 2);
  EXPECT_EQ(delegate.files[0].offset, 100);

  std::vector<std::size_t> pulls;
  channel->respond_stream(3, [&](std::size_t max) {
    pulls.push_back(max);
  });
  ASSERT_EQ(pulls.size(), 1);
  EXPECT_EQ(pulls[0], 256 * 1024);

  std::uint64_t received = 0;
  bool ended = false;
  std::uint64_t produced = 0;
  for (int round = 0; round < 100 && !ended; ++round) {
    if (pulls.size() > static_cast<std::size_t>(round)) {
      const auto chunk = std::min<std::uint64_t>(pulls[round], length - produced);
      produced += chunk;
      channel->respond_data(3, std::string(chunk, 'f'), false);
    }
    for (const auto &frame : output()) {
      received += frame.payload.size();
      ended = ended || frame.end();
    }
    if (!ended && pulls.size() == static_cast<std::size_t>(round + 1)) {
      send(msg_e::credit, serialize_single("bytes", mib), 3);
    }
  }
  EXPECT_TRUE(ended);
  EXPECT_EQ(received, length);
}

TEST_F(ClipboardChannelTest, GetFileProducerMustMatchTheLength) {
  open();
  const auto generation = announce({std::string {mime::files}});
  send(msg_e::lock, serialize_single("generation", generation), 1);
  channel->grant_lease(1, make_manifest());
  output();
  send(msg_e::get_file, serialize(get_file_t {1, 1, 0, 5}), 3);
  channel->respond_stream(3, [](std::size_t) {
  });
  channel->respond_data(3, "123456", false);
  send(msg_e::get_file, serialize(get_file_t {1, 1, 0, 5}), 5);
  channel->respond_stream(5, [](std::size_t) {
  });
  channel->respond_data(5, "12", true);
  send(msg_e::get_file, serialize(get_file_t {1, 1, 0, 5}), 7);
  channel->respond_bytes(7, "1234");
  send(msg_e::get_file, serialize(get_file_t {1, 1, 0, 5}), 9);
  channel->respond_stream(9, [](std::size_t) {
  });
  channel->respond_stream(9, [](std::size_t) {
  });
  channel->respond_data(9, "12345", false);
  const auto frames = output();
  ASSERT_EQ(frames.size(), 4);
  EXPECT_EQ(error_code(frames[0]), "io_error");
  EXPECT_EQ(error_code(frames[1]), "io_error");
  EXPECT_EQ(error_code(frames[2]), "io_error");
  EXPECT_EQ(frames[3].payload, "12345");
  EXPECT_TRUE(frames[3].end());
  channel->respond_data(9, "late", true);
  channel->respond_data(11, "unknown", true);
  EXPECT_TRUE(output().empty());
}

TEST_F(ClipboardChannelTest, UnlockReleasesALease) {
  open();
  const auto generation = announce({std::string {mime::files}});
  send(msg_e::lock, serialize_single("generation", generation), 1);
  const auto manifest = make_manifest();
  channel->grant_lease(1, manifest);
  send(msg_e::unlock, R"({"lease":5})");
  EXPECT_TRUE(delegate.released.empty());
  send(msg_e::unlock, R"({"lease":1})");
  ASSERT_EQ(delegate.released.size(), 1);
  EXPECT_EQ(delegate.released[0], manifest);
  output();
  send(msg_e::get_file_list, R"({"lease":1})", 3);
  EXPECT_EQ(error_code(output().at(0)), "unknown_lease");
}

TEST_F(ClipboardChannelTest, MalformedUnlockIsFatal) {
  open();
  send(msg_e::unlock, R"({"lease":true})");
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, LeaseExpiresAfterTenIdleMinutes) {
  open();
  const auto generation = announce({std::string {mime::files}});
  send(msg_e::lock, serialize_single("generation", generation), 1);
  channel->grant_lease(1, make_manifest());
  advance_alive(9min);
  send(msg_e::get_file_list, R"({"lease":1})", 3);
  advance_alive(9min);
  EXPECT_TRUE(delegate.released.empty());
  advance_alive(1min);
  EXPECT_EQ(delegate.released.size(), 1);
  EXPECT_EQ(channel->lease_count(), 0);
}

TEST_F(ClipboardChannelTest, HostRequestDeliversDataAndReturnsCredit) {
  open();
  std::string data;
  bool ended = false;
  const auto stream = channel->request(msg_e::get, serialize(get_t {1, std::string {mime::text}}), {}, {[&](std::string_view bytes) {
                                                                                                          data += bytes;
                                                                                                        },
                                                                                                        [&]() {
                                                                                                          ended = true;
                                                                                                        },
                                                                                                        [](error_e, const std::string &) {
                                                                                                          FAIL();
                                                                                                        }});
  EXPECT_EQ(stream, 2);
  auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::get));
  EXPECT_EQ(frames[0].stream_id, 2);
  EXPECT_EQ(frames[0].flags, 0);

  for (int i = 0; i < 5; ++i) {
    send(msg_e::data, std::string(limit::max_payload, 'a'), stream);
  }
  frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::credit));
  EXPECT_EQ(parse_single(frames[0].payload, "bytes"), 4u * limit::max_payload);
  send(msg_e::data, "", stream, flag_end);
  EXPECT_TRUE(ended);
  EXPECT_EQ(data.size(), 5 * limit::max_payload);
  send(msg_e::data, "late", stream, flag_end);
  EXPECT_EQ(data.size(), 5 * limit::max_payload);
}

TEST_F(ClipboardChannelTest, HostRequestSizeChecks) {
  open();
  std::vector<std::string> errors;
  const auto on_error = [&](error_e error, const std::string &) {
    errors.emplace_back(to_string(error));
  };
  const auto noop_data = [](std::string_view) {
  };
  const auto noop_end = []() {
  };
  const auto too_large = channel->request(msg_e::get, "{}", {10, std::nullopt, true}, {noop_data, noop_end, on_error});
  const auto too_long = channel->request(msg_e::get_file, "{}", {std::nullopt, 4, true}, {noop_data, noop_end, on_error});
  const auto too_short = channel->request(msg_e::get_file, "{}", {std::nullopt, 4, true}, {noop_data, noop_end, on_error});
  output();
  send(msg_e::data, std::string(11, 'x'), too_large);
  send(msg_e::data, "12345", too_long);
  send(msg_e::data, "123", too_short, flag_end);
  EXPECT_EQ(errors, (std::vector<std::string> {"too_large", "io_error", "io_error"}));
  const auto frames = output();
  ASSERT_EQ(frames.size(), 2);
  EXPECT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::cancel));
  EXPECT_EQ(frames[0].stream_id, too_large);
  EXPECT_EQ(frames[1].stream_id, too_long);
}

TEST_F(ClipboardChannelTest, DataBeyondCreditIsFatal) {
  open();
  const auto stream = channel->request(msg_e::get, "{}", {std::nullopt, std::nullopt, false}, {[](std::string_view) {
                                                                                               },
                                                                                               []() {
                                                                                               },
                                                                                               [](error_e, const std::string &) {
                                                                                               }});
  output();
  for (int i = 0; i < 16; ++i) {
    send(msg_e::data, std::string(limit::max_payload, 'a'), stream);
  }
  EXPECT_EQ(channel->state(), channel_t::state_e::open);
  send(msg_e::data, "x", stream);
  expect_fatal("limit_exceeded");
}

TEST_F(ClipboardChannelTest, ManualCreditIsReturnedWhenConsumed) {
  open();
  const auto stream = channel->request(msg_e::get_file, "{}", {std::nullopt, std::nullopt, false}, {[](std::string_view) {
                                                                                                    },
                                                                                                    []() {
                                                                                                    },
                                                                                                    [](error_e, const std::string &) {
                                                                                                    }});
  output();
  send(msg_e::data, std::string(1000, 'a'), stream);
  channel->grant(stream, 1000);
  EXPECT_TRUE(output().empty());
  for (int i = 0; i < 15; ++i) {
    send(msg_e::data, std::string(limit::max_payload, 'a'), stream);
  }
  channel->grant(stream, 0);
  channel->grant(stream, 10);
  const auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(parse_single(frames[0].payload, "bytes"), 1010u);
  channel->grant(99, 5);
  EXPECT_TRUE(output().empty());
}

TEST_F(ClipboardChannelTest, HostRequestErrorsAndLockReplies) {
  open();
  std::vector<std::string> errors;
  std::string lock_payload;
  bool lock_ended = false;
  const auto on_error = [&](error_e error, const std::string &message) {
    errors.push_back(std::string {to_string(error)} + ":" + message);
  };
  const auto first = channel->request(msg_e::get, "{}", {}, {[](std::string_view) {
                                                             },
                                                             []() {
                                                             },
                                                             on_error});
  const auto second = channel->request(msg_e::get, "{}", {}, {[](std::string_view) {
                                                              },
                                                              []() {
                                                              },
                                                              on_error});
  const auto third = channel->request(msg_e::get, "{}", {}, {[](std::string_view) {
                                                             },
                                                             []() {
                                                             },
                                                             on_error});
  const auto lock = channel->request(msg_e::lock, "{}", {}, {[&](std::string_view payload) {
                                                               lock_payload = payload;
                                                             },
                                                             [&]() {
                                                               lock_ended = true;
                                                             },
                                                             on_error});
  send(msg_e::error, serialize(error_msg_t {"stale_generation", "old"}), first, flag_end);
  send(msg_e::error, serialize(error_msg_t {"weird", "?"}), second, flag_end);
  send(msg_e::error, "not json", third, flag_end);
  send(msg_e::lock_reply, serialize(lock_reply_t {1, 2, 3}), lock, flag_end);
  EXPECT_EQ(errors, (std::vector<std::string> {"stale_generation:old", "io_error:?", "io_error:"}));
  EXPECT_TRUE(lock_ended);
  EXPECT_EQ(parse_lock_reply(lock_payload)->total_bytes, 3);
}

TEST_F(ClipboardChannelTest, DataOnALockStreamIsFatal) {
  open();
  const auto lock = channel->request(msg_e::lock, "{}", {}, {[](std::string_view) {
                                                             },
                                                             []() {
                                                             },
                                                             [](error_e, const std::string &) {
                                                             }});
  send(msg_e::data, "x", lock);
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, LockReplyOnAGetStreamIsFatal) {
  open();
  const auto get = channel->request(msg_e::get, "{}", {}, {[](std::string_view) {
                                                           },
                                                           []() {
                                                           },
                                                           [](error_e, const std::string &) {
                                                           }});
  send(msg_e::lock_reply, serialize(lock_reply_t {1, 2, 3}), get, flag_end);
  expect_fatal("bad_request");
}

TEST_F(ClipboardChannelTest, RequestsBeyondEightWaitForAFreeStream) {
  open();
  std::vector<std::uint32_t> streams;
  for (int i = 0; i < 10; ++i) {
    streams.push_back(channel->request(msg_e::get, "{}", {}, {[](std::string_view) {
                                                              },
                                                              []() {
                                                              },
                                                              [](error_e, const std::string &) {
                                                              }}));
  }
  EXPECT_EQ(output().size(), 8);
  EXPECT_EQ(channel->queued_requests(), 2);
  channel->cancel_request(streams[8]);
  EXPECT_EQ(channel->queued_requests(), 1);
  EXPECT_TRUE(output().empty());
  send(msg_e::data, "x", streams[8], flag_end);
  send(msg_e::data, "x", streams[0], flag_end);
  auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].stream_id, streams[9]);
  channel->cancel_request(streams[1]);
  frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::cancel));
  channel->cancel_request(streams[1]);
  EXPECT_TRUE(output().empty());
}

TEST_F(ClipboardChannelTest, CallbacksMayCancelTheirOwnRequest) {
  open();
  std::uint32_t stream = 0;
  int data_calls = 0;
  stream = channel->request(msg_e::get, "{}", {}, {[&](std::string_view) {
                                                     ++data_calls;
                                                     channel->cancel_request(stream);
                                                   },
                                                   []() {
                                                     FAIL();
                                                   },
                                                   [](error_e, const std::string &) {
                                                   }});
  output();
  send(msg_e::data, "x", stream, flag_end);
  EXPECT_EQ(data_calls, 1);
  const auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::cancel));
}

TEST_F(ClipboardChannelTest, StalledHostRequestFailsAfterThirtySeconds) {
  open();
  std::vector<error_e> errors;
  const auto stream = channel->request(msg_e::get, "{}", {}, {[](std::string_view) {
                                                              },
                                                              []() {
                                                              },
                                                              [&](error_e error, const std::string &) {
                                                                errors.push_back(error);
                                                              }});
  output();
  advance_alive(30s);
  EXPECT_EQ(errors, std::vector<error_e> {error_e::io_error});
  bool cancelled = false;
  for (const auto &frame : output()) {
    cancelled = cancelled || (frame.type == static_cast<std::uint16_t>(msg_e::cancel) && frame.stream_id == stream);
  }
  EXPECT_TRUE(cancelled);
}

TEST_F(ClipboardChannelTest, UnlockSendsTheClientsLease) {
  open();
  channel->unlock(3);
  const auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::unlock));
  EXPECT_EQ(parse_single(frames[0].payload, "lease"), 3u);
}

TEST_F(ClipboardChannelTest, CloseEndsStreamsRequestsAndLeasesOnce) {
  open();
  const auto generation = announce({std::string {mime::files}, std::string {mime::text}});
  send(msg_e::lock, serialize_single("generation", generation), 1);
  channel->grant_lease(1, make_manifest());
  send(msg_e::get, serialize(get_t {generation, std::string {mime::text}}), 3);
  std::vector<error_e> errors;
  channel->request(msg_e::get, "{}", {}, {[](std::string_view) {
                                          },
                                          []() {
                                          },
                                          [&](error_e error, const std::string &) {
                                            errors.push_back(error);
                                          }});
  channel->close();
  channel->close();
  EXPECT_EQ(channel->state(), channel_t::state_e::closed);
  EXPECT_TRUE(channel->wants_close());
  EXPECT_FALSE(channel->has_output());
  EXPECT_EQ(delegate.cancelled, std::vector<std::uint32_t> {3});
  EXPECT_EQ(errors, std::vector<error_e> {error_e::io_error});
  EXPECT_EQ(delegate.released.size(), 1);
  EXPECT_EQ(delegate.closed, 1);
  send(msg_e::ping, R"({"nonce":1})");
  channel->fail(error_e::io_error, "after close");
  channel->tick();
  EXPECT_TRUE(output().empty());
}

TEST_F(ClipboardChannelTest, ClientFatalErrorClosesTheChannel) {
  open();
  send(msg_e::error, serialize(error_msg_t {"limit_exceeded", "bye"}));
  EXPECT_EQ(channel->state(), channel_t::state_e::closed);
  EXPECT_EQ(delegate.closed, 1);
}

TEST_F(ClipboardChannelTest, FailFlushesControlFramesButDropsData) {
  open();
  const auto generation = announce({std::string {mime::png}});
  send(msg_e::get, serialize(get_t {generation, std::string {mime::png}}), 1);
  channel->respond_bytes(1, std::string(1000, 'p'));
  channel->ack_client(5, status_installed);
  channel->fail(error_e::io_error, "host shutting down");
  const auto frames = output();
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(error_code(frames[0]), "io_error");
}

TEST_F(ClipboardChannelTest, StalledRequestCallbackMayCloseTheChannel) {
  open();
  int errors = 0;
  const auto close_on_error = [&](error_e, const std::string &) {
    ++errors;
    channel->close();
  };
  channel->request(msg_e::get, "{}", {}, {[](std::string_view) {
                                          },
                                          []() {
                                          },
                                          close_on_error});
  channel->request(msg_e::get, "{}", {}, {[](std::string_view) {
                                          },
                                          []() {
                                          },
                                          close_on_error});
  output();
  now += 25s;
  send(msg_e::pong, R"({"nonce":1})");
  now += 5s;
  channel->tick();
  EXPECT_EQ(channel->state(), channel_t::state_e::closed);
  EXPECT_EQ(errors, 2);
  EXPECT_EQ(delegate.closed, 1);
}

TEST_F(ClipboardChannelTest, LockReplyBeyondTheLimitIsTooLarge) {
  open();
  std::vector<error_e> errors;
  const auto lock = channel->request(msg_e::lock, "{}", {8, std::nullopt, true}, {[](std::string_view) {
                                                                                    FAIL();
                                                                                  },
                                                                                  []() {
                                                                                    FAIL();
                                                                                  },
                                                                                  [&](error_e error, const std::string &) {
                                                                                    errors.push_back(error);
                                                                                  }});
  send(msg_e::lock_reply, serialize(lock_reply_t {1, 2, 3}), lock, flag_end);
  EXPECT_EQ(errors, std::vector<error_e> {error_e::too_large});
  EXPECT_EQ(channel->state(), channel_t::state_e::open);
}

TEST_F(ClipboardChannelTest, SynchronousProducersAreFramedBeforePumpReturns) {
  open();
  const auto generation = announce({std::string {mime::files}});
  send(msg_e::lock, serialize_single("generation", generation), 1);
  channel->grant_lease(1, make_manifest());
  output();

  send(msg_e::get_file, serialize(get_file_t {1, 1, 0, 5}), 3);
  channel->respond_stream(3, [this](std::size_t) {
    channel->respond_data(3, "12345", true);
  });
  send(msg_e::get_file, serialize(get_file_t {1, 1, 0, 5}), 5);
  channel->respond_stream(5, [this](std::size_t) {
    channel->respond_error(5, error_e::io_error, "gone");
  });
  send(msg_e::get_file, serialize(get_file_t {1, 1, 0, 5}), 7);
  send(msg_e::get_file, serialize(get_file_t {1, 1, 0, 5}), 9);
  channel->respond_stream(7, [this](std::size_t) {
    channel->respond_bytes(9, "abcde");
    channel->respond_data(7, "vwxyz", true);
  });

  const auto frames = output();
  std::map<std::uint32_t, std::string> bodies;
  std::map<std::uint32_t, std::string> errors;
  for (const auto &frame : frames) {
    if (frame.type == static_cast<std::uint16_t>(msg_e::data)) {
      bodies[frame.stream_id] += frame.payload;
      EXPECT_TRUE(frame.end());
    } else if (frame.type == static_cast<std::uint16_t>(msg_e::error)) {
      errors[frame.stream_id] = error_code(frame);
    }
  }
  EXPECT_EQ(bodies[3], "12345");
  EXPECT_EQ(errors[5], "io_error");
  EXPECT_EQ(bodies[7], "vwxyz");
  EXPECT_EQ(bodies[9], "abcde");
}
