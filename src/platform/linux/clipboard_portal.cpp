/**
 * @file src/platform/linux/clipboard_portal.cpp
 * @brief Definitions for the clipboard backend on the XDG desktop portal.
 * @details Session setup and the portal's signals run on a thread of the backend's own, which
 *          iterates a private GLib main context. Reads and ownership changes are synchronous D-Bus
 *          calls on their caller's thread. The portal hands clipboard data over through pipes: reads
 *          are drained by their caller, and Sunshine's own content goes out through the Wayland
 *          backend's writer thread.
 *
 *          Whether Sunshine owns the clipboard follows the portal's `SelectionOwnerChanged` signals,
 *          which arrive in the order the compositor changed owners, so the state settles on the
 *          compositor's even when a signal overtakes a call that caused it.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// lib includes
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

// local includes
#include "clipboard_portal.h"
#include "clipboard_wayland.h"
#include "src/clipboard/formats.h"
#include "src/clipboard/protocol.h"
#include "src/logging.h"

using namespace std::literals;

namespace portal {
  namespace {
    constexpr auto portal_name = "org.freedesktop.portal.Desktop";  ///< Bus name of the portal.
    constexpr auto portal_path = "/org/freedesktop/portal/desktop";  ///< Object path of the portal.
    constexpr auto remote_desktop_interface = "org.freedesktop.portal.RemoteDesktop";  ///< Remote desktop portal.
    constexpr auto clipboard_interface = "org.freedesktop.portal.Clipboard";  ///< Clipboard portal.
    constexpr auto request_interface = "org.freedesktop.portal.Request";  ///< Interface of a pending request.
    constexpr auto session_interface = "org.freedesktop.portal.Session";  ///< Interface of a session.
    constexpr std::string_view request_prefix = "/org/freedesktop/portal/desktop/request/";  ///< Path prefix of requests.
    constexpr auto session_token = "sunshine_clipboard";  ///< Handle token of the session.
    constexpr std::uint32_t keyboard_device = 1;  ///< Device type of the keyboard.
    constexpr std::uint32_t persist_until_revoked = 2;  ///< Persist mode that keeps the permission across restarts.
    constexpr std::uint32_t response_cancelled = 1;  ///< Response code of a request the user turned down.
    constexpr int call_timeout_ms = 5000;  ///< Longest wait for a portal method reply.
    constexpr auto stall_timeout = 5s;  ///< Longest time a pipe may go without progress.
    constexpr std::uint64_t max_uri_list = 1048576;  ///< Largest `text/uri-list` read to detect files.

    /**
     * @brief Progress of the portal session.
     */
    enum class state_e {
      starting,  ///< The session is being set up or waits for the user's answer.
      active,  ///< The session has clipboard access.
      ended,  ///< The session failed, was turned down or was closed.
    };

    /**
     * @brief Get the descriptor a method returned as its only handle.
     *
     * @param reply Method reply of type `(h)`, or null.
     * @param fds Descriptors that came with the reply, or null.
     * @return Owned descriptor, or -1.
     */
    int take_fd(GVariant *reply, GUnixFDList *fds) {
      if (!reply || !fds) {
        return -1;
      }
      gint32 index = -1;
      g_variant_get(reply, "(h)", &index);
      return g_unix_fd_list_get(fds, index, nullptr);
    }

    /**
     * @brief Clipboard backend on a remote desktop session of the XDG desktop portal.
     */
    class portal_clipboard_t: public platf::clipboard_t {
    public:
      /**
       * @brief Create an idle backend.
       *
       * @param token_file File that keeps the portal's restore token.
       */
      explicit portal_clipboard_t(std::filesystem::path token_file):
          token_file_ {std::move(token_file)} {
      }

      portal_clipboard_t(const portal_clipboard_t &) = delete;
      portal_clipboard_t &operator=(const portal_clipboard_t &) = delete;

      ~portal_clipboard_t() override {
        stop();
      }

      bool start(platf::clipboard_observer_t &observer) override {
        observer_ = &observer;
        g_autoptr(GError) error = nullptr;
        g_autofree gchar *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (address) {
          connection_ = g_dbus_connection_new_for_address_sync(address, GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, &error);
        }
        if (!connection_) {
          BOOST_LOG(info) << "[clipboard] No D-Bus session bus to reach the desktop portal on: "sv << (error ? error->message : "unknown error");
          return false;
        }
        if (version(clipboard_interface) < 1) {
          BOOST_LOG(info) << "[clipboard] The desktop portal does not offer clipboard access"sv;
          disconnect();
          return false;
        }
        persist_ = version(remote_desktop_interface) >= 2;
        if (persist_) {
          std::ifstream file {token_file_};
          std::getline(file, restore_token_);
        }

        // Request and session paths embed the connection's unique name without its leading colon.
        sender_ = g_dbus_connection_get_unique_name(connection_) + 1;
        std::ranges::replace(sender_, '.', '_');
        context_ = g_main_context_new();
        loop_ = g_main_loop_new(context_, FALSE);
        writer_ = std::make_unique<wl::writer_t>();
        thread_ = std::thread([this]() {
          run();
        });
        started_ = true;
        BOOST_LOG(info) << "[clipboard] Using the desktop portal clipboard"sv;
        return true;
      }

      void stop() override {
        if (!started_.exchange(false)) {
          return;
        }
        release();
        std::string request;
        std::string session;
        {
          std::lock_guard lock {mutex_};
          state_ = state_e::ended;
          request = std::exchange(request_path_, {});
          session = std::exchange(session_, {});
        }
        close_object(request, request_interface);
        close_object(session, session_interface);
        g_main_context_invoke(
          context_,
          [](gpointer loop) {
            g_main_loop_quit(static_cast<GMainLoop *>(loop));
            return G_SOURCE_REMOVE;
          },
          loop_
        );
        thread_.join();
        writer_.reset();
        disconnect();
      }

      [[nodiscard]] std::vector<std::string> formats() const override {
        return {std::string {clipboard::mime::text}, std::string {clipboard::mime::html}, std::string {clipboard::mime::rtf}, std::string {clipboard::mime::png}, std::string {clipboard::mime::files}};
      }

      platf::clipboard_read_t read(const std::uint64_t revision, const std::string &mime, const std::uint64_t max_size) override {
        platf::clipboard_read_t result;
        std::string native;
        std::string session;
        {
          std::lock_guard lock {mutex_};
          if (state_ != state_e::active) {
            return result;
          }
          const auto offer = clipboard::formats::map_linux_offer(current_mimes_, current_has_files_);
          const auto format = std::ranges::find(offer.formats, mime, &clipboard::formats::native_format_t::canonical);
          if (revision != revision_ || own_ || offer.sensitive || format == offer.formats.end()) {
            result.result = platf::clipboard_result_e::stale;
            return result;
          }
          native = format->native;
          session = session_;
        }

        std::string bytes;
        {
          std::lock_guard read_lock {read_mutex_};
          result.result = read_selection(session, native, max_size, bytes);
        }
        if (current_revision() != revision) {
          result.result = platf::clipboard_result_e::stale;
        }
        if (result.result != platf::clipboard_result_e::ok) {
          return result;
        }
        if (mime == clipboard::mime::files) {
          result.paths = clipboard::formats::file_paths_from_uri_list(bytes);
        } else {
          result.bytes = clipboard::formats::linux_to_canonical(mime, bytes);
        }
        return result;
      }

      platf::clipboard_result_e take_ownership(const std::uint64_t expected_revision, const std::map<std::string, std::string, std::less<>> &content, const std::vector<std::string> &file_paths) override {
        auto offers = std::make_shared<const clipboard::formats::native_offers_t>(clipboard::formats::linux_native_offers(content, file_paths));
        std::vector<std::string> types;
        for (const auto &[type, bytes] : *offers) {
          types.push_back(type);
        }
        std::string session;
        {
          std::lock_guard lock {mutex_};
          if (state_ != state_e::active) {
            return platf::clipboard_result_e::not_permitted;
          }
          if (expected_revision != revision_) {
            return platf::clipboard_result_e::stale;
          }
          served_ = offers;
          own_ = true;
          expect_own_clear_ = false;
          session = session_;
        }

        // The portal replies once the compositor made the session the owner.
        if (set_selection(session, types)) {
          return platf::clipboard_result_e::ok;
        }
        std::lock_guard lock {mutex_};
        if (served_ == offers) {
          served_.reset();
          own_ = false;
        }
        return platf::clipboard_result_e::io_error;
      }

      void release() override {
        std::shared_ptr<const clipboard::formats::native_offers_t> released;
        std::string session;
        {
          std::lock_guard lock {mutex_};
          if (!own_ || state_ != state_e::active) {
            return;
          }
          own_ = false;
          expect_own_clear_ = true;
          released = served_;
          session = session_;
        }
        set_selection(session, {});
        std::lock_guard lock {mutex_};
        if (served_ == released) {
          served_.reset();
        }
      }

    private:
      /**
       * @brief Get a portal interface's version.
       * @details Waits as long as D-Bus activation may take, since Sunshine can start along with
       *          the desktop session before the portal runs.
       *
       * @param interface Portal interface.
       * @return Version, or 0 when the portal does not offer the interface.
       */
      std::uint32_t version(const char *interface) {
        g_autoptr(GVariant) reply = g_dbus_connection_call_sync(connection_, portal_name, portal_path, "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", interface, "version"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
        if (!reply) {
          return 0;
        }
        g_autoptr(GVariant) value = nullptr;
        g_variant_get(reply, "(v)", &value);
        return g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(value) : 0;
      }

      /**
       * @brief Close the bus connection and free the GLib objects.
       */
      void disconnect() {
        if (connection_) {
          g_dbus_connection_close_sync(connection_, nullptr, nullptr);
          if (context_) {
            // Replies to calls cut off by the close are delivered here; they find the session ended.
            while (g_main_context_iteration(context_, FALSE)) {
            }
          }
          g_clear_object(&connection_);
        }
        g_clear_pointer(&loop_, g_main_loop_unref);
        g_clear_pointer(&context_, g_main_context_unref);
      }

      /**
       * @brief Close a portal request or session, ignoring failures.
       *
       * @param path Object path; nothing happens when empty.
       * @param interface Interface with the `Close` method.
       */
      void close_object(const std::string &path, const char *interface) {
        if (path.empty()) {
          return;
        }
        g_autoptr(GVariant) reply = g_dbus_connection_call_sync(connection_, portal_name, path.c_str(), interface, "Close", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, call_timeout_ms, nullptr, nullptr);
      }

      /**
       * @brief Get the revision of the current foreign selection.
       *
       * @return Revision.
       */
      std::uint64_t current_revision() {
        std::lock_guard lock {mutex_};
        return revision_;
      }

      /**
       * @brief Make the session the clipboard owner, or give up ownership.
       *
       * @param session Session handle.
       * @param types Native types to offer; empty to give up ownership.
       * @return `true` when the portal accepted the change.
       */
      bool set_selection(const std::string &session, const std::vector<std::string> &types) {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        if (!types.empty()) {
          std::vector<const gchar *> names;
          for (const auto &type : types) {
            names.push_back(type.c_str());
          }
          g_variant_builder_add(&options, "{sv}", "mime_types", g_variant_new_strv(names.data(), static_cast<gssize>(names.size())));
        }
        g_autoptr(GError) error = nullptr;
        g_autoptr(GVariant) reply = g_dbus_connection_call_sync(connection_, portal_name, portal_path, clipboard_interface, "SetSelection", g_variant_new("(oa{sv})", session.c_str(), &options), nullptr, G_DBUS_CALL_FLAGS_NONE, call_timeout_ms, nullptr, &error);
        if (!reply) {
          BOOST_LOG(warning) << "[clipboard] The desktop portal did not change the clipboard: "sv << error->message;
          return false;
        }
        return true;
      }

      /**
       * @brief Read one native type of the current selection.
       * @details The compositor serves one read per session at a time; callers hold `read_mutex_`.
       *
       * @param session Session handle.
       * @param native Native type.
       * @param max_size Largest acceptable size in bytes.
       * @param bytes Receives the bytes read.
       * @return Read result.
       */
      platf::clipboard_result_e read_selection(const std::string &session, const std::string &native, const std::uint64_t max_size, std::string &bytes) {
        g_autoptr(GError) error = nullptr;
        g_autoptr(GUnixFDList) fds = nullptr;
        g_autoptr(GVariant) reply = g_dbus_connection_call_with_unix_fd_list_sync(connection_, portal_name, portal_path, clipboard_interface, "SelectionRead", g_variant_new("(os)", session.c_str(), native.c_str()), G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, call_timeout_ms, nullptr, &fds, nullptr, &error);
        const auto fd = take_fd(reply, fds);
        if (fd < 0) {
          BOOST_LOG(warning) << "[clipboard] The desktop portal could not read the clipboard: "sv << (error ? error->message : "no pipe");
          return platf::clipboard_result_e::io_error;
        }
        const auto result = wl::drain_pipe(fd, max_size, stall_timeout, bytes);
        ::close(fd);
        return result;
      }

      /**
       * @brief Thread body: set up the session, then dispatch the portal's signals until `stop`.
       */
      void run() {
        g_main_context_push_thread_default(context_);
        owner_changed_ = subscribe(clipboard_interface, "SelectionOwnerChanged", portal_path, &on_owner_changed);
        transfer_ = subscribe(clipboard_interface, "SelectionTransfer", portal_path, &on_transfer);
        create_session();
        g_main_loop_run(loop_);
        for (const auto id : {owner_changed_, transfer_, closed_, response_}) {
          if (id) {
            g_dbus_connection_signal_unsubscribe(connection_, id);
          }
        }
        g_main_context_pop_thread_default(context_);
      }

      /**
       * @brief Subscribe to a signal of the portal on the backend's thread.
       *
       * @param interface Interface of the signal.
       * @param member Signal name.
       * @param path Object path that emits it.
       * @param callback Handler; receives the backend.
       * @return Subscription id.
       */
      guint subscribe(const char *interface, const char *member, const char *path, GDBusSignalCallback callback) {
        return g_dbus_connection_signal_subscribe(connection_, portal_name, interface, member, path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, callback, this, nullptr);
      }

      /**
       * @brief A method call whose reply goes to a callback on the backend's thread.
       */
      struct pending_call_t {
        portal_clipboard_t *self;  ///< Backend.
        const char *method;  ///< Method name, for the log.
        std::function<void(GVariant *)> on_reply;  ///< Handles the reply.
      };

      /**
       * @brief Call a portal method without waiting; a failure ends the session.
       *
       * @param interface Interface of the method.
       * @param method Method name.
       * @param parameters Arguments; floating references are consumed.
       * @param reply_type Expected reply type.
       * @param on_reply Handles a successful reply.
       */
      void call(const char *interface, const char *method, GVariant *parameters, const GVariantType *reply_type, std::function<void(GVariant *)> on_reply) {
        auto *pending = new pending_call_t {this, method, std::move(on_reply)};
        g_dbus_connection_call(
          connection_,
          portal_name,
          portal_path,
          interface,
          method,
          parameters,
          reply_type,
          G_DBUS_CALL_FLAGS_NONE,
          call_timeout_ms,
          nullptr,
          [](GObject *source, GAsyncResult *result, gpointer data) {
            const std::unique_ptr<pending_call_t> pending {static_cast<pending_call_t *>(data)};
            g_autoptr(GError) error = nullptr;
            g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
            if (!reply) {
              pending->self->end("[clipboard] The desktop portal failed to set up clipboard access: "s + pending->method + ": " + error->message, true);
              return;
            }
            pending->on_reply(reply);
          },
          pending
        );
      }

      /**
       * @brief Call a remote desktop method that answers through a request object.
       * @details Subscribes to the request's `Response` before calling, so the answer cannot be
       *          missed. Session setup makes one request at a time.
       *
       * @param method Method name.
       * @param token Handle token in the method's options.
       * @param parameters Arguments; floating references are consumed.
       * @param on_response Handles the response code and results.
       */
      void request(const char *method, const std::string &token, GVariant *parameters, std::function<void(std::uint32_t, GVariant *)> on_response) {
        const auto path = std::string {request_prefix} + sender_ + "/" + token;
        on_response_ = std::move(on_response);
        response_ = subscribe(request_interface, "Response", path.c_str(), &on_response_signal);
        {
          std::lock_guard lock {mutex_};
          request_path_ = path;
        }
        call(remote_desktop_interface, method, parameters, G_VARIANT_TYPE("(o)"), [](GVariant *) {
        });
      }

      /**
       * @brief Get a new handle token for a request.
       *
       * @return Token.
       */
      std::string next_token() {
        return "sunshine_clipboard_" + std::to_string(++tokens_);
      }

      /**
       * @brief Ask the portal for a remote desktop session.
       */
      void create_session() {
        const auto token = next_token();
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(session_token));
        request("CreateSession", token, g_variant_new("(a{sv})", &options), [this](const std::uint32_t code, GVariant *results) {
          g_autoptr(GVariant) handle = g_variant_lookup_value(results, "session_handle", nullptr);
          if (code != 0 || !handle || !(g_variant_is_of_type(handle, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(handle, G_VARIANT_TYPE_OBJECT_PATH))) {
            end("[clipboard] The desktop portal did not create a remote desktop session", false);
            return;
          }
          const std::string session = g_variant_get_string(handle, nullptr);
          bool ended;
          {
            std::lock_guard lock {mutex_};
            ended = state_ == state_e::ended;
            if (!ended) {
              session_ = session;
            }
          }
          if (ended) {
            close_object(session, session_interface);
            return;
          }
          closed_ = subscribe(session_interface, "Closed", session.c_str(), &on_closed);
          select_devices(session);
        });
      }

      /**
       * @brief Ask for a session with the keyboard, restoring an earlier permission if there is one.
       * @details GNOME keeps the permission for later starts only for a session with an input device or
       *          a screen, and allows its dialog only with remote interaction turned on. The keyboard is
       *          the least of these; Sunshine never uses it.
       *
       * @param session Session handle.
       */
      void select_devices(const std::string &session) {
        const auto token = next_token();
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        g_variant_builder_add(&options, "{sv}", "types", g_variant_new_uint32(keyboard_device));
        if (persist_) {
          g_variant_builder_add(&options, "{sv}", "persist_mode", g_variant_new_uint32(persist_until_revoked));
          if (!restore_token_.empty()) {
            g_variant_builder_add(&options, "{sv}", "restore_token", g_variant_new_string(restore_token_.c_str()));
          }
        }
        request("SelectDevices", token, g_variant_new("(oa{sv})", session.c_str(), &options), [this, session](const std::uint32_t code, GVariant *) {
          if (code != 0) {
            end("[clipboard] The desktop portal turned down the remote desktop session", true);
            return;
          }
          GVariantBuilder empty;
          g_variant_builder_init(&empty, G_VARIANT_TYPE_VARDICT);
          call(clipboard_interface, "RequestClipboard", g_variant_new("(oa{sv})", session.c_str(), &empty), G_VARIANT_TYPE("()"), [this, session](GVariant *) {
            start_session(session);
          });
        });
      }

      /**
       * @brief Start the session, which asks the user unless an earlier permission is restored.
       *
       * @param session Session handle.
       */
      void start_session(const std::string &session) {
        if (restore_token_.empty()) {
          BOOST_LOG(info) << "[clipboard] Waiting for clipboard access to be allowed on the host; on GNOME, turn on \"Allow Remote Interaction\" and \"Remember this selection\" in its dialog"sv;
        }
        const auto token = next_token();
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        request("Start", token, g_variant_new("(osa{sv})", session.c_str(), "", &options), [this](const std::uint32_t code, GVariant *results) {
          started(code, results);
        });
      }

      /**
       * @brief Handle the answer to `Start`.
       *
       * @param code Response code.
       * @param results Response results.
       */
      void started(const std::uint32_t code, GVariant *results) {
        gboolean enabled = FALSE;
        g_variant_lookup(results, "clipboard_enabled", "b", &enabled);
        if (code == response_cancelled) {
          end("[clipboard] Clipboard access was not allowed on the host; clipboard sync stays off until Sunshine restarts", true);
          return;
        }
        if (code != 0) {
          end("[clipboard] The desktop portal could not start the clipboard session", true);
          return;
        }
        if (!enabled) {
          end("[clipboard] Clipboard access was turned off on the host; clipboard sync stays off until Sunshine restarts", true);
          return;
        }
        if (persist_) {
          keep_restore_token(results);
        }

        std::uint64_t revision;
        {
          std::lock_guard lock {mutex_};
          if (state_ != state_e::starting) {
            return;
          }
          state_ = state_e::active;
          revision = revision_;
        }
        BOOST_LOG(info) << "[clipboard] Clipboard access was allowed on the host"sv;
        // The compositor reports the selection it has while the session starts; announce it now.
        if (revision != 0) {
          announce(revision);
        }
      }

      /**
       * @brief Save the restore token of a started session, or forget a spent one.
       *
       * @param results Results of `Start`.
       */
      void keep_restore_token(GVariant *results) {
        const gchar *token = nullptr;
        if (g_variant_lookup(results, "restore_token", "&s", &token) && token[0] != '\0') {
          if (restore_token_ == token) {
            return;
          }
          restore_token_ = token;
          std::ofstream file {token_file_, std::ios::trunc};
          file << restore_token_;
          file.close();
          std::error_code ec;
          std::filesystem::permissions(token_file_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
          if (!file || ec) {
            BOOST_LOG(warning) << "[clipboard] Could not save the desktop portal's restore token to "sv << token_file_.string();
          }
          return;
        }
        restore_token_.clear();
        std::error_code ec;
        std::filesystem::remove(token_file_, ec);
        BOOST_LOG(info) << "[clipboard] The host will ask again for clipboard access when Sunshine restarts; \"Remember this selection\" in its dialog keeps the permission"sv;
      }

      /**
       * @brief End the session for good and log why.
       *
       * @param message Warning to log.
       * @param close Whether the session is still open on the portal's side.
       */
      void end(const std::string &message, const bool close) {
        std::string session;
        {
          std::lock_guard lock {mutex_};
          if (state_ == state_e::ended) {
            return;
          }
          state_ = state_e::ended;
          own_ = false;
          served_.reset();
          session = std::exchange(session_, {});
        }
        BOOST_LOG(warning) << message;
        if (close) {
          close_object(session, session_interface);
        }
      }

      /**
       * @brief Handle a change of the clipboard's owner.
       *
       * @param session Session the signal is for.
       * @param is_owner The session itself is the new owner.
       * @param types Native types of the new selection; empty when the clipboard was emptied.
       */
      void on_selection(const std::string &session, const bool is_owner, std::vector<std::string> types) {
        std::uint64_t revision;
        {
          std::lock_guard lock {mutex_};
          if (session.empty() || session != session_) {
            return;
          }
          if (is_owner) {
            own_ = true;
            return;
          }
          if (types.empty() && expect_own_clear_) {
            // The empty clipboard after Sunshine gave up ownership is not announced.
            expect_own_clear_ = false;
            own_ = false;
            return;
          }
          expect_own_clear_ = false;
          own_ = false;
          ++revision_;
          current_mimes_ = std::move(types);
          current_has_files_ = false;
          if (state_ != state_e::active) {
            return;
          }
          revision = revision_;
        }
        announce(revision);
      }

      /**
       * @brief Report a foreign selection to the observer, once it is known whether it holds files.
       *
       * @param revision Revision to report; nothing happens when the clipboard moved on.
       */
      void announce(const std::uint64_t revision) {
        std::vector<std::string> types;
        std::string session;
        {
          std::lock_guard lock {mutex_};
          if (revision != revision_) {
            return;
          }
          types = current_mimes_;
          session = session_;
        }
        bool files = false;
        if (std::ranges::find(types, clipboard::formats::uri_list) != types.end()) {
          std::string list;
          std::lock_guard read_lock {read_mutex_};
          read_selection(session, std::string {clipboard::formats::uri_list}, max_uri_list, list);
          files = !clipboard::formats::file_paths_from_uri_list(list).empty();
        }

        std::vector<std::string> formats;
        {
          std::lock_guard lock {mutex_};
          if (revision != revision_) {
            return;
          }
          current_has_files_ = files;
          auto offer = clipboard::formats::map_linux_offer(current_mimes_, files);
          if (!offer.sensitive) {
            for (auto &format : offer.formats) {
              formats.push_back(std::move(format.canonical));
            }
          }
        }
        observer_->on_clipboard_changed(revision, std::move(formats));
      }

      /**
       * @brief Hand Sunshine's content for one type to the compositor.
       *
       * @param session Session the request is for.
       * @param type Requested native type.
       * @param serial Serial of the transfer.
       */
      void serve(const std::string &session, const std::string &type, const std::uint32_t serial) {
        std::shared_ptr<const std::string> bytes;
        {
          std::lock_guard lock {mutex_};
          if (session.empty() || session != session_) {
            return;
          }
          if (served_) {
            if (const auto it = std::ranges::find(*served_, type, &clipboard::formats::native_offers_t::value_type::first); it != served_->end()) {
              bytes = it->second;
            }
          }
        }
        g_autoptr(GError) error = nullptr;
        g_autoptr(GUnixFDList) fds = nullptr;
        g_autoptr(GVariant) reply = g_dbus_connection_call_with_unix_fd_list_sync(connection_, portal_name, portal_path, clipboard_interface, "SelectionWrite", g_variant_new("(ou)", session.c_str(), serial), G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, call_timeout_ms, nullptr, &fds, nullptr, &error);
        const auto fd = take_fd(reply, fds);
        if (fd < 0) {
          BOOST_LOG(warning) << "[clipboard] The desktop portal did not take clipboard data: "sv << (error ? error->message : "no pipe");
          write_done(session, serial, false);
          return;
        }
        if (!bytes) {
          // A type Sunshine does not offer ends empty, so the application is not left waiting.
          ::close(fd);
          write_done(session, serial, false);
          return;
        }
        writer_->add(fd, std::move(bytes), [this, session, serial](const bool written) {
          write_done(session, serial, written);
        });
      }

      /**
       * @brief Tell the portal a transfer is over.
       * @details Safe on any thread; it does not wait for the reply.
       *
       * @param session Session handle.
       * @param serial Serial of the transfer.
       * @param success Whether every byte was written.
       */
      void write_done(const std::string &session, const std::uint32_t serial, const bool success) {
        g_dbus_connection_call(connection_, portal_name, portal_path, clipboard_interface, "SelectionWriteDone", g_variant_new("(oub)", session.c_str(), serial, success), nullptr, G_DBUS_CALL_FLAGS_NONE, call_timeout_ms, nullptr, nullptr, nullptr);
      }

      /**
       * @brief Handle the `Response` of the pending request.
       *
       * @param parameters Response code and results.
       * @param data Backend.
       */
      static void on_response_signal(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *parameters, gpointer data) {
        auto *self = static_cast<portal_clipboard_t *>(data);
        g_dbus_connection_signal_unsubscribe(self->connection_, std::exchange(self->response_, 0));
        {
          std::lock_guard lock {self->mutex_};
          self->request_path_.clear();
        }
        const auto on_response = std::exchange(self->on_response_, {});
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})"))) {
          self->end("[clipboard] The desktop portal sent a malformed response", true);
          return;
        }
        std::uint32_t code = 0;
        g_autoptr(GVariant) results = nullptr;
        g_variant_get(parameters, "(u@a{sv})", &code, &results);
        on_response(code, results);
      }

      /**
       * @brief Handle `SelectionOwnerChanged`.
       *
       * @param parameters Session handle and options.
       * @param data Backend.
       */
      static void on_owner_changed(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *parameters, gpointer data) {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(oa{sv})"))) {
          return;
        }
        const gchar *session = nullptr;
        g_autoptr(GVariant) options = nullptr;
        g_variant_get(parameters, "(&o@a{sv})", &session, &options);
        gboolean is_owner = FALSE;
        g_variant_lookup(options, "session_is_owner", "b", &is_owner);
        std::vector<std::string> types;
        g_autofree const gchar **names = nullptr;
        if (g_variant_lookup(options, "mime_types", "^a&s", &names)) {
          for (auto **name = names; *name; ++name) {
            types.emplace_back(*name);
          }
        }
        static_cast<portal_clipboard_t *>(data)->on_selection(session, is_owner, std::move(types));
      }

      /**
       * @brief Handle `SelectionTransfer`, a request for Sunshine's content.
       *
       * @param parameters Session handle, native type and serial.
       * @param data Backend.
       */
      static void on_transfer(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *parameters, gpointer data) {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(osu)"))) {
          return;
        }
        const gchar *session = nullptr;
        const gchar *type = nullptr;
        std::uint32_t serial = 0;
        g_variant_get(parameters, "(&o&su)", &session, &type, &serial);
        static_cast<portal_clipboard_t *>(data)->serve(session, type, serial);
      }

      /**
       * @brief Handle the session's `Closed`.
       *
       * @param path Path of the closed session.
       * @param data Backend.
       */
      static void on_closed(GDBusConnection *, const gchar *, const gchar *path, const gchar *, const gchar *, GVariant *, gpointer data) {
        auto *self = static_cast<portal_clipboard_t *>(data);
        {
          std::lock_guard lock {self->mutex_};
          if (path != self->session_) {
            return;
          }
        }
        self->end("[clipboard] The desktop ended the clipboard session; clipboard sync stays off until Sunshine restarts", false);
      }

      std::filesystem::path token_file_;  ///< File that keeps the restore token.
      platf::clipboard_observer_t *observer_ = nullptr;  ///< Receives foreign changes.
      GDBusConnection *connection_ = nullptr;  ///< Private connection to the session bus.
      GMainContext *context_ = nullptr;  ///< Context the backend's thread iterates.
      GMainLoop *loop_ = nullptr;  ///< Loop of the backend's thread.
      std::thread thread_;  ///< Backend thread.
      std::atomic<bool> started_ {false};  ///< `start` succeeded and `stop` has not run.
      std::unique_ptr<wl::writer_t> writer_;  ///< Writes content to requesting applications.

      bool persist_ = false;  ///< The portal can keep the permission across restarts.
      std::string restore_token_;  ///< Token that restores the last permission; used on the backend's thread after `start`.
      std::string sender_;  ///< Unique bus name in the form request paths use.
      unsigned tokens_ = 0;  ///< Handle tokens handed out.
      guint owner_changed_ = 0;  ///< Subscription to `SelectionOwnerChanged`.
      guint transfer_ = 0;  ///< Subscription to `SelectionTransfer`.
      guint closed_ = 0;  ///< Subscription to the session's `Closed`.
      guint response_ = 0;  ///< Subscription to the pending request's `Response`.
      std::function<void(std::uint32_t, GVariant *)> on_response_;  ///< Handles the pending request's response.

      std::mutex mutex_;  ///< Guards the members below.
      state_e state_ = state_e::starting;  ///< Progress of the session.
      std::string session_;  ///< Session handle; empty before creation and after the end.
      std::string request_path_;  ///< Path of the pending request; empty when none is pending.
      std::uint64_t revision_ = 0;  ///< Revision of the current foreign selection.
      std::vector<std::string> current_mimes_;  ///< Types of the current foreign selection.
      bool current_has_files_ = false;  ///< The current selection's `text/uri-list` holds a local file.
      bool own_ = false;  ///< The clipboard holds Sunshine's content.
      bool expect_own_clear_ = false;  ///< The next empty clipboard is Sunshine giving up ownership.
      std::shared_ptr<const clipboard::formats::native_offers_t> served_;  ///< Content of Sunshine's latest selection, which transfers are served from.

      std::mutex read_mutex_;  ///< Serializes reads, which the compositor serves one at a time.
    };
  }  // namespace

  std::unique_ptr<platf::clipboard_t> make_clipboard(std::filesystem::path token_file) {
    return std::make_unique<portal_clipboard_t>(std::move(token_file));
  }
}  // namespace portal
