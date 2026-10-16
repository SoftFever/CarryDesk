/**
 * @file tests/unit/clipboard/test_clipboard_protocol.cpp
 * @brief Test src/clipboard/protocol.*
 */

// test includes
#include "../../tests_common.h"

// local includes
#include <src/clipboard/protocol.h>

using namespace clipboard;

namespace {
  /**
   * @brief Decode every complete frame in a byte string.
   *
   * @param bytes Encoded frames.
   * @return Decoded frames.
   */
  std::vector<frame_t> decode_all(const std::string &bytes) {
    frame_decoder_t decoder;
    decoder.feed(bytes);
    std::vector<frame_t> frames;
    frame_t frame;
    while (decoder.next(frame) == frame_decoder_t::result_e::frame) {
      frames.push_back(frame);
    }
    return frames;
  }

  /**
   * @brief Build a raw frame header with an arbitrary length field.
   *
   * @param length Value of the length field.
   * @return Twelve header bytes.
   */
  std::string raw_header(std::uint32_t length) {
    std::string header(12, '\0');
    for (int i = 0; i < 4; ++i) {
      header[i] = static_cast<char>((length >> (8 * i)) & 0xFF);
    }
    return header;
  }
}  // namespace

TEST(ClipboardProtocolTest, FrameRoundTripIsLittleEndian) {
  const auto bytes = encode_frame(msg_e::get_file, flag_end, 0x01020304, "abc");
  ASSERT_EQ(bytes.size(), 15);
  EXPECT_EQ(static_cast<unsigned char>(bytes[0]), 11);
  EXPECT_EQ(static_cast<unsigned char>(bytes[4]), 11);
  EXPECT_EQ(static_cast<unsigned char>(bytes[6]), 1);
  EXPECT_EQ(static_cast<unsigned char>(bytes[8]), 0x04);
  EXPECT_EQ(static_cast<unsigned char>(bytes[11]), 0x01);

  const auto frames = decode_all(bytes);
  ASSERT_EQ(frames.size(), 1);
  EXPECT_EQ(frames[0].type, static_cast<std::uint16_t>(msg_e::get_file));
  EXPECT_TRUE(frames[0].end());
  EXPECT_EQ(frames[0].stream_id, 0x01020304u);
  EXPECT_EQ(frames[0].payload, "abc");
}

TEST(ClipboardProtocolTest, DecoderReassemblesFramesSplitAcrossReads) {
  const auto bytes = encode_frame(msg_e::ping, 0, 0, R"({"nonce":7})") + encode_frame(msg_e::data, 0, 3, std::string(limit::max_payload, 'x')) + encode_frame(msg_e::data, flag_end, 3, "");
  frame_decoder_t decoder;
  std::vector<frame_t> frames;
  frame_t frame;
  for (std::size_t i = 0; i < bytes.size(); i += 1000) {
    decoder.feed(std::string_view {bytes}.substr(i, 1000));
    while (decoder.next(frame) == frame_decoder_t::result_e::frame) {
      frames.push_back(frame);
    }
  }
  ASSERT_EQ(frames.size(), 3);
  EXPECT_EQ(frames[1].payload.size(), limit::max_payload);
  EXPECT_TRUE(frames[2].payload.empty());
  EXPECT_TRUE(frames[2].end());
  EXPECT_FALSE(frames[1].end());
  EXPECT_EQ(decoder.buffered(), 0);
}

TEST(ClipboardProtocolTest, DecoderWaitsForCompleteHeaderAndPayload) {
  frame_decoder_t decoder;
  frame_t frame;
  const auto bytes = encode_frame(msg_e::pong, 0, 0, "{}");
  decoder.feed(bytes.substr(0, 11));
  EXPECT_EQ(decoder.next(frame), frame_decoder_t::result_e::need_more);
  decoder.feed(bytes.substr(11, 2));
  EXPECT_EQ(decoder.next(frame), frame_decoder_t::result_e::need_more);
  EXPECT_EQ(decoder.buffered(), 13);
  decoder.feed(bytes.substr(13));
  EXPECT_EQ(decoder.next(frame), frame_decoder_t::result_e::frame);
  EXPECT_EQ(frame.payload, "{}");
}

