/**
 * @file src/clipboard/protocol.h
 * @brief Declarations for the clipboard channel's frame codec and messages.
 * @details The wire format is specified in docs/clipboard_protocol.md. Everything here is pure: no I/O,
 *          no clocks and no global state.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

/**
 * @brief Clipboard sharing between the host and streaming clients.
 */
namespace clipboard {
  /**
   * @brief Host feature flag advertised in `x-ss-general.featureFlags` when the client may open a channel.
   * @details Far from upstream's `LI_FF_*` sequence, which allocates from the bottom.
   */
  inline constexpr std::uint32_t feature_flag = 0x40000000;

  inline constexpr int protocol_version = 1;  ///< Protocol version spoken by this host.
  inline constexpr std::string_view upgrade_path = "/clipboard";  ///< Request path of the upgrade.
  inline constexpr std::string_view upgrade_token = "sunshine-clipboard";  ///< Value of the `Upgrade` header.
  inline constexpr std::string_view version_header = "Sunshine-Clipboard-Version";  ///< Header carrying the protocol version.

  /**
   * @brief Protocol limits that every endpoint enforces whatever the peer requests.
   */
  namespace limit {
    inline constexpr std::size_t header_size = 12;  ///< Frame header size in bytes.
    inline constexpr std::size_t max_payload = 65536;  ///< Largest payload of any frame.
    inline constexpr std::uint64_t initial_credit = 1048576;  ///< DATA credit every response stream starts with.
    inline constexpr std::size_t max_formats = 32;  ///< Most formats in one ANNOUNCE.
    inline constexpr std::size_t max_mime_length = 128;  ///< Longest mime in an ANNOUNCE.
    inline constexpr std::size_t max_streams = 8;  ///< Most concurrent requests from one side.
    inline constexpr std::size_t max_leases = 4;  ///< Most leases one side holds per channel.
    inline constexpr std::size_t write_queue = 2 * 1048576;  ///< Queued DATA bytes after which producers pause.
    inline constexpr std::size_t max_channels = 8;  ///< Most channels open on the host.
    inline constexpr std::uint64_t max_json_integer = (std::uint64_t {1} << 53) - 1;  ///< Largest integer JSON carries exactly.
    inline constexpr std::uint64_t max_item_size = 256ULL * 1048576;  ///< Hard cap on one non-file item.
    inline constexpr std::uint64_t max_transfer_size = 1048576ULL * 1048576;  ///< Hard cap on one file paste (1 TiB).
    inline constexpr std::size_t max_manifest_entries = 100000;  ///< Most entries in a file manifest.
    inline constexpr std::size_t max_depth = 64;  ///< Deepest path in a file manifest.
    inline constexpr std::size_t max_segment = 255;  ///< Longest path segment in UTF-8 bytes.
    inline constexpr std::size_t max_path = 4096;  ///< Longest path in UTF-8 bytes.
    inline constexpr std::size_t max_manifest_bytes = 32 * 1048576;  ///< Largest encoded manifest.
  }  // namespace limit

  /**
   * @brief Protocol timers.
   */
  namespace timing {
    inline constexpr std::chrono::seconds hello_timeout {10};  ///< Time allowed for HELLO after the upgrade.
    inline constexpr std::chrono::seconds ping_interval {10};  ///< Interval between PINGs.
    inline constexpr std::chrono::seconds idle_timeout {30};  ///< Silence after which the channel closes.
    inline constexpr std::chrono::seconds stream_timeout {30};  ///< Time a stream may go without progress.
    inline constexpr std::chrono::minutes lease_timeout {10};  ///< Time a lease may go without a request.
  }  // namespace timing

  /**
   * @brief Canonical clipboard formats.
   */
  namespace mime {
    inline constexpr std::string_view text = "text/plain;charset=utf-8";  ///< UTF-8 text with LF line endings.
    inline constexpr std::string_view html = "text/html";  ///< UTF-8 HTML.
    inline constexpr std::string_view rtf = "text/rtf";  ///< Rich Text Format.
    inline constexpr std::string_view png = "image/png";  ///< PNG image.
    inline constexpr std::string_view files = "files";  ///< Files and folders, fetched through leases.
  }  // namespace mime

