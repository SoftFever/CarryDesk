/**
 * @file src/platform/macos/clipboard.mm
 * @brief Definitions for the macOS clipboard backend.
 * @details This file is built without ARC.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// platform includes
#include <AppKit/AppKit.h>
#include <dispatch/dispatch.h>

// local includes
#include "clipboard.h"
#include "src/clipboard/formats.h"
#include "src/clipboard/protocol.h"
#include "src/logging.h"

using namespace std::literals;

namespace clipboard::mac {
  namespace {
    namespace fmt = clipboard::formats;

    constexpr auto task_timeout = 5s;  ///< Longest wait for the pasteboard queue to start a task.
    constexpr auto poll_interval = 250ms;  ///< Interval between checks of the pasteboard's change count.
    constexpr NSInteger max_dimension = 16384;  ///< Largest image width or height converted.
    constexpr std::uint64_t max_decoded = 256ULL * 1048576;  ///< Largest decoded image converted.

    /**
     * @brief Progress of a task handed to the pasteboard queue.
     */
    struct task_state_t {
      /**
       * @brief Task progress.
       */
      enum class step_e {
        queued,  ///< Waiting in the queue.
        running,  ///< Running on the queue.
        finished,  ///< Done; its results may be read.
        abandoned,  ///< The caller gave up; the task will not run.
      };

      std::mutex mutex;  ///< Guards `step`.
      std::condition_variable done;  ///< Signalled when the task finished.
      step_e step = step_e::queued;  ///< Current progress.
    };

    /**
     * @brief Make a string from UTF-8 bytes.
     *
     * @param bytes UTF-8 bytes.
     * @return Autoreleased string; empty when the bytes are not UTF-8.
     */
    NSString *ns_string(const std::string_view bytes) {
      NSString *string = [[[NSString alloc] initWithBytes:bytes.data() length:bytes.size() encoding:NSUTF8StringEncoding] autorelease];
      return string ? string : @"";
    }

    /**
     * @brief Make data from bytes.
     *
     * @param bytes Bytes.
     * @return Autoreleased data.
     */
    NSData *ns_data(const std::string_view bytes) {
      return [NSData dataWithBytes:bytes.data() length:bytes.size()];
    }

    /**
     * @brief Copy the bytes of data.
     *
     * @param data Data.
     * @return Bytes.
     */
    std::string bytes_of(NSData *data) {
      return {static_cast<const char *>(data.bytes), data.length};
    }

    /**
     * @brief Get the UTF-8 bytes of a string.
     *
     * @param string String; unpaired surrogates become U+FFFD.
     * @return UTF-8 bytes.
     */
    std::string utf8_of(NSString *string) {
      NSData *data = [string dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:YES];
      return data ? bytes_of(data) : std::string {};
    }

    /**
     * @brief Decode an image within the conversion limits.
     *
     * @param data PNG or TIFF image.
     * @return Autoreleased bitmap, or nil when the image cannot be decoded or exceeds the limits.
     */
    NSBitmapImageRep *decode(NSData *data) {
      NSBitmapImageRep *image = data ? [NSBitmapImageRep imageRepWithData:data] : nil;
      if (!image || image.pixelsWide <= 0 || image.pixelsHigh <= 0 || image.pixelsWide > max_dimension || image.pixelsHigh > max_dimension ||
          static_cast<std::uint64_t>(image.pixelsWide) * static_cast<std::uint64_t>(image.pixelsHigh) * 4 > max_decoded) {
        return nil;
      }
      return image;
    }

    /**
     * @brief Clipboard backend on an `NSPasteboard`.
     * @details Every pasteboard access runs on one serial queue, which also checks the change count
     *          on a timer; callers on other threads hand their work to it and wait.
     */
    class macos_clipboard_t: public platf::clipboard_t {
    public:
      /**
       * @brief Prepare a backend.
       *
       * @param pasteboard Name of the pasteboard; empty for the general pasteboard.
       */
      explicit macos_clipboard_t(std::string pasteboard):
          name_ {std::move(pasteboard)} {
      }

      ~macos_clipboard_t() override {
        stop();
      }

      bool start(platf::clipboard_observer_t &observer) override {
        @autoreleasepool {
          NSPasteboard *pasteboard = name_.empty() ? [NSPasteboard generalPasteboard] : [NSPasteboard pasteboardWithName:ns_string(name_)];
          if (!pasteboard) {
            BOOST_LOG(warning) << "[clipboard] Cannot open the macOS pasteboard"sv;
            return false;
          }
          pasteboard_ = [pasteboard retain];
        }
        observer_ = &observer;
        queue_ = dispatch_queue_create("dev.lizardbyte.sunshine.clipboard", DISPATCH_QUEUE_SERIAL);
        timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);
        const auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(poll_interval).count();
        dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, interval), interval, interval / 10);
        dispatch_source_set_event_handler(timer_, ^{
          check(false);
        });
        // The pasteboard's content at start is reported like a change.
        dispatch_async(queue_, ^{
          check(true);
        });
        dispatch_resume(timer_);
        {
          std::lock_guard lock {tasks_mutex_};
          stopping_ = false;
        }
        started_ = true;
        BOOST_LOG(info) << "[clipboard] Using the macOS pasteboard"sv;
        return true;
      }

      void stop() override {
        if (!started_.exchange(false)) {
          return;
        }
        run_task([this]() {
          release_owned();
        });
        {
          std::lock_guard lock {tasks_mutex_};
          stopping_ = true;
        }
        dispatch_source_cancel(timer_);
        // Waits for whatever the queue is running: a check, or a task a caller is waiting for.
        dispatch_sync(queue_, ^{});
        dispatch_release(timer_);
        dispatch_release(queue_);
        timer_ = nullptr;
        queue_ = nullptr;
        [pasteboard_ release];
        pasteboard_ = nil;
      }

      [[nodiscard]] std::vector<std::string> formats() const override {
        return {std::string {mime::text}, std::string {mime::html}, std::string {mime::rtf}, std::string {mime::png}, std::string {mime::files}};
      }

      platf::clipboard_read_t read(const std::uint64_t revision, const std::string &mime, const std::uint64_t max_size) override {
        platf::clipboard_read_t result;
        result.result = platf::clipboard_result_e::stale;
        fmt::macos_offer_t offer;
        NSInteger count = 0;
        {
          std::lock_guard lock {state_mutex_};
          if (revision != revision_) {
            return result;
          }
          offer = offer_;
          count = count_;
        }
        const auto format = std::ranges::find(offer.formats, mime, &fmt::native_format_t::canonical);
        if (offer.sensitive || format == offer.formats.end()) {
          return result;
        }
        const auto &native = format->native;
        if (!run_task([&]() {
              result = read_pasteboard(count, mime, native, max_size);
            })) {
          result.result = platf::clipboard_result_e::io_error;
        }
        if (revision != revision_) {
          result = {};
          result.result = platf::clipboard_result_e::stale;
        }
        return result;
      }

      platf::clipboard_result_e take_ownership(const std::uint64_t expected_revision, const std::map<std::string, std::string, std::less<>> &content, const std::vector<std::string> &file_paths) override {
        auto status = platf::clipboard_result_e::io_error;
        run_task([&]() {
          status = write_pasteboard(expected_revision, content, file_paths);
        });
        return status;
      }

      void release() override {
        run_task([this]() {
          release_owned();
        });
      }

    private:
      /**
       * @brief Check whether macOS refuses Sunshine every pasteboard read.
       *
       * @return `true` when the user denied Sunshine access to other applications' pasteboard content.
       */
      bool reads_denied() const {
        if (@available(macOS 15.4, *)) {
          return pasteboard_.accessBehavior == NSPasteboardAccessBehaviorAlwaysDeny;
        }
        return false;
      }

      /**
       * @brief Read one canonical format of the pasteboard; runs on the queue.
       *
       * @param count Change count of the content being read.
       * @param mime Canonical format.
       * @param native Pasteboard type to read it from.
       * @param max_size Largest acceptable size in bytes.
       * @return Read result.
       */
      platf::clipboard_read_t read_pasteboard(const NSInteger count, const std::string &mime, const std::string &native, const std::uint64_t max_size) {
        @autoreleasepool {
          platf::clipboard_read_t result;
          result.result = platf::clipboard_result_e::stale;
          if (pasteboard_.changeCount != count) {
            return result;
          }

          bool found = false;
          if (mime == clipboard::mime::files) {
            NSArray<NSURL *> *urls = [pasteboard_ readObjectsForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
            for (NSURL *url in urls) {
              // A file reference URL names the file by its id; its path URL names it by path.
              NSString *path = url.filePathURL.path;
              if (path) {
                result.paths.emplace_back(path.fileSystemRepresentation);
              }
            }
            found = !result.paths.empty();
          } else if (mime == clipboard::mime::text || mime == clipboard::mime::html) {
            if (NSString *string = [pasteboard_ stringForType:ns_string(native)]) {
              const auto utf8 = utf8_of(string);
              result.bytes = mime == clipboard::mime::text ? fmt::normalize_text(utf8) : fmt::html_to_utf8(utf8);
              found = true;
            }
          } else if (NSData *data = [pasteboard_ dataForType:ns_string(native)]) {
            found = true;
            if (mime == clipboard::mime::png && native != fmt::macos::png) {
              NSBitmapImageRep *image = decode(data);
              NSData *png = image ? [image representationUsingType:NSBitmapImageFileTypePNG properties:@{}] : nil;
              if (!png) {
                BOOST_LOG(warning) << "[clipboard] Cannot convert the host's image to PNG"sv;
                result.result = platf::clipboard_result_e::io_error;
                return result;
              }
              result.bytes = bytes_of(png);
            } else if (data.length <= max_size) {
              result.bytes = bytes_of(data);
            } else {
              result.result = platf::clipboard_result_e::too_large;
              return result;
            }
          }

          if (!found) {
            if (reads_denied()) {
              if (!denied_logged_.exchange(true)) {
                BOOST_LOG(warning) << "[clipboard] macOS denies Sunshine access to the clipboard; allow Sunshine to paste from other apps in Privacy & Security settings"sv;
              }
              result.result = platf::clipboard_result_e::not_permitted;
            } else {
              result.result = platf::clipboard_result_e::io_error;
            }
            return result;
          }
          if (pasteboard_.changeCount != count) {
            return {.result = platf::clipboard_result_e::stale};
          }
          if (result.bytes.size() > max_size) {
            result.bytes.clear();
            result.result = platf::clipboard_result_e::too_large;
            return result;
          }
          result.result = platf::clipboard_result_e::ok;
          return result;
        }
      }

      /**
       * @brief Put a client's content on the pasteboard; runs on the queue.
       * @details All content goes on the first item; each staged file adds its URL, the first one to
       *          that item. The content stays on this Mac, out of Universal Clipboard, and is marked
       *          transient so clipboard history applications do not record it.
       *
       * @param expected_revision Latest revision the caller saw.
       * @param content Canonical format to bytes.
       * @param file_paths Absolute paths of staged top-level file entries.
       * @return Outcome.
       */
      platf::clipboard_result_e write_pasteboard(const std::uint64_t expected_revision, const std::map<std::string, std::string, std::less<>> &content, const std::vector<std::string> &file_paths) {
        @autoreleasepool {
          if (expected_revision != revision_ || pasteboard_.changeCount != last_count_) {
            // Another application changed the pasteboard; the next check reports it.
            return platf::clipboard_result_e::stale;
          }

          NSMutableArray<NSPasteboardItem *> *items = [NSMutableArray array];
          NSPasteboardItem *first = [[[NSPasteboardItem alloc] init] autorelease];
          [items addObject:first];
          bool complete = true;
          if (const auto text = content.find(mime::text); text != content.end()) {
            complete &= [first setString:ns_string(text->second) forType:NSPasteboardTypeString];
          } else if (!file_paths.empty()) {
            complete &= [first setString:ns_string(fmt::make_path_text(file_paths)) forType:NSPasteboardTypeString];
          }
          if (const auto html = content.find(mime::html); html != content.end()) {
            complete &= [first setString:ns_string(html->second) forType:NSPasteboardTypeHTML];
          }
          if (const auto rtf = content.find(mime::rtf); rtf != content.end()) {
            complete &= [first setData:ns_data(rtf->second) forType:NSPasteboardTypeRTF];
          }
          if (const auto png = content.find(mime::png); png != content.end()) {
            NSData *data = ns_data(png->second);
            complete &= [first setData:data forType:NSPasteboardTypePNG];
            // Many Mac applications paste only TIFF.
            if (NSData *tiff = decode(data).TIFFRepresentation) {
              complete &= [first setData:tiff forType:NSPasteboardTypeTIFF];
            } else {
              BOOST_LOG(warning) << "[clipboard] Cannot convert the client's image for applications that need TIFF"sv;
            }
          }
          for (std::size_t i = 0; i < file_paths.size(); ++i) {
            NSPasteboardItem *item = first;
            if (i > 0) {
              item = [[[NSPasteboardItem alloc] init] autorelease];
              [items addObject:item];
            }
            NSURL *url = [NSURL fileURLWithPath:ns_string(file_paths[i])];
            complete &= [item setString:url.absoluteString forType:NSPasteboardTypeFileURL];
          }
          complete &= [first setData:[NSData data] forType:ns_string(fmt::macos::transient)];

          [pasteboard_ prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
          if (complete) {
            complete = [pasteboard_ writeObjects:items];
          }
          if (!complete) {
            [pasteboard_ clearContents];
          }
          own_count_ = last_count_ = pasteboard_.changeCount;
          return complete ? platf::clipboard_result_e::ok : platf::clipboard_result_e::io_error;
        }
      }

      /**
       * @brief Empty the pasteboard if it still holds Sunshine's content; runs on the queue.
       */
      void release_owned() {
        @autoreleasepool {
          if (own_count_ >= 0 && pasteboard_.changeCount == own_count_) {
            [pasteboard_ clearContents];
            own_count_ = last_count_ = pasteboard_.changeCount;
          }
        }
      }

      /**
       * @brief Inspect the pasteboard after a change and report it unless Sunshine made it; runs on the queue.
       * @details Inspecting reads only the content's types, which macOS lets Sunshine do without asking
       *          the user.
       *
       * @param initial Report the pasteboard as it is at start, whatever its change count.
       */
      void check(const bool initial) {
        @autoreleasepool {
          const auto count = pasteboard_.changeCount;
          if (!initial && count == last_count_) {
            return;
          }
          last_count_ = count;
          if (!initial && count == own_count_) {
            // Sunshine's own content, or the empty pasteboard it left after releasing it.
            return;
          }
          std::vector<std::string> types;
          for (NSPasteboardType type in pasteboard_.types) {
            if (const char *name = type.UTF8String) {
              types.emplace_back(name);
            }
          }
          auto offer = fmt::map_macos_offer(types);
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
            count_ = count;
            offer_ = std::move(offer);
          }
          observer_->on_clipboard_changed(revision, std::move(formats));
        }
      }

      /**
       * @brief Run a task on the pasteboard queue and wait for it.
       * @details A task the queue has not started within the timeout is abandoned and never runs,
       *          so a caller that gave up can neither race the task's results nor be surprised by its
       *          effects later. A task that has started is waited for.
       *
       * @param task Task to run.
       * @return `false` when the task was abandoned or the backend is stopping.
       */
      bool run_task(std::function<void()> task) {
        auto state = std::make_shared<task_state_t>();
        auto job = std::make_shared<std::function<void()>>(std::move(task));
        {
          std::lock_guard lock {tasks_mutex_};
          if (stopping_ || !queue_) {
            return false;
          }
          dispatch_async(queue_, ^{
            {
              std::lock_guard step_lock {state->mutex};
              if (state->step == task_state_t::step_e::abandoned) {
                return;
              }
              state->step = task_state_t::step_e::running;
            }
            (*job)();
            {
              std::lock_guard step_lock {state->mutex};
              state->step = task_state_t::step_e::finished;
            }
            state->done.notify_all();
          });
        }
        std::unique_lock lock {state->mutex};
        const auto finished = [&state]() {
          return state->step == task_state_t::step_e::finished;
        };
        if (state->done.wait_for(lock, task_timeout, finished)) {
          return true;
        }
        if (state->step == task_state_t::step_e::queued) {
          state->step = task_state_t::step_e::abandoned;
          BOOST_LOG(warning) << "[clipboard] The pasteboard queue did not respond in time"sv;
          return false;
        }
        state->done.wait(lock, finished);
        return true;
      }

      std::string name_;  ///< Name of the pasteboard; empty for the general pasteboard.
      NSPasteboard *pasteboard_ = nil;  ///< Pasteboard, retained while started.
      platf::clipboard_observer_t *observer_ = nullptr;  ///< Receives foreign changes.
      dispatch_queue_t queue_ = nullptr;  ///< Serial queue of every pasteboard access.
      dispatch_source_t timer_ = nullptr;  ///< Checks the change count on the queue.
      NSInteger last_count_ = -1;  ///< Change count last inspected or produced; queue only.
      NSInteger own_count_ = -1;  ///< Change count of Sunshine's own content, or -1; queue only.
      std::atomic<bool> denied_logged_ {false};  ///< The denied-access warning was logged.

      mutable std::mutex state_mutex_;  ///< Guards `revision_`, `count_` and `offer_`.
      std::atomic<std::uint64_t> revision_ {0};  ///< Revision of the current foreign content.
      NSInteger count_ = -1;  ///< Change count of that content.
      fmt::macos_offer_t offer_;  ///< Canonical view of that content.

      std::mutex tasks_mutex_;  ///< Guards `stopping_` and queueing tasks.
      bool stopping_ = false;  ///< The queue takes no more tasks.
      std::atomic<bool> started_ {false};  ///< `start` succeeded and `stop` has not run.
    };
  }  // namespace

  std::unique_ptr<platf::clipboard_t> make_clipboard(const std::string &pasteboard) {
    return std::make_unique<macos_clipboard_t>(pasteboard);
  }
}  // namespace clipboard::mac