TEST(ClipboardProtocolTest, DecoderRejectsInvalidLengths) {
  for (const std::uint32_t length : {0u, 7u, static_cast<std::uint32_t>(8 + limit::max_payload + 1), 0xFFFFFFFFu}) {
    frame_decoder_t decoder;
    frame_t frame;
    decoder.feed(raw_header(length));
    EXPECT_EQ(decoder.next(frame), frame_decoder_t::result_e::bad_length) << length;
  }
  frame_decoder_t decoder;
  frame_t frame;
  decoder.feed(raw_header(8));
  EXPECT_EQ(decoder.next(frame), frame_decoder_t::result_e::frame);
  EXPECT_TRUE(frame.payload.empty());
}

TEST(ClipboardProtocolTest, DecoderCompactsAfterLargeConsumption) {
  frame_decoder_t decoder;
  frame_t frame;
  const auto big = encode_frame(msg_e::data, 0, 1, std::string(limit::max_payload, 'a'));
  decoder.feed(big + big.substr(0, 5));
  ASSERT_EQ(decoder.next(frame), frame_decoder_t::result_e::frame);
  decoder.feed(big.substr(5));
  ASSERT_EQ(decoder.next(frame), frame_decoder_t::result_e::frame);
  EXPECT_EQ(frame.payload.size(), limit::max_payload);
  EXPECT_EQ(decoder.buffered(), 0);
}

TEST(ClipboardProtocolTest, ErrorNamesRoundTrip) {
  for (const auto error : {error_e::stale_generation, error_e::unknown_lease, error_e::not_permitted, error_e::too_large, error_e::unsupported_format, error_e::io_error, error_e::busy, error_e::bad_request, error_e::limit_exceeded}) {
    EXPECT_EQ(error_from_string(to_string(error)), error);
  }
  EXPECT_FALSE(error_from_string("installed"));
  EXPECT_FALSE(error_from_string(""));
}

TEST(ClipboardProtocolTest, CountsAreNonNegativeIntegersUpToTwoToTheFiftyThird) {
  const auto object = parse_object(R"({"a":0,"b":9007199254740991,"c":9007199254740992,"d":-1,"e":1.0,"f":"1","g":1e3,"h":null,"i":18446744073709551615})");
  ASSERT_TRUE(object);
  EXPECT_EQ(get_count(*object, "a"), 0u);
  EXPECT_EQ(get_count(*object, "b"), limit::max_json_integer);
  EXPECT_FALSE(get_count(*object, "c"));
  EXPECT_FALSE(get_count(*object, "d"));
  EXPECT_FALSE(get_count(*object, "e"));
  EXPECT_FALSE(get_count(*object, "f"));
  EXPECT_FALSE(get_count(*object, "g"));
  EXPECT_FALSE(get_count(*object, "h"));
  EXPECT_FALSE(get_count(*object, "i"));
  EXPECT_FALSE(get_count(*object, "missing"));
}

TEST(ClipboardProtocolTest, ParseObjectRejectsNonObjects) {
  EXPECT_FALSE(parse_object("[]"));
  EXPECT_FALSE(parse_object("1"));
  EXPECT_FALSE(parse_object("{"));
  EXPECT_FALSE(parse_object(""));
  EXPECT_TRUE(parse_object("{}"));
}

TEST(ClipboardProtocolTest, HelloRoundTripAndDefaults) {
  hello_t hello {1, "macos", {std::string {mime::text}, std::string {mime::files}}, {true, false, true}, 1000, std::nullopt};
  const auto parsed = parse_hello(serialize(hello));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->version, 1);
  EXPECT_EQ(parsed->os, "macos");
  EXPECT_EQ(parsed->formats, hello.formats);
  EXPECT_EQ(parsed->policy, hello.policy);
  EXPECT_EQ(parsed->max_item_size, 1000u);
  EXPECT_FALSE(parsed->max_transfer_size);

  const auto minimal = parse_hello(R"({"version":1,"unknown":{"x":1}})");
  ASSERT_TRUE(minimal);
  EXPECT_EQ(minimal->policy, policy_t {});
  EXPECT_TRUE(minimal->formats.empty());
  EXPECT_TRUE(minimal->os.empty());

  const auto partial_policy = parse_hello(R"({"version":1,"policy":{"client_to_host":true}})");
  ASSERT_TRUE(partial_policy);
  EXPECT_EQ(partial_policy->policy, (policy_t {false, true, false}));
}