  /**
   * @brief Message types.
   */
  enum class msg_e : std::uint16_t {
    hello = 1,  ///< Client opens the channel.
    hello_reply = 2,  ///< Host answers HELLO with the effective settings.
    announce = 3,  ///< Sender became the clipboard owner.
    announce_ack = 4,  ///< Receiver installed an announcement or failed to.
    clear = 5,  ///< Sender's clipboard is empty or not available.
    get = 6,  ///< Request one format of a generation.
    lock = 7,  ///< Request a lease on a generation's files.
    lock_reply = 8,  ///< Lease granted.
    unlock = 9,  ///< Release a lease.
    get_file_list = 10,  ///< Request a lease's manifest.
    get_file = 11,  ///< Request a byte range of one file.
    data = 12,  ///< Raw response bytes.
    credit = 13,  ///< Grant more DATA bytes on a stream.
    cancel = 14,  ///< Requester aborts a stream.
    error = 15,  ///< Error on a stream or, on stream 0, a fatal channel error.
    ping = 16,  ///< Keepalive request.
    pong = 17,  ///< Keepalive answer.
  };

  inline constexpr std::uint16_t flag_end = 0x1;  ///< Last frame of a response.

  /**
   * @brief Error codes.
   */
  enum class error_e {
    stale_generation,  ///< The generation is not the sender's current one.
    unknown_lease,  ///< The lease or index is unknown, expired or released.
    not_permitted,  ///< Policy or the OS does not allow it.
    too_large,  ///< Exceeds a negotiated or local limit.
    unsupported_format,  ///< The format was not announced.
    io_error,  ///< Reading or writing failed, or no progress.
    busy,  ///< Too many concurrent streams or leases.
    bad_request,  ///< Malformed request.
    limit_exceeded,  ///< Frame or credit violation; fatal.
  };

  /**
   * @brief Get the wire name of an error code.
   *
   * @param error Error code.
   * @return Name used in ERROR frames and ACK statuses.
   */
  std::string_view to_string(error_e error);

  /**
   * @brief Parse a wire error name.
   *
   * @param name Name from an ERROR frame or ACK status.
   * @return The error code, or no value for an unknown name.
   */
  std::optional<error_e> error_from_string(std::string_view name);

  /**
   * @brief Wire status of a successful installation.
   */
  inline constexpr std::string_view status_installed = "installed";

  /**
   * @brief One decoded frame.
   */
  struct frame_t {
    std::uint16_t type = 0;  ///< Raw message type; may be unknown.
    std::uint16_t flags = 0;  ///< Frame flags.
    std::uint32_t stream_id = 0;  ///< Stream the frame belongs to.
    std::string payload;  ///< Frame payload.

    /**
     * @brief Check the END flag.
     *
     * @return `true` when this is the last frame of a response.
     */
    [[nodiscard]] bool end() const {
      return (flags & flag_end) != 0;
    }
  };

  /**
   * @brief Encode one frame.
   *
   * @param type Message type.
   * @param flags Frame flags.
   * @param stream_id Stream id.
   * @param payload Payload of at most `limit::max_payload` bytes.
   * @return Encoded header and payload.
   */
  std::string encode_frame(msg_e type, std::uint16_t flags, std::uint32_t stream_id, std::string_view payload);

  /**
   * @brief Incremental frame decoder for a byte stream.
   */
  class frame_decoder_t {
  public:
    /**
     * @brief Result of asking for the next frame.
     */
    enum class result_e {
      frame,  ///< A frame was decoded.
      need_more,  ///< More bytes are needed.
      bad_length,  ///< The stream carries an invalid frame length; it cannot be resynchronised.
    };

    /**
     * @brief Append received bytes.
     *
     * @param bytes Bytes read from the peer.
     */
    void feed(std::string_view bytes);

    /**
     * @brief Decode the next complete frame.
     *
     * @param frame Receives the frame when one is complete.
     * @return Whether a frame was decoded, more bytes are needed, or the stream is invalid.
     */
    result_e next(frame_t &frame);

    /**
     * @brief Get the number of buffered bytes not yet decoded.
     *
     * @return Buffered byte count.
     */
    [[nodiscard]] std::size_t buffered() const;

  private:
    std::string buffer;  ///< Received bytes not yet consumed.
    std::size_t offset = 0;  ///< Start of the undecoded bytes in `buffer`.
  };

  /**
   * @brief Directions and features one side allows.
   */
  struct policy_t {
    bool host_to_client = false;  ///< Host clipboard may reach the client.
    bool client_to_host = false;  ///< Client clipboard may reach the host.
    bool files = false;  ///< Files may be transferred.

