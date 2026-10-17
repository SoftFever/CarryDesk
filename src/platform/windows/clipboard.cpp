/**
 * @file src/platform/windows/clipboard.cpp
 * @brief Definitions for the Windows clipboard backend.
 * @details A thread owns a message-only window that listens for clipboard updates and puts
 *          Sunshine's content on the clipboard; other threads hand it tasks through its message
 *          queue. Sunshine's content is always rendered at once, never on request, so pasting never
 *          waits on Sunshine and no message has to reach Sunshine's window from the user's
 *          applications, which run at a lower integrity level. Reads open the clipboard on the
 *          caller's thread, so an application slow to render its own content never stalls the window.
 *
 *          Running as SYSTEM, Sunshine sees only the predefined text and bitmap formats of the user's
 *          content; HTML, RTF, PNG and file lists are hidden from it. The backend then inspects and
 *          reads through an agent: `sunshine.exe --clipboard-agent` running as the logged-on user,
 *          which answers requests on its standard input and output.
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

// platform includes
#include <Windows.h>
#include <shellapi.h>
#include <userenv.h>
#include <wincodec.h>
#include <wrl/client.h>

// local includes
#include "clipboard.h"
#include "clipboard_files.h"
#include "src/clipboard/formats.h"
#include "src/clipboard/protocol.h"
#include "src/logging.h"

using namespace std::literals;
using Microsoft::WRL::ComPtr;

namespace clipboard::win {
  namespace {
    namespace fmt = clipboard::formats;

    constexpr auto task_timeout = 5s;  ///< Longest wait for the window thread to run a task.
    constexpr int open_attempts = 50;  ///< Tries to open the clipboard while another application holds it.
    constexpr auto open_retry = 10ms;  ///< Pause between those tries.
    constexpr UINT task_message = WM_APP + 1;  ///< Runs queued tasks.
    constexpr UINT check_message = WM_APP + 2;  ///< Inspects the clipboard as if it had changed.
    constexpr UINT quit_message = WM_APP + 3;  ///< Ends the message loop.
    constexpr UINT_PTR poll_timer = 1;  ///< Timer that catches changes whose notification was missed.
    constexpr UINT poll_interval_ms = 500;  ///< Interval of that timer.
    constexpr std::uint32_t max_dimension = 16384;  ///< Largest image width or height.
    constexpr std::uint64_t max_decoded = 256ULL * 1048576;  ///< Largest decoded image.
    constexpr std::uint64_t max_bitmap = max_decoded + 65536;  ///< Largest bitmap format read.

    using clock = std::chrono::steady_clock;

    /**
     * @brief A predefined clipboard format and the name it goes by.
     */
    struct predefined_t {
      std::string_view name;  ///< Name of the `CF_` constant.
      UINT id;  ///< Format id.
    };

    constexpr std::array<predefined_t, 17> predefined {{
      {"CF_TEXT", CF_TEXT},
      {"CF_BITMAP", CF_BITMAP},
      {"CF_METAFILEPICT", CF_METAFILEPICT},
      {"CF_SYLK", CF_SYLK},
      {"CF_DIF", CF_DIF},
      {"CF_TIFF", CF_TIFF},
      {"CF_OEMTEXT", CF_OEMTEXT},
      {"CF_DIB", CF_DIB},
      {"CF_PALETTE", CF_PALETTE},
      {"CF_PENDATA", CF_PENDATA},
      {"CF_RIFF", CF_RIFF},
      {"CF_WAVE", CF_WAVE},
      {"CF_UNICODETEXT", CF_UNICODETEXT},
      {"CF_ENHMETAFILE", CF_ENHMETAFILE},
      {"CF_HDROP", CF_HDROP},
      {"CF_LOCALE", CF_LOCALE},
      {"CF_DIBV5", CF_DIBV5},
    }};

    /**
     * @brief Get the id of a format by name, registering it when needed.
     *
     * @param name Format name.
     * @return Format id, or 0 on failure.
     */
    UINT format_id(const std::string_view name) {
      for (const auto &[predefined_name, id] : predefined) {
        if (predefined_name == name) {
          return id;
        }
      }
      const auto wide = widen(name);
      return wide ? RegisterClipboardFormatW(wide->c_str()) : 0;
    }

    /**
     * @brief Get the name of a format.
     *
     * @param id Format id.
     * @return Registered name, `CF_` name, or `CF_` and the number.
     */
    std::string format_name(const UINT id) {
      for (const auto &[name, predefined_id] : predefined) {
        if (predefined_id == id) {
          return std::string {name};
        }
      }
      std::array<wchar_t, 256> buffer {};
      if (const auto size = GetClipboardFormatNameW(id, buffer.data(), static_cast<int>(buffer.size())); size > 0) {
        if (auto name = narrow({buffer.data(), static_cast<std::size_t>(size)})) {
          return *name;
        }
      }
      return "CF_" + std::to_string(id);
    }

    /**
     * @brief Holds the clipboard open for its lifetime.
     */
    class open_clipboard_t {
    public:
      /**
       * @brief Open the clipboard, retrying while another application holds it.
       *
       * @param owner Window to associate, or null to read only.
       */
      explicit open_clipboard_t(HWND owner) {
        for (int attempt = 0; attempt < open_attempts; ++attempt) {
          if (OpenClipboard(owner)) {
            open_ = true;
            return;
          }
          std::this_thread::sleep_for(open_retry);
        }
      }

      ~open_clipboard_t() {
        if (open_) {
          CloseClipboard();
        }
      }

      open_clipboard_t(const open_clipboard_t &) = delete;
      open_clipboard_t &operator=(const open_clipboard_t &) = delete;

      /**
       * @brief Check whether the clipboard is open.
       *
       * @return `true` when it is.
       */
      explicit operator bool() const {
        return open_;
      }

    private:
      bool open_ = false;  ///< `CloseClipboard` is due.
    };

    /**
     * @brief Copy a memory format of the open clipboard.
     *
     * @param format Format id.
     * @param max_size Largest acceptable size.
     * @param bytes Receives the bytes.
     * @return `ok`, `too_large`, or `io_error` when the format is missing or not in memory.
     */
    platf::clipboard_result_e get_data(const UINT format, const std::uint64_t max_size, std::string &bytes) {
      const auto handle = GetClipboardData(format);
      if (!handle) {
        return platf::clipboard_result_e::io_error;
      }
      const auto size = GlobalSize(handle);
      if (size > max_size) {
        return platf::clipboard_result_e::too_large;
      }
      const auto *data = GlobalLock(handle);
      if (!data) {
        return platf::clipboard_result_e::io_error;
      }
      bytes.assign(static_cast<const char *>(data), size);
      GlobalUnlock(handle);
      return platf::clipboard_result_e::ok;
    }

    /**
     * @brief List the formats of the open clipboard in clipboard order.
     * @details Windows hides most formats of other users' content from a process running as SYSTEM,
     *          and `EnumClipboardFormats` then stops at the first hidden one.
     *          `GetUpdatedClipboardFormats` lists every format the process can read, so it comes first
     *          and enumeration only adds what it left out.
     *
     * @return Format names.
     */
    std::vector<std::string> format_names() {
      std::vector<UINT> ids(64);
      UINT count = 0;
      while (!GetUpdatedClipboardFormats(ids.data(), static_cast<UINT>(ids.size()), &count)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || ids.size() >= 4096) {
          count = 0;
          break;
        }
        ids.resize(ids.size() * 2);
      }
      ids.resize(std::min<std::size_t>(count, ids.size()));
      for (auto format = EnumClipboardFormats(0); format; format = EnumClipboardFormats(format)) {
        if (std::ranges::find(ids, format) == ids.end()) {
          ids.push_back(format);
        }
      }
      std::vector<std::string> names;
      for (const auto id : ids) {
        names.push_back(format_name(id));
      }
      return names;
    }

    /**
     * @brief Check whether the open clipboard holds a `DWORD` format set to 0.
     *
     * @param name Format name.
     * @return `true` when the format is there with the value 0.
     */
    bool dword_is_zero(const std::string_view name) {
      std::string bytes;
      const auto format = format_id(name);
      return format && IsClipboardFormatAvailable(format) && get_data(format, 64, bytes) == platf::clipboard_result_e::ok && bytes.size() >= 4 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0;
    }

    /**
     * @brief Read the paths of a `DROPFILES` that holds them in the ANSI code page.
     *
     * @param bytes `DROPFILES` data.
     * @return UTF-8 paths.
     */
    std::vector<std::string> ansi_drop_paths(const std::string_view bytes) {
      std::vector<std::string> paths;
      std::uint32_t offset = 0;
      if (bytes.size() < 20) {
        return paths;
      }
      std::memcpy(&offset, bytes.data(), sizeof(offset));
      for (auto rest = offset >= 20 && offset < bytes.size() ? bytes.substr(offset) : std::string_view {}; !rest.empty();) {
        const auto end = rest.find('\0');
        const auto path = rest.substr(0, end);
        if (path.empty()) {
          break;
        }
        std::wstring wide(static_cast<std::size_t>(MultiByteToWideChar(CP_ACP, 0, path.data(), static_cast<int>(path.size()), nullptr, 0)), L'\0');
        MultiByteToWideChar(CP_ACP, 0, path.data(), static_cast<int>(path.size()), wide.data(), static_cast<int>(wide.size()));
        if (auto utf8 = narrow(wide)) {
          paths.push_back(std::move(*utf8));
        }
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
      }
      return paths;
    }

    /**
     * @brief What the clipboard holds.
     */
    struct inspection_t {
      DWORD sequence = 0;  ///< Clipboard sequence number of the content.
      std::vector<std::string> names;  ///< Format names in clipboard order.
      bool opted_out = false;  ///< The content opted out of the clipboard history or the cloud clipboard.
    };

    /**
     * @brief Inspect the clipboard, which the calling thread holds open.
     *
     * @return What it holds.
     */
    inspection_t inspect_open_clipboard() {
      inspection_t inspection;
      inspection.sequence = GetClipboardSequenceNumber();
      inspection.names = format_names();
      inspection.opted_out = dword_is_zero(fmt::windows::history_opt_in) || dword_is_zero(fmt::windows::cloud_opt_in);
      return inspection;
    }

    /**
     * @brief One format read from the clipboard.
     */
    struct fetched_t {
      platf::clipboard_result_e result = platf::clipboard_result_e::io_error;  ///< Outcome of the read.
      DWORD sequence = 0;  ///< Clipboard sequence number while reading.
      std::string bytes;  ///< Bytes of the format.
    };

    /**
     * @brief Read one format of the clipboard on the calling thread.
     *
     * @param name Format name.
     * @param max_size Largest acceptable size.
     * @return Outcome, sequence number and bytes.
     */
    fetched_t fetch_clipboard(const std::string_view name, const std::uint64_t max_size) {
      fetched_t fetched;
      const open_clipboard_t clipboard {nullptr};
      if (clipboard) {
        fetched.sequence = GetClipboardSequenceNumber();
        fetched.result = get_data(format_id(name), max_size, fetched.bytes);
      }
      return fetched;
    }

    constexpr auto agent_stall = 5s;  ///< Longest time an agent's reply may go without progress.
    constexpr auto agent_retry = 5s;  ///< Shortest time between attempts to start an agent.
    constexpr char op_inspect = 1;  ///< Request: inspect the clipboard.
    constexpr char op_fetch = 2;  ///< Request: read one format, with a u64 size limit and the format name.
    constexpr std::uint32_t max_message = 0x7FFFFFFF;  ///< Largest message on the agent's pipes.

    /**
     * @brief Append a little-endian value.
     *
     * @param out Output bytes.
     * @param value Value.
     * @param size Byte count.
     */
    void append_le(std::string &out, const std::uint64_t value, const std::size_t size) {
      for (std::size_t i = 0; i < size; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
      }
    }

    /**
     * @brief Read a little-endian value.
     *
     * @param bytes Source with at least `offset + size` bytes.
     * @param offset Byte offset.
     * @param size Byte count.
     * @return Value.
     */
    std::uint64_t read_le(const std::string_view bytes, const std::size_t offset, const std::size_t size) {
      std::uint64_t value = 0;
      for (std::size_t i = 0; i < size; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
      }
      return value;
    }

    /**
     * @brief Write bytes to a pipe.
     *
     * @param pipe Pipe.
     * @param bytes Bytes.
     * @return `false` when the pipe broke.
     */
    bool write_pipe(HANDLE pipe, std::string_view bytes) {
      while (!bytes.empty()) {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes.data(), static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1 << 20)), &written, nullptr) || written == 0) {
          return false;
        }
        bytes.remove_prefix(written);
      }
      return true;
    }

    /**
     * @brief Read an exact number of bytes from a pipe.
     *
     * @param pipe Pipe.
     * @param data Destination.
     * @param size Byte count.
     * @param stall Longest time without progress; blocks without limit when absent.
     * @return `false` when the pipe broke or stalled.
     */
    bool read_pipe(HANDLE pipe, char *data, std::size_t size, const std::optional<std::chrono::milliseconds> stall) {
      auto deadline = clock::now() + stall.value_or(0ms);
      while (size > 0) {
        auto chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1 << 20));
        if (stall) {
          DWORD available = 0;
          if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            return false;
          }
          if (available == 0) {
            if (clock::now() >= deadline) {
              return false;
            }
            std::this_thread::sleep_for(1ms);
            continue;
          }
          chunk = std::min(chunk, available);
        }
        DWORD read = 0;
        if (!ReadFile(pipe, data, chunk, &read, nullptr) || read == 0) {
          return false;
        }
        data += read;
        size -= read;
        deadline = clock::now() + stall.value_or(0ms);
      }
      return true;
    }

    /**
     * @brief Write one length-prefixed message.
     *
     * @param pipe Pipe.
     * @param body Message.
     * @return `false` when the pipe broke.
     */
    bool write_message(HANDLE pipe, const std::string_view body) {
      std::string header;
      append_le(header, body.size(), 4);
      return body.size() <= max_message && write_pipe(pipe, header) && write_pipe(pipe, body);
    }

    /**
     * @brief Read one length-prefixed message.
     *
     * @param pipe Pipe.
     * @param stall Longest time without progress; blocks without limit when absent.
     * @return Message, or no value when the pipe broke, stalled or carried a bad length.
     */
    std::optional<std::string> read_message(HANDLE pipe, const std::optional<std::chrono::milliseconds> stall) {
      char header[4];
      if (!read_pipe(pipe, header, sizeof(header), stall)) {
        return std::nullopt;
      }
      const auto size = read_le({header, sizeof(header)}, 0, 4);
      if (size > max_message) {
        return std::nullopt;
      }
      std::string body(static_cast<std::size_t>(size), '\0');
      if (!read_pipe(pipe, body.data(), body.size(), stall)) {
        return std::nullopt;
      }
      return body;
    }

    /**
     * @brief Asks a clipboard agent to inspect and read the clipboard, starting one when needed.
     * @details Calls are serialized. An agent that breaks or stops answering is ended, and another is
     *          started at the next call, at most once per retry interval.
     */
    class agent_client_t {
    public:
      /**
       * @brief Prepare a client.
       *
       * @param factory Starts an agent.
       */
      explicit agent_client_t(agent_factory_t factory):
          factory_ {std::move(factory)} {
      }

      ~agent_client_t() {
        std::lock_guard lock {mutex_};
        disconnect();
      }

      agent_client_t(const agent_client_t &) = delete;
      agent_client_t &operator=(const agent_client_t &) = delete;

      /**
       * @brief Inspect the clipboard through the agent.
       *
       * @return What it holds, or no value when no agent answered.
       */
      std::optional<inspection_t> inspect() {
        const auto reply = call(std::string(1, op_inspect));
        if (!reply || reply->size() < 6 || static_cast<platf::clipboard_result_e>((*reply)[0]) != platf::clipboard_result_e::ok) {
          return std::nullopt;
        }
        inspection_t inspection;
        inspection.sequence = static_cast<DWORD>(read_le(*reply, 1, 4));
        inspection.opted_out = (*reply)[5] != 0;
        for (std::string_view names = std::string_view {*reply}.substr(6); !names.empty();) {
          const auto end = names.find('\0');
          inspection.names.emplace_back(names.substr(0, end));
          names.remove_prefix(end == std::string_view::npos ? names.size() : end + 1);
        }
        return inspection;
      }

      /**
       * @brief Read one format through the agent.
       *
       * @param name Format name.
       * @param max_size Largest acceptable size.
       * @return Outcome, sequence number and bytes, or no value when no agent answered.
       */
      std::optional<fetched_t> fetch(const std::string_view name, const std::uint64_t max_size) {
        std::string request(1, op_fetch);
        append_le(request, max_size, 8);
        request += name;
        const auto reply = call(request);
        if (!reply || reply->size() < 5) {
          return std::nullopt;
        }
        fetched_t fetched;
        fetched.result = static_cast<platf::clipboard_result_e>((*reply)[0]);
        fetched.sequence = static_cast<DWORD>(read_le(*reply, 1, 4));
        fetched.bytes = reply->substr(5);
        return fetched;
      }

    private:
      /**
       * @brief Send a request and wait for its reply.
       *
       * @param request Request message.
       * @return Reply message, or no value when no agent answered.
       */
      std::optional<std::string> call(const std::string &request) {
        std::lock_guard lock {mutex_};
        if (!requests_ && !connect()) {
          return std::nullopt;
        }
        if (write_message(requests_.get(), request)) {
          if (auto reply = read_message(replies_.get(), agent_stall)) {
            return reply;
          }
        }
        BOOST_LOG(warning) << "[clipboard] The clipboard agent stopped answering"sv;
        disconnect();
        return std::nullopt;
      }

      /**
       * @brief Start an agent unless an attempt was made too recently.
       *
       * @return `true` when an agent runs.
       */
      bool connect() {
        const auto now = clock::now();
        if (now < retry_after_) {
          return false;
        }
        retry_after_ = now + agent_retry;
        auto link = factory_();
        if (!link) {
          return false;
        }
        requests_.reset(link->requests);
        replies_.reset(link->replies);
        close_ = std::move(link->close);
        BOOST_LOG(info) << "[clipboard] Reading the clipboard through an agent in the user's session"sv;
        return true;
      }

      /**
       * @brief Close the pipes and end the agent.
       */
      void disconnect() {
        requests_.reset();
        replies_.reset();
        if (close_) {
          close_();
          close_ = nullptr;
        }
      }

      agent_factory_t factory_;  ///< Starts an agent.
      std::mutex mutex_;  ///< Serializes calls.
      handle_t requests_;  ///< Pipe to the agent.
      handle_t replies_;  ///< Pipe from the agent.
      std::function<void()> close_;  ///< Ends the running agent.
      clock::time_point retry_after_ {};  ///< Earliest time of the next start attempt.
    };

    /**
     * @brief Initializes COM on the calling thread for its lifetime when it is not already.
     */
    class com_scope_t {
    public:
      com_scope_t():
          result_ {CoInitializeEx(nullptr, COINIT_MULTITHREADED)} {
      }

      ~com_scope_t() {
        if (SUCCEEDED(result_)) {
          CoUninitialize();
        }
      }

      com_scope_t(const com_scope_t &) = delete;
      com_scope_t &operator=(const com_scope_t &) = delete;

    private:
      HRESULT result_;  ///< Result of the initialization; a thread in another apartment keeps it.
    };

    /**
     * @brief Decoded image: top-down rows of 32-bit BGRA with straight alpha.
     */
    struct image_t {
      std::uint32_t width = 0;  ///< Width in pixels.
      std::uint32_t height = 0;  ///< Height in pixels.
      std::vector<std::uint8_t> pixels;  ///< Pixel bytes.
    };

    /**
     * @brief Create a WIC factory.
     *
     * @return Factory, or null.
     */
    ComPtr<IWICImagingFactory> wic_factory() {
      ComPtr<IWICImagingFactory> factory;
      if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return nullptr;
      }
      return factory;
    }

    /**
     * @brief Decode an image file in memory.
     *
     * @param factory WIC factory.
     * @param bytes Encoded image.
     * @return Image, or no value when it cannot be decoded or exceeds the limits.
     */
    std::optional<image_t> decode(IWICImagingFactory *factory, const std::string_view bytes) {
      ComPtr<IWICStream> stream;
      ComPtr<IWICBitmapDecoder> decoder;
      ComPtr<IWICBitmapFrameDecode> frame;
      ComPtr<IWICFormatConverter> converter;
      UINT width = 0;
      UINT height = 0;
      if (bytes.size() > 0xFFFFFFFF || FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE *>(const_cast<char *>(bytes.data())), static_cast<DWORD>(bytes.size()))) || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) || FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height))) {
        return std::nullopt;
      }
      if (width == 0 || height == 0 || width > max_dimension || height > max_dimension || std::uint64_t {width} * height * 4 > max_decoded) {
        return std::nullopt;
      }
      image_t image {width, height, std::vector<std::uint8_t>(std::size_t {width} * height * 4)};
      if (FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) || FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.pixels.size()), image.pixels.data()))) {
        return std::nullopt;
      }
      return image;
    }

    /**
     * @brief Encode an image as PNG, without an alpha channel when it is fully opaque.
     *
     * @param factory WIC factory.
     * @param image Image to encode.
     * @return PNG bytes, or no value on failure.
     */
    std::optional<std::string> encode_png(IWICImagingFactory *factory, image_t &image) {
      bool opaque = true;
      for (std::size_t i = 3; i < image.pixels.size() && opaque; i += 4) {
        opaque = image.pixels[i] == 0xFF;
      }
      ComPtr<IStream> stream;
      ComPtr<IWICBitmapEncoder> encoder;
      ComPtr<IWICBitmapFrameEncode> frame;
      ComPtr<IPropertyBag2> properties;
      ComPtr<IWICBitmap> bitmap;
      ComPtr<IWICFormatConverter> converter;
      if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, &properties)) || FAILED(frame->Initialize(properties.Get())) || FAILED(frame->SetSize(image.width, image.height)) || FAILED(factory->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppBGRA, image.width * 4, static_cast<UINT>(image.pixels.size()), image.pixels.data(), &bitmap))) {
        return std::nullopt;
      }
      ComPtr<IWICBitmapSource> source = bitmap;
      WICPixelFormatGUID format = opaque ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
      if (opaque) {
        if (FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
          return std::nullopt;
        }
        source = converter;
      }
      STATSTG stat {};
      HGLOBAL global = nullptr;
      if (FAILED(frame->SetPixelFormat(&format)) || FAILED(frame->WriteSource(source.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()) || FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.Get(), &global))) {
        return std::nullopt;
      }
      const auto *data = GlobalLock(global);
      if (!data) {
        return std::nullopt;
      }
      std::string png {static_cast<const char *>(data), static_cast<std::size_t>(stat.cbSize.QuadPart)};
      GlobalUnlock(global);
      return png;
    }

    /**
     * @brief Progress of a task handed to the window thread.
     */
    struct task_state_t {
      /**
       * @brief Task progress.
       */
      enum class step_e {
        queued,  ///< Waiting in the queue.
        running,  ///< Running on the window thread.
        finished,  ///< Done; its results may be read.
        abandoned,  ///< The caller gave up; the task will not run.
      };

      std::mutex mutex;  ///< Guards `step`.
      std::condition_variable done;  ///< Signalled when the task finished.
      step_e step = step_e::queued;  ///< Current progress.
    };

    /**
     * @brief Clipboard backend on the Win32 clipboard.
     */
    class windows_clipboard_t: public platf::clipboard_t {
    public:
      /**
       * @brief Prepare a backend.
       *
       * @param desktop Desktop the window thread joins; empty for the process's own.
       * @param agent Starts an agent that inspects and reads the clipboard; empty to do it here.
       */
      windows_clipboard_t(std::wstring desktop, agent_factory_t agent):
          desktop_ {std::move(desktop)},
          agent_ {agent ? std::make_unique<agent_client_t>(std::move(agent)) : nullptr} {
      }

      ~windows_clipboard_t() override {
        stop();
      }

      bool start(platf::clipboard_observer_t &observer) override {
        observer_ = &observer;
        auto ready = std::make_shared<std::promise<bool>>();
        auto started = ready->get_future();
        thread_ = std::thread([this, ready]() {
          run(*ready);
        });
        if (!started.get()) {
          thread_.join();
          return false;
        }
        started_ = true;
        BOOST_LOG(info) << "[clipboard] Using the Windows clipboard"sv;
        return true;
      }

      void stop() override {
        if (!started_.exchange(false)) {
          return;
        }
        run_task([this]() {
          release_locked();
        });
        {
          std::lock_guard lock {tasks_mutex_};
          stopping_ = true;
        }
        PostMessageW(window_, quit_message, 0, 0);
        thread_.join();
      }

      [[nodiscard]] std::vector<std::string> formats() const override {
        return {std::string {mime::text}, std::string {mime::html}, std::string {mime::rtf}, std::string {mime::png}, std::string {mime::files}};
      }

      platf::clipboard_read_t read(const std::uint64_t revision, const std::string &mime, const std::uint64_t max_size) override {
        platf::clipboard_read_t result;
        result.result = platf::clipboard_result_e::stale;
        fmt::windows_offer_t offer;
        DWORD sequence = 0;
        {
          std::lock_guard lock {state_mutex_};
          if (revision != revision_) {
            return result;
          }
          offer = offer_;
          sequence = sequence_;
        }
        const auto format = std::ranges::find(offer.formats, mime, &fmt::native_format_t::canonical);
        if (offer.sensitive || format == offer.formats.end()) {
          return result;
        }

        const auto bitmap = format->native == fmt::windows::dib || format->native == fmt::windows::dibv5;
        const auto files = mime == clipboard::mime::files;
        std::uint64_t native_limit = max_size;
        if (bitmap) {
          native_limit = max_bitmap;
        } else if (mime == clipboard::mime::text || mime == clipboard::mime::html) {
          // UTF-16 text is at most twice its UTF-8; CF_HTML adds a header.
          native_limit = std::min(max_size, std::uint64_t {1} << 40) * 2 + 4096;
        } else if (!files) {
          native_limit = std::min(max_size, std::uint64_t {1} << 40) + 4096;
        }

        auto fetched = agent_ ? agent_->fetch(format->native, native_limit) : std::nullopt;
        if (!fetched) {
          fetched = fetch_clipboard(format->native, native_limit);
        }
        if (fetched->sequence != sequence) {
          return result;
        }
        result.result = fetched->result;
        const auto &bytes = fetched->bytes;
        if (files && result.result == platf::clipboard_result_e::ok) {
          if (auto paths = fmt::paths_from_drop_files(bytes)) {
            result.paths = std::move(*paths);
          } else {
            result.paths = ansi_drop_paths(bytes);
          }
        }
        if (revision != revision_) {
          result.result = platf::clipboard_result_e::stale;
        }
        if (result.result != platf::clipboard_result_e::ok || files) {
          return result;
        }

        if (bitmap) {
          auto png = png_from_bitmap(bytes);
          if (!png) {
            BOOST_LOG(warning) << "[clipboard] Cannot convert the host's bitmap to PNG"sv;
            result.result = platf::clipboard_result_e::io_error;
            return result;
          }
          result.bytes = std::move(*png);
        } else {
          result.bytes = fmt::windows_to_canonical(mime, bytes);
        }
        if (result.bytes.size() > max_size) {
          result.bytes.clear();
          result.result = platf::clipboard_result_e::too_large;
        }
        return result;
      }

      platf::clipboard_result_e take_ownership(const std::uint64_t expected_revision, const std::map<std::string, std::string, std::less<>> &content, const std::vector<std::string> &file_paths) override {
        auto offers = fmt::windows_native_offers(content, file_paths);
        if (const auto png = content.find(mime::png); png != content.end()) {
          if (auto bitmaps = bitmaps_from_png(png->second)) {
            const auto position = std::ranges::find(offers, std::string {fmt::windows::png}, &fmt::native_offers_t::value_type::first);
            const auto after = position == offers.end() ? offers.end() : position + 1;
            offers.insert(after, {{std::string {fmt::windows::dibv5}, std::make_shared<const std::string>(std::move(bitmaps->first))}, {std::string {fmt::windows::dib}, std::make_shared<const std::string>(std::move(bitmaps->second))}});
          } else {
            BOOST_LOG(warning) << "[clipboard] Cannot convert the client's image for applications that need a bitmap"sv;
          }
        }

        auto status = platf::clipboard_result_e::io_error;
        run_task([this, expected_revision, &offers, &status]() {
          if (expected_revision != revision_) {
            status = platf::clipboard_result_e::stale;
            return;
          }
          const open_clipboard_t clipboard {window_};
          if (!clipboard) {
            BOOST_LOG(warning) << "[clipboard] Another application keeps the clipboard open"sv;
            return;
          }
          if (GetClipboardSequenceNumber() != last_sequence_ && GetClipboardOwner() != window_) {
            // Another application changed the clipboard; its notification is still on the way.
            status = platf::clipboard_result_e::stale;
            return;
          }
          if (!EmptyClipboard()) {
            return;
          }
          bool complete = true;
          for (const auto &[name, bytes] : offers) {
            if (!set_data(name, *bytes)) {
              complete = false;
              break;
            }
          }
          if (!complete) {
            EmptyClipboard();
          }
          last_sequence_ = GetClipboardSequenceNumber();
          status = complete ? platf::clipboard_result_e::ok : platf::clipboard_result_e::io_error;
        });
        return status;
      }

      void release() override {
        run_task([this]() {
          release_locked();
        });
      }

    private:
      /**
       * @brief Put one format on the open clipboard.
       *
       * @param name Format name.
       * @param bytes Format bytes.
       * @return `false` on failure.
       */
      static bool set_data(const std::string &name, const std::string &bytes) {
        const auto format = format_id(name);
        const auto memory = format ? GlobalAlloc(GMEM_MOVEABLE, std::max<std::size_t>(bytes.size(), 1)) : nullptr;
        if (!memory) {
          return false;
        }
        auto *data = GlobalLock(memory);
        if (!data) {
          GlobalFree(memory);
          return false;
        }
        std::memcpy(data, bytes.data(), bytes.size());
        GlobalUnlock(memory);
        if (!SetClipboardData(format, memory)) {
          GlobalFree(memory);
          return false;
        }
        return true;
      }

      /**
       * @brief Empty the clipboard if Sunshine still owns it; runs on the window thread.
       */
      void release_locked() {
        if (!window_ || GetClipboardOwner() != window_) {
          return;
        }
        const open_clipboard_t clipboard {window_};
        if (clipboard && GetClipboardOwner() == window_) {
          EmptyClipboard();
          last_sequence_ = GetClipboardSequenceNumber();
        }
      }

      /**
       * @brief Inspect the clipboard after a change and report it unless Sunshine made it.
       *
       * @param initial Report the clipboard as it is at start, whatever its sequence number.
       */
      void handle_change(const bool initial = false) {
        std::optional<inspection_t> inspection;
        {
          const open_clipboard_t clipboard {window_};
          if (!clipboard) {
            // The poll timer tries again.
            return;
          }
          const auto sequence = GetClipboardSequenceNumber();
          if (sequence == last_sequence_ && !initial) {
            return;
          }
          last_sequence_ = sequence;
          if (GetClipboardOwner() == window_ && !initial) {
            // Sunshine's own content, or the empty clipboard it left after releasing it.
            return;
          }
          if (!agent_) {
            inspection = inspect_open_clipboard();
          }
        }
        if (agent_) {
          // The agent needs the clipboard, so it is asked once Sunshine closed it.
          inspection = agent_->inspect();
          if (!inspection) {
            const open_clipboard_t clipboard {window_};
            if (!clipboard) {
              last_sequence_ = 0;
              return;
            }
            inspection = inspect_open_clipboard();
          }
        }
        const auto sequence = inspection->sequence;
        const auto &names = inspection->names;
        const auto opted_out = inspection->opted_out;
        auto offer = fmt::map_windows_offer(names, opted_out);
        std::vector<std::string> formats;
        if (!offer.sensitive) {
          for (const auto &format : offer.formats) {
            formats.push_back(format.canonical);
          }
        }
        std::uint64_t revision = 0;
        {
          std::lock_guard lock {state_mutex_};
          revision = ++revision_;
          sequence_ = sequence;
          offer_ = std::move(offer);
        }
        observer_->on_clipboard_changed(revision, std::move(formats));
      }

      /**
       * @brief Run a task on the window thread and wait for it.
       * @details A task the thread has not started within the timeout is abandoned and never runs,
       *          so a caller that gave up can neither race the task's results nor be surprised by its
       *          effects later. A task that has started is waited for.
       *
       * @param task Task to run.
       * @return `false` when the task was abandoned or the thread is stopping.
       */
      bool run_task(std::function<void()> task) {
        if (std::this_thread::get_id() == thread_id_) {
          task();
          return true;
        }
        auto state = std::make_shared<task_state_t>();
        {
          std::lock_guard lock {tasks_mutex_};
          if (stopping_ || !window_) {
            return false;
          }
          tasks_.push_back([task = std::move(task), state]() {
            {
              std::lock_guard lock {state->mutex};
              if (state->step == task_state_t::step_e::abandoned) {
                return;
              }
              state->step = task_state_t::step_e::running;
            }
            task();
            {
              std::lock_guard lock {state->mutex};
              state->step = task_state_t::step_e::finished;
            }
            state->done.notify_all();
          });
        }
        PostMessageW(window_, task_message, 0, 0);
        std::unique_lock lock {state->mutex};
        const auto finished = [&state]() {
          return state->step == task_state_t::step_e::finished;
        };
        if (state->done.wait_for(lock, task_timeout, finished)) {
          return true;
        }
        if (state->step == task_state_t::step_e::queued) {
          state->step = task_state_t::step_e::abandoned;
          BOOST_LOG(warning) << "[clipboard] The clipboard window did not respond in time"sv;
          return false;
        }
        state->done.wait(lock, finished);
        return true;
      }

      /**
       * @brief Run queued tasks on the window thread.
       */
      void run_tasks() {
        std::deque<std::function<void()>> tasks;
        {
          std::lock_guard lock {tasks_mutex_};
          tasks.swap(tasks_);
        }
        for (auto &task : tasks) {
          task();
        }
      }

      /**
       * @brief Window thread body: create the window, then dispatch messages until stopped.
       *
       * @param ready Receives whether the window is listening.
       */
      void run(std::promise<bool> &ready) {
        platf::set_thread_name("clipboard-win");
        HDESK desktop = nullptr;
        if (!desktop_.empty()) {
          desktop = OpenDesktopW(desktop_.c_str(), 0, FALSE, GENERIC_ALL);
          if (!desktop || !SetThreadDesktop(desktop)) {
            BOOST_LOG(warning) << "[clipboard] Cannot join the clipboard desktop: "sv << GetLastError();
            if (desktop) {
              CloseDesktop(desktop);
            }
            ready.set_value(false);
            return;
          }
        }
        const auto instance = GetModuleHandleW(nullptr);
        const auto class_name = L"SunshineClipboard" + std::to_wstring(reinterpret_cast<std::uintptr_t>(this));
        WNDCLASSEXW window_class {};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &windows_clipboard_t::window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = class_name.c_str();
        HWND window = nullptr;
        if (RegisterClassExW(&window_class)) {
          window = CreateWindowExW(0, class_name.c_str(), L"Sunshine clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        }
        if (!window || !AddClipboardFormatListener(window)) {
          BOOST_LOG(warning) << "[clipboard] Cannot listen to the Windows clipboard: "sv << GetLastError();
          if (window) {
            DestroyWindow(window);
          }
          UnregisterClassW(class_name.c_str(), instance);
          if (desktop) {
            CloseDesktop(desktop);
          }
          ready.set_value(false);
          return;
        }
        ChangeWindowMessageFilterEx(window, WM_CLIPBOARDUPDATE, MSGFLT_ALLOW, nullptr);
        SetTimer(window, poll_timer, poll_interval_ms, nullptr);
        thread_id_ = std::this_thread::get_id();
        {
          std::lock_guard lock {tasks_mutex_};
          window_ = window;
        }
        // The clipboard's content at start is reported like a change, once the loop runs.
        PostMessageW(window, check_message, 0, 0);
        ready.set_value(true);

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
          DispatchMessageW(&message);
        }

        {
          std::lock_guard lock {tasks_mutex_};
          stopping_ = true;
        }
        run_tasks();
        release_locked();
        KillTimer(window, poll_timer);
        RemoveClipboardFormatListener(window);
        DestroyWindow(window);
        UnregisterClassW(class_name.c_str(), instance);
        if (desktop) {
          CloseDesktop(desktop);
        }
      }

      /**
       * @brief Window procedure of the message-only window.
       *
       * @param window Window.
       * @param message Message.
       * @param wparam Message parameter.
       * @param lparam Message parameter.
       * @return Message result.
       */
      static LRESULT CALLBACK window_proc(HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) {
        if (message == WM_NCCREATE) {
          SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams));
        }
        auto *self = reinterpret_cast<windows_clipboard_t *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self) {
          switch (message) {
            case WM_CLIPBOARDUPDATE:
              self->handle_change();
              return 0;
            case check_message:
              self->handle_change(true);
              return 0;
            case WM_TIMER:
              if (wparam == poll_timer && GetClipboardSequenceNumber() != self->last_sequence_) {
                self->handle_change();
              }
              return 0;
            case task_message:
              self->run_tasks();
              return 0;
            case quit_message:
              PostQuitMessage(0);
              return 0;
            default:
              break;
          }
        }
        return DefWindowProcW(window, message, wparam, lparam);
      }

      std::wstring desktop_;  ///< Desktop the window thread joins; empty for the process's own.
      std::unique_ptr<agent_client_t> agent_;  ///< Inspects and reads the clipboard in the user's session, when used.
      platf::clipboard_observer_t *observer_ = nullptr;  ///< Receives foreign changes.
      HWND window_ = nullptr;  ///< Message-only window; set once it listens.
      DWORD last_sequence_ = 0;  ///< Clipboard sequence number last inspected or produced; window thread only.

      mutable std::mutex state_mutex_;  ///< Guards `revision_`, `sequence_` and `offer_`.
      std::atomic<std::uint64_t> revision_ {0};  ///< Revision of the current foreign content.
      DWORD sequence_ = 0;  ///< Clipboard sequence number of that content.
      fmt::windows_offer_t offer_;  ///< Canonical view of that content.

      std::mutex tasks_mutex_;  ///< Guards `tasks_`, `stopping_` and setting `window_`.
      std::deque<std::function<void()>> tasks_;  ///< Tasks for the window thread.
      bool stopping_ = false;  ///< The window thread takes no more tasks.
      std::atomic<bool> started_ {false};  ///< `start` succeeded and `stop` has not run.
      std::thread thread_;  ///< Window thread.
      std::thread::id thread_id_;  ///< Id of the window thread, set once it listens.
    };
  }  // namespace

  std::unique_ptr<platf::clipboard_t> make_clipboard(const std::wstring &desktop, agent_factory_t agent) {
    return std::make_unique<windows_clipboard_t>(desktop, std::move(agent));
  }

  std::optional<agent_link_t> spawn_agent() {
    const auto token = session_user_token();
    if (!token) {
      return std::nullopt;
    }
    std::wstring executable(MAX_PATH, L'\0');
    while (true) {
      const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
      if (size == 0) {
        return std::nullopt;
      }
      if (size < executable.size()) {
        executable.resize(size);
        break;
      }
      executable.resize(executable.size() * 2);
    }
    SECURITY_ATTRIBUTES inheritable {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE request_read = nullptr;
    HANDLE request_write = nullptr;
    HANDLE reply_read = nullptr;
    HANDLE reply_write = nullptr;
    if (!CreatePipe(&request_read, &request_write, &inheritable, 0)) {
      return std::nullopt;
    }
    if (!CreatePipe(&reply_read, &reply_write, &inheritable, 0)) {
      CloseHandle(request_read);
      CloseHandle(request_write);
      return std::nullopt;
    }
    const handle_t child_requests {request_read};
    const handle_t child_replies {reply_write};
    handle_t requests {request_write};
    handle_t replies {reply_read};
    SetHandleInformation(request_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(reply_read, HANDLE_FLAG_INHERIT, 0);

    // Only the agent's ends of the pipes are inherited.
    SIZE_T list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
    std::vector<std::uint8_t> list_memory(list_size);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_memory.data());
    std::array<HANDLE, 2> inherited {request_read, reply_write};
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &list_size)) {
      return std::nullopt;
    }
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), sizeof(inherited), nullptr, nullptr);
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = request_read;
    startup.StartupInfo.hStdOutput = reply_write;
    startup.StartupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    startup.lpAttributeList = attributes;
    void *environment = nullptr;
    CreateEnvironmentBlock(&environment, token, FALSE);
    auto command = L"\"" + executable + L"\" " + std::wstring {agent_argument.begin(), agent_argument.end()};
    const auto directory = std::filesystem::path {executable}.parent_path().native();
    PROCESS_INFORMATION process {};
    const auto created = CreateProcessAsUserW(token, executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, environment, directory.c_str(), &startup.StartupInfo, &process);
    const auto error = GetLastError();
    if (environment) {
      DestroyEnvironmentBlock(environment);
    }
    DeleteProcThreadAttributeList(attributes);
    if (!created) {
      BOOST_LOG(warning) << "[clipboard] Cannot start the clipboard agent in the user's session: "sv << error;
      return std::nullopt;
    }
    CloseHandle(process.hThread);
    auto owned_process = std::make_shared<handle_t>(process.hProcess);
    return agent_link_t {
      requests.release(),
      replies.release(),
      [owned_process]() {
        // The agent exits once its request pipe closes; one that hangs is ended.
        if (WaitForSingleObject(owned_process->get(), 2000) != WAIT_OBJECT_0) {
          TerminateProcess(owned_process->get(), 1);
        }
      },
    };
  }

  int run_agent(void *requests, void *replies, const std::wstring &desktop) {
    HDESK joined = nullptr;
    if (!desktop.empty()) {
      joined = OpenDesktopW(desktop.c_str(), 0, FALSE, GENERIC_ALL);
      if (!joined || !SetThreadDesktop(joined)) {
        if (joined) {
          CloseDesktop(joined);
        }
        return 1;
      }
    }
    while (const auto request = read_message(requests, std::nullopt)) {
      std::string reply;
      if (*request == std::string_view {&op_inspect, 1}) {
        const open_clipboard_t clipboard {nullptr};
        if (clipboard) {
          const auto inspection = inspect_open_clipboard();
          reply.push_back(static_cast<char>(platf::clipboard_result_e::ok));
          append_le(reply, inspection.sequence, 4);
          reply.push_back(inspection.opted_out ? 1 : 0);
          for (const auto &name : inspection.names) {
            reply += name;
            reply.push_back('\0');
          }
        }
      } else if (request->size() >= 9 && (*request)[0] == op_fetch) {
        const auto fetched = fetch_clipboard(std::string_view {*request}.substr(9), read_le(*request, 1, 8));
        reply.push_back(static_cast<char>(fetched.result));
        append_le(reply, fetched.sequence, 4);
        reply += fetched.bytes;
      }
      if (reply.empty()) {
        reply.push_back(static_cast<char>(platf::clipboard_result_e::io_error));
        append_le(reply, 0, 4);
      }
      if (!write_message(replies, reply)) {
        break;
      }
    }
    if (joined) {
      CloseDesktop(joined);
    }
    return 0;
  }

  std::optional<std::pair<std::string, std::string>> bitmaps_from_png(const std::string_view png) {
    const com_scope_t com;
    const auto factory = wic_factory();
    const auto image = factory ? decode(factory.Get(), png) : std::nullopt;
    if (!image) {
      return std::nullopt;
    }
    const auto width = image->width;
    const auto height = image->height;
    const auto row = std::size_t {width} * 4;

    BITMAPV5HEADER v5 {};
    v5.bV5Size = sizeof(v5);
    v5.bV5Width = static_cast<LONG>(width);
    v5.bV5Height = static_cast<LONG>(height);
    v5.bV5Planes = 1;
    v5.bV5BitCount = 32;
    v5.bV5Compression = BI_BITFIELDS;
    v5.bV5SizeImage = static_cast<DWORD>(row * height);
    v5.bV5RedMask = 0x00FF0000;
    v5.bV5GreenMask = 0x0000FF00;
    v5.bV5BlueMask = 0x000000FF;
    v5.bV5AlphaMask = 0xFF000000;
    v5.bV5CSType = 0x73524742;  // LCS_sRGB, which the SDK spells as a multi-character constant.
    v5.bV5Intent = LCS_GM_IMAGES;
    std::string dibv5 {reinterpret_cast<const char *>(&v5), sizeof(v5)};
    dibv5.reserve(sizeof(v5) + row * height);
    for (auto y = height; y-- > 0;) {
      dibv5.append(reinterpret_cast<const char *>(image->pixels.data() + y * row), row);
    }

    // Applications that read CF_DIB ignore alpha, so transparent areas are composited on white.
    const auto stride = (std::size_t {width} * 3 + 3) & ~std::size_t {3};
    BITMAPINFOHEADER info {};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(stride * height);
    std::string dib {reinterpret_cast<const char *>(&info), sizeof(info)};
    dib.reserve(sizeof(info) + stride * height);
    for (auto y = height; y-- > 0;) {
      const auto *pixel = image->pixels.data() + y * row;
      for (std::uint32_t x = 0; x < width; ++x, pixel += 4) {
        const unsigned alpha = pixel[3];
        for (int channel = 0; channel < 3; ++channel) {
          dib.push_back(static_cast<char>((pixel[channel] * alpha + 255 * (255 - alpha) + 127) / 255));
        }
      }
      dib.append(stride - std::size_t {width} * 3, '\0');
    }
    return std::pair {std::move(dibv5), std::move(dib)};
  }

  std::optional<std::string> png_from_bitmap(const std::string_view dib) {
    BITMAPINFOHEADER info {};
    if (dib.size() < sizeof(info)) {
      return std::nullopt;
    }
    std::memcpy(&info, dib.data(), sizeof(info));
    const std::int64_t height = info.biHeight;
    if (info.biSize < sizeof(info) || info.biSize > dib.size() || info.biWidth <= 0 || info.biWidth > static_cast<LONG>(max_dimension) || height == 0 || height > max_dimension || height < -static_cast<std::int64_t>(max_dimension)) {
      return std::nullopt;
    }
    // The file header needs the offset of the pixels: after the header, masks and color table.
    constexpr DWORD alpha_bitfields = 6;
    std::uint64_t masks = 0;
    if (info.biSize == sizeof(info) && info.biCompression == BI_BITFIELDS) {
      masks = 12;
    } else if (info.biSize == sizeof(info) && info.biCompression == alpha_bitfields) {
      masks = 16;
    }
    const std::uint64_t colors = info.biClrUsed ? info.biClrUsed : info.biBitCount <= 8 ? std::uint64_t {1} << info.biBitCount : 0;
    const auto pixels = info.biSize + masks + colors * 4;
    if (colors > 65536 || pixels > dib.size()) {
      return std::nullopt;
    }
    std::string file = "BM";
    append_le(file, static_cast<std::uint32_t>(14 + dib.size()), 4);
    append_le(file, 0, 4);
    append_le(file, static_cast<std::uint32_t>(14 + pixels), 4);
    file.append(dib);

    const com_scope_t com;
    const auto factory = wic_factory();
    auto image = factory ? decode(factory.Get(), file) : std::nullopt;
    if (!image) {
      return std::nullopt;
    }
    bool transparent = true;
    for (std::size_t i = 3; i < image->pixels.size() && transparent; i += 4) {
      transparent = image->pixels[i] == 0;
    }
    if (transparent) {
      for (std::size_t i = 3; i < image->pixels.size(); i += 4) {
        image->pixels[i] = 0xFF;
      }
    }
    return encode_png(factory.Get(), *image);
  }
}  // namespace clipboard::win