TEST(ClipboardProtocolTest, HelloRejectsMistypedFields) {
  for (const auto *payload : {
         R"({})",
         R"({"version":"1"})",
         R"({"version":1,"os":5})",
         R"({"version":1,"formats":"text"})",
         R"({"version":1,"formats":[1]})",
         R"({"version":1,"policy":true})",
         R"({"version":1,"policy":{"files":"yes"}})",
         R"({"version":1,"limits":[]})",
         R"({"version":1,"limits":{"max_item_size":-5}})",
         R"({"version":1,"limits":{"max_transfer_size":1.5}})",
       }) {
    EXPECT_FALSE(parse_hello(payload)) << payload;
  }
}

TEST(ClipboardProtocolTest, HelloReplyRoundTrip) {
  hello_reply_t reply;
  reply.os = "linux";
  reply.formats = {std::string {mime::text}};
  reply.policy = {true, true, false};
  reply.limits = {64, 128};
  reply.host_clipboard = true;
  const auto parsed = parse_hello_reply(serialize(reply));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->version, 1);
  EXPECT_EQ(parsed->os, "linux");
  EXPECT_EQ(parsed->formats, reply.formats);
  EXPECT_EQ(parsed->policy, reply.policy);
  EXPECT_EQ(parsed->limits.max_item_size, 64);
  EXPECT_EQ(parsed->limits.max_transfer_size, 128);
  EXPECT_TRUE(parsed->host_clipboard);

  EXPECT_FALSE(parse_hello_reply(R"({"version":1})"));
  EXPECT_FALSE(parse_hello_reply(R"({"version":1,"limits":{"max_item_size":1}})"));
  EXPECT_FALSE(parse_hello_reply(R"({"version":1,"limits":{"max_transfer_size":1}})"));
  EXPECT_FALSE(parse_hello_reply(R"({"version":1,"limits":{"max_item_size":1,"max_transfer_size":1},"host_clipboard":1})"));
  EXPECT_FALSE(parse_hello_reply("x"));
}

TEST(ClipboardProtocolTest, AnnounceRoundTripKeepsOptionalSizes) {
  announce_t announce {5, {{std::string {mime::text}, 12}, {std::string {mime::files}, std::nullopt}}};
  const auto payload = serialize(announce);
  EXPECT_EQ(payload.find("\"size\"", payload.find("files")), std::string::npos);
  const auto parsed = parse_announce(payload);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->generation, 5);
  EXPECT_EQ(parsed->formats, announce.formats);

  const auto empty = parse_announce(R"({"generation":1,"formats":[]})");
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->formats.empty());

  for (const auto *payload : {
         R"({"formats":[]})",
         R"({"generation":1})",
         R"({"generation":1,"formats":{}})",
         R"({"generation":1,"formats":["text/html"]})",
         R"({"generation":1,"formats":[{"size":1}]})",
         R"({"generation":1,"formats":[{"mime":1}]})",
         R"({"generation":1,"formats":[{"mime":"text/html","size":-1}]})",
         R"({"generation":-1,"formats":[]})",
         "[]",
       }) {
    EXPECT_FALSE(parse_announce(payload)) << payload;
  }
}

TEST(ClipboardProtocolTest, AnnounceAckRoundTrip) {
  const auto parsed = parse_announce_ack(serialize(announce_ack_t {3, "installed"}));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->generation, 3);
  EXPECT_EQ(parsed->status, "installed");
  EXPECT_FALSE(parse_announce_ack(R"({"generation":3})"));
  EXPECT_FALSE(parse_announce_ack(R"({"status":"installed"})"));
  EXPECT_FALSE(parse_announce_ack(R"({"generation":3,"status":1})"));
  EXPECT_FALSE(parse_announce_ack("nope"));
}

TEST(ClipboardProtocolTest, GetRoundTrip) {
  const auto parsed = parse_get(serialize(get_t {9, std::string {mime::png}}));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->generation, 9);
  EXPECT_EQ(parsed->mime, mime::png);
  EXPECT_FALSE(parse_get(R"({"generation":9})"));
  EXPECT_FALSE(parse_get(R"({"mime":"x"})"));
  EXPECT_FALSE(parse_get(R"({"generation":9,"mime":[]})"));
  EXPECT_FALSE(parse_get("1"));
}