    /**
     * @brief Intersect two policies.
     *
     * @param other Other side's policy.
     * @return Policy allowing only what both allow.
     */
    [[nodiscard]] policy_t operator&(const policy_t &other) const {
      return {host_to_client && other.host_to_client, client_to_host && other.client_to_host, files && other.files};
    }

    /**
     * @brief Compare two policies.
     *
     * @param other Policy to compare with.
     * @return `true` when both allow the same.
     */
    bool operator==(const policy_t &other) const = default;
  };

  /**
   * @brief Size limits in bytes.
   */
  struct limits_t {
    std::uint64_t max_item_size = 0;  ///< Largest non-file item.
    std::uint64_t max_transfer_size = 0;  ///< Largest file transfer.
  };

  /**
   * @brief HELLO message.
   */
  struct hello_t {
    std::uint64_t version = 0;  ///< Protocol version.
    std::string os;  ///< Client operating system.
    std::vector<std::string> formats;  ///< Canonical formats the client can send and receive.
    policy_t policy;  ///< What the client allows; missing fields are false.
    std::optional<std::uint64_t> max_item_size;  ///< Client's wish, if given.
    std::optional<std::uint64_t> max_transfer_size;  ///< Client's wish, if given.
  };

  /**
   * @brief HELLO_REPLY message.
   */
  struct hello_reply_t {
    std::uint64_t version = protocol_version;  ///< Protocol version.
    std::string os;  ///< Host operating system.
    std::vector<std::string> formats;  ///< Effective formats.
    policy_t policy;  ///< Effective policy.
    limits_t limits;  ///< Effective limits.
    bool host_clipboard = false;  ///< An ANNOUNCE of the host clipboard follows.
  };

  /**
   * @brief One announced format.
   */
  struct format_t {
    std::string mime;  ///< Canonical mime.
    std::optional<std::uint64_t> size;  ///< Size hint, if known.

    /**
     * @brief Compare two formats.
     *
     * @param other Format to compare with.
     * @return `true` when mime and size match.
     */
    bool operator==(const format_t &other) const = default;
  };

  /**
   * @brief ANNOUNCE message.
   */
  struct announce_t {
    std::uint64_t generation = 0;  ///< Sender's new generation.
    std::vector<format_t> formats;  ///< Available formats.
  };

  /**
   * @brief ANNOUNCE_ACK message.
   */
  struct announce_ack_t {
    std::uint64_t generation = 0;  ///< Acknowledged generation.
    std::string status;  ///< `installed` or an error code.
  };

  /**
   * @brief GET request.
   */
  struct get_t {
    std::uint64_t generation = 0;  ///< Requested generation.
    std::string mime;  ///< Requested format.
  };

  /**
   * @brief LOCK_REPLY message.
   */
  struct lock_reply_t {
    std::uint64_t lease = 0;  ///< Granted lease.
    std::uint64_t count = 0;  ///< Manifest entries.
    std::uint64_t total_bytes = 0;  ///< Summed file sizes.
  };

  /**
   * @brief GET_FILE request.
   */
  struct get_file_t {
    std::uint64_t lease = 0;  ///< Lease the file belongs to.
    std::uint64_t index = 0;  ///< Manifest index.
    std::uint64_t offset = 0;  ///< First byte.
    std::uint64_t length = 0;  ///< Bytes requested.
  };

  /**
   * @brief One manifest entry of GET_FILE_LIST.
   */
  struct file_entry_t {
    std::uint64_t index = 0;  ///< Position in the manifest.
    std::string path;  ///< Relative `/`-separated UTF-8 path.
    std::uint64_t size = 0;  ///< File size; 0 for directories.
    std::int64_t mtime = 0;  ///< Modification time in unix seconds.
    bool dir = false;  ///< Entry is a directory.

    /**
     * @brief Compare two entries.
     *
     * @param other Entry to compare with.
     * @return `true` when all fields match.
     */
    bool operator==(const file_entry_t &other) const = default;
  };

  /**
   * @brief ERROR message.
   */
  struct error_msg_t {
    std::string code;  ///< Error code name.
    std::string message;  ///< Human-readable detail.
  };

  /**
   * @brief Parse a payload as a JSON object.
   *
   * @param payload Frame payload.
   * @return The object, or no value when the payload is not a JSON object.
   */
  std::optional<nlohmann::json> parse_object(std::string_view payload);

