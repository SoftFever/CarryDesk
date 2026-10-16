/**
 * @file src/clipboard/protocol.cpp
 * @brief Definitions for the clipboard channel's frame codec and messages.
 */
// standard includes
#include <array>
#include <utility>

// local includes
#include "protocol.h"

namespace clipboard {
  namespace {
    using json = nlohmann::json;

    constexpr std::array<std::pair<error_e, std::string_view>, 9> error_names {{
      {error_e::stale_generation, "stale_generation"},
      {error_e::unknown_lease, "unknown_lease"},
      {error_e::not_permitted, "not_permitted"},
      {error_e::too_large, "too_large"},
      {error_e::unsupported_format, "unsupported_format"},
      {error_e::io_error, "io_error"},
      {error_e::busy, "busy"},
      {error_e::bad_request, "bad_request"},
      {error_e::limit_exceeded, "limit_exceeded"},
    }};

    /**
     * @brief Write a little-endian integer into a buffer.
     *
     * @tparam T Unsigned integer type.
     * @param out Buffer to append to.
     * @param value Value to write.
     */
    template<class T>
    void put_le(std::string &out, T value) {
      for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
      }
    }

    /**
     * @brief Read a little-endian integer from a buffer.
     *
     * @tparam T Unsigned integer type.
     * @param data First byte of the integer.
     * @return Decoded value.
     */
    template<class T>
    T get_le(const char *data) {
      T value = 0;
      for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(static_cast<unsigned char>(data[i])) << (8 * i);
      }
      return value;
    }

    /**
     * @brief Read an optional boolean field.
     *
     * @param object JSON object.
     * @param key Field name.
     * @param value Receives the value when present.
     * @return `false` when the field is present but not a boolean.
     */
    bool read_bool(const json &object, std::string_view key, bool &value) {
      const auto it = object.find(key);
      if (it == object.end()) {
        return true;
      }
      if (!it->is_boolean()) {
        return false;
      }
      value = it->get<bool>();
      return true;
    }

    /**
     * @brief Read an optional count field.
     *
     * @param object JSON object.
     * @param key Field name.
     * @param value Receives the value when present.
     * @return `false` when the field is present but invalid.
     */
    bool read_optional_count(const json &object, std::string_view key, std::optional<std::uint64_t> &value) {
      if (!object.contains(key)) {
        return true;
      }
      value = get_count(object, key);
      return value.has_value();
    }

    /**
     * @brief Read an optional string field.
     *
     * @param object JSON object.
     * @param key Field name.
     * @param value Receives the value when present.
     * @return `false` when the field is present but not a string.
     */
    bool read_string(const json &object, std::string_view key, std::string &value) {
      const auto it = object.find(key);
      if (it == object.end()) {
        return true;
      }
      if (!it->is_string()) {
        return false;
      }
      value = it->get<std::string>();
      return true;
    }

    /**
     * @brief Read an optional array of strings.
     *
     * @param object JSON object.
     * @param key Field name.
     * @param value Receives the strings when present.
     * @return `false` when the field is present but not an array of strings.
     */
    bool read_strings(const json &object, std::string_view key, std::vector<std::string> &value) {
      const auto it = object.find(key);
      if (it == object.end()) {
        return true;
      }
      if (!it->is_array()) {
        return false;
      }
      for (const auto &element : *it) {
        if (!element.is_string()) {
          return false;
        }
        value.push_back(element.get<std::string>());
      }
      return true;
    }

    /**
     * @brief Read a policy object.
     *
     * @param object JSON object containing `policy`.
     * @param policy Receives the policy; missing fields stay false.
     * @return `false` when the policy is present but malformed.
     */
    bool read_policy(const json &object, policy_t &policy) {
      const auto it = object.find("policy");
      if (it == object.end()) {
        return true;
      }
      return it->is_object() &&
             read_bool(*it, "host_to_client", policy.host_to_client) &&
             read_bool(*it, "client_to_host", policy.client_to_host) &&
             read_bool(*it, "files", policy.files);
    }

    /**
     * @brief Build the JSON form of a policy.
     *
     * @param policy Policy to convert.
     * @return JSON object.
     */
    json policy_json(const policy_t &policy) {
      return {{"host_to_client", policy.host_to_client}, {"client_to_host", policy.client_to_host}, {"files", policy.files}};
    }
  }  // namespace

  std::string_view to_string(const error_e error) {
    for (const auto &[code, name] : error_names) {
      if (code == error) {
        return name;
      }
    }
    return "io_error";
  }

  std::optional<error_e> error_from_string(const std::string_view name) {
    for (const auto &[code, error_name] : error_names) {
      if (error_name == name) {
        return code;
      }
    }
    return std::nullopt;
  }

  std::string encode_frame(const msg_e type, const std::uint16_t flags, const std::uint32_t stream_id, const std::string_view payload) {
    std::string frame;
    frame.reserve(limit::header_size + payload.size());
    put_le<std::uint32_t>(frame, static_cast<std::uint32_t>(8 + payload.size()));
    put_le<std::uint16_t>(frame, static_cast<std::uint16_t>(type));
    put_le<std::uint16_t>(frame, flags);
    put_le<std::uint32_t>(frame, stream_id);
    frame.append(payload);
    return frame;
  }

  void frame_decoder_t::feed(const std::string_view bytes) {
    if (offset > 0 && offset == buffer.size()) {
      buffer.clear();
      offset = 0;
    } else if (offset > limit::max_payload) {
      buffer.erase(0, offset);
      offset = 0;
    }
    buffer.append(bytes);
  }

  frame_decoder_t::result_e frame_decoder_t::next(frame_t &frame) {
    const auto available = buffer.size() - offset;
    if (available < limit::header_size) {
      return result_e::need_more;
    }
    const auto *header = buffer.data() + offset;
    const auto length = get_le<std::uint32_t>(header);
    if (length < 8 || length - 8 > limit::max_payload) {
      return result_e::bad_length;
    }
    const std::size_t payload_size = length - 8;
    if (available < limit::header_size + payload_size) {
      return result_e::need_more;
    }
    frame.type = get_le<std::uint16_t>(header + 4);
    frame.flags = get_le<std::uint16_t>(header + 6);
    frame.stream_id = get_le<std::uint32_t>(header + 8);
    frame.payload.assign(header + limit::header_size, payload_size);
    offset += limit::header_size + payload_size;
    return result_e::frame;
  }

  std::size_t frame_decoder_t::buffered() const {
    return buffer.size() - offset;
  }

  std::optional<json> parse_object(const std::string_view payload) {
    auto object = json::parse(payload, nullptr, false);
    if (object.is_discarded() || !object.is_object()) {
      return std::nullopt;
    }
    return object;
  }

  std::optional<std::uint64_t> get_count(const json &object, const std::string_view key) {
    const auto it = object.find(key);
    if (it == object.end()) {
      return std::nullopt;
    }
    if (it->is_number_unsigned()) {
      const auto value = it->get<std::uint64_t>();
      if (value <= limit::max_json_integer) {
        return value;
      }
      return std::nullopt;
    }
    if (it->is_number_integer() && it->get<std::int64_t>() >= 0) {
      return static_cast<std::uint64_t>(it->get<std::int64_t>());
    }
    return std::nullopt;
  }

  std::optional<hello_t> parse_hello(const std::string_view payload) {
    const auto object = parse_object(payload);
    if (!object) {
      return std::nullopt;
    }
    hello_t hello;
    const auto version = get_count(*object, "version");
    if (!version || !read_string(*object, "os", hello.os) || !read_strings(*object, "formats", hello.formats) || !read_policy(*object, hello.policy)) {
      return std::nullopt;
    }
    hello.version = *version;
    if (const auto it = object->find("limits"); it != object->end()) {
      if (!it->is_object() || !read_optional_count(*it, "max_item_size", hello.max_item_size) || !read_optional_count(*it, "max_transfer_size", hello.max_transfer_size)) {
        return std::nullopt;
      }
    }
    return hello;
  }

  std::optional<hello_reply_t> parse_hello_reply(const std::string_view payload) {
    const auto object = parse_object(payload);
    if (!object) {
      return std::nullopt;
    }
    hello_reply_t reply;
    const auto version = get_count(*object, "version");
    const auto limits = object->find("limits");
    if (!version || !read_string(*object, "os", reply.os) || !read_strings(*object, "formats", reply.formats) || !read_policy(*object, reply.policy) || !read_bool(*object, "host_clipboard", reply.host_clipboard) || limits == object->end() || !limits->is_object()) {
      return std::nullopt;
    }
    const auto max_item_size = get_count(*limits, "max_item_size");
    const auto max_transfer_size = get_count(*limits, "max_transfer_size");
    if (!max_item_size || !max_transfer_size) {
      return std::nullopt;
    }
    reply.version = *version;
    reply.limits = {*max_item_size, *max_transfer_size};
    return reply;
  }

  std::optional<announce_t> parse_announce(const std::string_view payload) {
    const auto object = parse_object(payload);
    if (!object) {
      return std::nullopt;
    }
    const auto generation = get_count(*object, "generation");
    const auto formats = object->find("formats");
    if (!generation || formats == object->end() || !formats->is_array()) {
      return std::nullopt;
    }
    announce_t announce {*generation, {}};
    for (const auto &element : *formats) {
      if (!element.is_object()) {
        return std::nullopt;
      }
      format_t format;
      const auto mime = element.find("mime");
      if (mime == element.end() || !mime->is_string() || !read_optional_count(element, "size", format.size)) {
        return std::nullopt;
      }
      format.mime = mime->get<std::string>();
      announce.formats.push_back(std::move(format));
    }
    return announce;
  }

  std::optional<announce_ack_t> parse_announce_ack(const std::string_view payload) {
    const auto object = parse_object(payload);
    if (!object) {
      return std::nullopt;
    }
    const auto generation = get_count(*object, "generation");
    const auto status = object->find("status");
    if (!generation || status == object->end() || !status->is_string()) {
      return std::nullopt;
    }
    return announce_ack_t {*generation, status->get<std::string>()};
  }

  std::optional<get_t> parse_get(const std::string_view payload) {
    const auto object = parse_object(payload);
    if (!object) {
      return std::nullopt;
    }
    const auto generation = get_count(*object, "generation");
    const auto mime = object->find("mime");
    if (!generation || mime == object->end() || !mime->is_string()) {
      return std::nullopt;
    }
    return get_t {*generation, mime->get<std::string>()};
  }

  std::optional<lock_reply_t> parse_lock_reply(const std::string_view payload) {
    const auto object = parse_object(payload);
    if (!object) {
      return std::nullopt;
    }
    const auto lease = get_count(*object, "lease");
    const auto count = get_count(*object, "count");
    const auto total_bytes = get_count(*object, "total_bytes");
    if (!lease || !count || !total_bytes) {
      return std::nullopt;
    }
    return lock_reply_t {*lease, *count, *total_bytes};
  }

  std::optional<get_file_t> parse_get_file(const std::string_view payload) {
    const auto object = parse_object(payload);
    if (!object) {
      return std::nullopt;
    }
    const auto lease = get_count(*object, "lease");
    const auto index = get_count(*object, "index");
    const auto offset = get_count(*object, "offset");
    const auto length = get_count(*object, "length");
    if (!lease || !index || !offset || !length) {
      return std::nullopt;
    }
    return get_file_t {*lease, *index, *offset, *length};
  }

  std::optional<error_msg_t> parse_error(const std::string_view payload) {
    const auto object = parse_object(payload);
    if (!object) {
      return std::nullopt;
    }
    error_msg_t error;
    const auto code = object->find("code");
    if (code == object->end() || !code->is_string() || !read_string(*object, "message", error.message)) {
      return std::nullopt;
    }
    error.code = code->get<std::string>();
    return error;
  }

  std::optional<std::uint64_t> parse_single(const std::string_view payload, const std::string_view key) {
    const auto object = parse_object(payload);
    if (!object) {
      return std::nullopt;
    }
    return get_count(*object, key);
  }

  std::optional<file_entry_t> parse_file_entry(const std::string_view line) {
    const auto object = parse_object(line);
    if (!object) {
      return std::nullopt;
    }
    file_entry_t entry;
    const auto index = get_count(*object, "index");
    const auto size = get_count(*object, "size");
    const auto path = object->find("path");
    const auto mtime = object->find("mtime");
    if (!index || !size || path == object->end() || !path->is_string() || mtime == object->end() || !mtime->is_number_integer() || !read_bool(*object, "dir", entry.dir)) {
      return std::nullopt;
    }
    entry.index = *index;
    entry.size = *size;
    entry.path = path->get<std::string>();
    entry.mtime = mtime->get<std::int64_t>();
    return entry;
  }

  std::string serialize(const hello_t &message) {
    json object {{"version", message.version}, {"os", message.os}, {"formats", message.formats}, {"policy", policy_json(message.policy)}};
    json limits = json::object();
    if (message.max_item_size) {
      limits["max_item_size"] = *message.max_item_size;
    }
    if (message.max_transfer_size) {
      limits["max_transfer_size"] = *message.max_transfer_size;
    }
    object["limits"] = std::move(limits);
    return object.dump();
  }

  std::string serialize(const hello_reply_t &message) {
    return json {
      {"version", message.version},
      {"os", message.os},
      {"formats", message.formats},
      {"policy", policy_json(message.policy)},
      {"limits", {{"max_item_size", message.limits.max_item_size}, {"max_transfer_size", message.limits.max_transfer_size}}},
      {"host_clipboard", message.host_clipboard},
    }
      .dump();
  }

  std::string serialize(const announce_t &message) {
    json formats = json::array();
    for (const auto &format : message.formats) {
      json entry {{"mime", format.mime}};
      if (format.size) {
        entry["size"] = *format.size;
      }
      formats.push_back(std::move(entry));
    }
    return json {{"generation", message.generation}, {"formats", std::move(formats)}}.dump();
  }

  std::string serialize(const announce_ack_t &message) {
    return json {{"generation", message.generation}, {"status", message.status}}.dump();
  }

  std::string serialize(const get_t &message) {
    return json {{"generation", message.generation}, {"mime", message.mime}}.dump();
  }

  std::string serialize(const lock_reply_t &message) {
    return json {{"lease", message.lease}, {"count", message.count}, {"total_bytes", message.total_bytes}}.dump();
  }

  std::string serialize(const get_file_t &message) {
    return json {{"lease", message.lease}, {"index", message.index}, {"offset", message.offset}, {"length", message.length}}.dump();
  }

  std::string serialize(const error_msg_t &message) {
    return json {{"code", message.code}, {"message", message.message}}.dump();
  }

  std::string serialize(const file_entry_t &entry) {
    return json {{"index", entry.index}, {"path", entry.path}, {"size", entry.size}, {"mtime", entry.mtime}, {"dir", entry.dir}}.dump() + "\n";
  }

  std::string serialize_single(const std::string_view key, const std::uint64_t value) {
    json object = json::object();
    object[std::string {key}] = value;
    return object.dump();
  }
}  // namespace clipboard