TEST(ClipboardProtocolTest, LockReplyRoundTrip) {
  const auto parsed = parse_lock_reply(serialize(lock_reply_t {1, 2, 3}));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->lease, 1);
  EXPECT_EQ(parsed->count, 2);
  EXPECT_EQ(parsed->total_bytes, 3);
  EXPECT_FALSE(parse_lock_reply(R"({"lease":1,"count":2})"));
  EXPECT_FALSE(parse_lock_reply(R"({"lease":1,"total_bytes":2})"));
  EXPECT_FALSE(parse_lock_reply(R"({"count":1,"total_bytes":2})"));
  EXPECT_FALSE(parse_lock_reply("{"));
}

TEST(ClipboardProtocolTest, GetFileRoundTrip) {
  const auto parsed = parse_get_file(serialize(get_file_t {1, 2, 3, 4}));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->lease, 1);
  EXPECT_EQ(parsed->index, 2);
  EXPECT_EQ(parsed->offset, 3);
  EXPECT_EQ(parsed->length, 4);
  EXPECT_FALSE(parse_get_file(R"({"lease":1,"index":2,"offset":3})"));
  EXPECT_FALSE(parse_get_file(R"({"lease":1,"index":2,"length":3})"));
  EXPECT_FALSE(parse_get_file(R"({"lease":1,"offset":2,"length":3})"));
  EXPECT_FALSE(parse_get_file(R"({"index":1,"offset":2,"length":3})"));
  EXPECT_FALSE(parse_get_file("[]"));
}

TEST(ClipboardProtocolTest, ErrorRoundTrip) {
  const auto parsed = parse_error(serialize(error_msg_t {"busy", "too many"}));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->code, "busy");
  EXPECT_EQ(parsed->message, "too many");
  const auto no_message = parse_error(R"({"code":"io_error"})");
  ASSERT_TRUE(no_message);
  EXPECT_TRUE(no_message->message.empty());
  EXPECT_FALSE(parse_error(R"({"message":"x"})"));
  EXPECT_FALSE(parse_error(R"({"code":1})"));
  EXPECT_FALSE(parse_error(R"({"code":"busy","message":2})"));
  EXPECT_FALSE(parse_error(""));
}

TEST(ClipboardProtocolTest, SingleFieldMessagesRoundTrip) {
  EXPECT_EQ(parse_single(serialize_single("nonce", 42), "nonce"), 42u);
  EXPECT_FALSE(parse_single(serialize_single("nonce", 42), "lease"));
  EXPECT_FALSE(parse_single("{", "nonce"));
}

TEST(ClipboardProtocolTest, FileEntryRoundTrip) {
  file_entry_t entry {3, "dir/fi le é.txt", 10, -5, false};
  const auto line = serialize(entry);
  ASSERT_EQ(line.back(), '\n');
  const auto parsed = parse_file_entry(std::string_view {line}.substr(0, line.size() - 1));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, entry);

  const auto dir = parse_file_entry(R"({"index":0,"path":"d","size":0,"mtime":1,"dir":true})");
  ASSERT_TRUE(dir);
  EXPECT_TRUE(dir->dir);

  for (const auto *payload : {
         R"({"path":"a","size":0,"mtime":1})",
         R"({"index":0,"size":0,"mtime":1})",
         R"({"index":0,"path":"a","mtime":1})",
         R"({"index":0,"path":"a","size":0})",
         R"({"index":0,"path":"a","size":0,"mtime":1.5})",
         R"({"index":0,"path":1,"size":0,"mtime":1})",
         R"({"index":0,"path":"a","size":0,"mtime":1,"dir":1})",
         "x",
       }) {
    EXPECT_FALSE(parse_file_entry(payload)) << payload;
  }
}

TEST(ClipboardProtocolTest, PolicyIntersection) {
  const policy_t all {true, true, true};
  const policy_t some {true, false, true};
  EXPECT_EQ(all & some, some);
  EXPECT_EQ((some & policy_t {false, true, true}), (policy_t {false, false, true}));
}

TEST(ClipboardProtocolTest, CountsAcceptSignedIntegersBuiltInCode) {
  const nlohmann::json object {{"signed", 5}, {"negative", -5}};
  EXPECT_EQ(get_count(object, "signed"), 5u);
  EXPECT_FALSE(get_count(object, "negative"));
}