  /**
   * @brief Read a required non-negative integer field.
   *
   * @param object JSON object.
   * @param key Field name.
   * @return The value, or no value when missing, mistyped, negative or above 2^53-1.
   */
  std::optional<std::uint64_t> get_count(const nlohmann::json &object, std::string_view key);

  /**
   * @brief Parse HELLO.
   *
   * @param payload Frame payload.
   * @return The message, or no value when malformed.
   */
  std::optional<hello_t> parse_hello(std::string_view payload);

  /**
   * @brief Parse HELLO_REPLY.
   *
   * @param payload Frame payload.
   * @return The message, or no value when malformed.
   */
  std::optional<hello_reply_t> parse_hello_reply(std::string_view payload);

  /**
   * @brief Parse ANNOUNCE.
   *
   * @param payload Frame payload.
   * @return The message, or no value when malformed.
   */
  std::optional<announce_t> parse_announce(std::string_view payload);

  /**
   * @brief Parse ANNOUNCE_ACK.
   *
   * @param payload Frame payload.
   * @return The message, or no value when malformed.
   */
  std::optional<announce_ack_t> parse_announce_ack(std::string_view payload);

  /**
   * @brief Parse GET.
   *
   * @param payload Frame payload.
   * @return The message, or no value when malformed.
   */
  std::optional<get_t> parse_get(std::string_view payload);

  /**
   * @brief Parse LOCK_REPLY.
   *
   * @param payload Frame payload.
   * @return The message, or no value when malformed.
   */
  std::optional<lock_reply_t> parse_lock_reply(std::string_view payload);

  /**
   * @brief Parse GET_FILE.
   *
   * @param payload Frame payload.
   * @return The message, or no value when malformed.
   */
  std::optional<get_file_t> parse_get_file(std::string_view payload);

  /**
   * @brief Parse ERROR.
   *
   * @param payload Frame payload.
   * @return The message, or no value when malformed.
   */
  std::optional<error_msg_t> parse_error(std::string_view payload);

  /**
   * @brief Parse a message whose payload is one required integer field.
   * @details Used for CLEAR, LOCK (`generation`), UNLOCK and GET_FILE_LIST (`lease`), CREDIT
   *          (`bytes`), PING and PONG (`nonce`).
   *
   * @param payload Frame payload.
   * @param key Field name.
   * @return The value, or no value when malformed.
   */
  std::optional<std::uint64_t> parse_single(std::string_view payload, std::string_view key);

  /**
   * @brief Parse one manifest line.
   *
   * @param line One NDJSON line without its newline.
   * @return The entry, or no value when malformed.
   */
  std::optional<file_entry_t> parse_file_entry(std::string_view line);

  /**
   * @brief Serialize HELLO.
   *
   * @param message Message to serialize.
   * @return JSON payload.
   */
  std::string serialize(const hello_t &message);

  /**
   * @brief Serialize HELLO_REPLY.
   *
   * @param message Message to serialize.
   * @return JSON payload.
   */
  std::string serialize(const hello_reply_t &message);

  /**
   * @brief Serialize ANNOUNCE.
   *
   * @param message Message to serialize.
   * @return JSON payload.
   */
  std::string serialize(const announce_t &message);

  /**
   * @brief Serialize ANNOUNCE_ACK.
   *
   * @param message Message to serialize.
   * @return JSON payload.
   */
  std::string serialize(const announce_ack_t &message);

  /**
   * @brief Serialize GET.
   *
   * @param message Message to serialize.
   * @return JSON payload.
   */
  std::string serialize(const get_t &message);

  /**
   * @brief Serialize LOCK_REPLY.
   *
   * @param message Message to serialize.
   * @return JSON payload.
   */
  std::string serialize(const lock_reply_t &message);

  /**
   * @brief Serialize GET_FILE.
   *
   * @param message Message to serialize.
   * @return JSON payload.
   */
  std::string serialize(const get_file_t &message);

  /**
   * @brief Serialize ERROR.
   *
   * @param message Message to serialize.
   * @return JSON payload.
   */
  std::string serialize(const error_msg_t &message);

  /**
   * @brief Serialize one manifest entry as an NDJSON line including its newline.
   *
   * @param entry Entry to serialize.
   * @return NDJSON line.
   */
  std::string serialize(const file_entry_t &entry);

  /**
   * @brief Serialize a message whose payload is one integer field.
   *
   * @param key Field name.
   * @param value Field value.
   * @return JSON payload.
   */
  std::string serialize_single(std::string_view key, std::uint64_t value);
}  // namespace clipboard
