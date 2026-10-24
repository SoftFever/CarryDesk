/**
 * @file tests/unit/platform/linux/test_clipboard_portal.cpp
 * @brief Test src/platform/linux/clipboard_portal.* against a fake desktop portal on a private bus.
 * @details The suite starts its own `dbus-daemon` and serves a fake `org.freedesktop.portal.Desktop`
 *          on it, so it never reaches the desktop's portal or clipboard. Restore tokens go to a
 *          temporary directory. The suite is skipped when `dbus-daemon` is not installed.
 */
#ifdef SUNSHINE_BUILD_WAYLAND

  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <condition_variable>
  #include <csignal>
  #include <cstdlib>
  #include <fcntl.h>
  #include <filesystem>
  #include <fstream>
  #include <functional>
  #include <future>
  #include <map>
  #include <mutex>
  #include <optional>
  #include <poll.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>

  // lib includes
  #include <gio/gio.h>
  #include <gio/gunixfdlist.h>

  // local includes
  #include <src/clipboard/protocol.h>
  #include <src/platform/linux/clipboard_portal.h>

using namespace std::chrono_literals;

namespace {
  const std::string text_mime {clipboard::mime::text};
  const std::string html_mime {clipboard::mime::html};
  const std::string png_mime {clipboard::mime::png};
  const std::string files_mime {clipboard::mime::files};
  const std::string text_native {"text/plain;charset=utf-8"};

  constexpr auto portal_path = "/org/freedesktop/portal/desktop";
  constexpr auto clipboard_interface = "org.freedesktop.portal.Clipboard";

  /**
   * @brief Introspection data of the fake portal's interfaces.
   */
  constexpr auto portal_xml = R"(<node>
  <interface name='org.freedesktop.portal.RemoteDesktop'>
    <method name='CreateSession'><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>
    <method name='SelectDevices'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>
    <method name='Start'><arg type='o' direction='in'/><arg type='s' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>
    <property name='version' type='u' access='read'/>
  </interface>
  <interface name='org.freedesktop.portal.Clipboard'>
    <method name='RequestClipboard'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/></method>
    <method name='SetSelection'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/></method>
    <method name='SelectionWrite'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='h' direction='out'/></method>
    <method name='SelectionWriteDone'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='b' direction='in'/></method>
    <method name='SelectionRead'><arg type='o' direction='in'/><arg type='s' direction='in'/><arg type='h' direction='out'/></method>
    <property name='version' type='u' access='read'/>
  </interface>
  <interface name='org.freedesktop.portal.Session'><method name='Close'/></interface>
  <interface name='org.freedesktop.portal.Request'><method name='Close'/></interface>
</node>)";

  /**
   * @brief Check whether a program is on the PATH.
   *
   * @param name Program name.
   * @return `true` when found.
   */
  bool have_program(const std::string &name) {
    return std::system(("command -v " + name + " > /dev/null 2>&1").c_str()) == 0;
  }

  /**
   * @brief Wait until a condition holds.
   *
   * @param condition Condition to poll.
   * @param timeout Longest wait.
   * @return `true` when the condition held in time.
   */
  bool eventually(const std::function<bool()> &condition, const std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(10ms);
    }
    return true;
  }

  /**
   * @brief Read a descriptor until EOF.
   *
   * @param fd Descriptor; closed when done.
   * @return Bytes read, or no value when the writer stalled for a second.
   */
  std::optional<std::string> read_all(const int fd) {
    std::string bytes;
    char buffer[65536];
    while (true) {
      pollfd pfd {fd, POLLIN, 0};
      if (poll(&pfd, 1, 1000) <= 0) {
        ::close(fd);
        return std::nullopt;
      }
      const auto size = ::read(fd, buffer, sizeof(buffer));
      if (size <= 0) {
        ::close(fd);
        return bytes;
      }
      bytes.append(buffer, static_cast<std::size_t>(size));
    }
  }

  /**
   * @brief Read the first line a process writes.
   *
   * @param fd Read end of the process's output; closed when done.
   * @return Line without its newline, or no value when none arrived within ten seconds.
   */
  std::optional<std::string> read_line(const int fd) {
    std::string line;
    char c;
    while (true) {
      pollfd pfd {fd, POLLIN, 0};
      if (poll(&pfd, 1, 10000) <= 0 || ::read(fd, &c, 1) != 1) {
        ::close(fd);
        return std::nullopt;
      }
      if (c == '\n') {
        ::close(fd);
        return line;
      }
      line += c;
    }
  }

  /**
   * @brief Observer that records reported changes.
   */
  class recorder_t: public platf::clipboard_observer_t {
  public:
    void on_clipboard_changed(std::uint64_t revision, std::vector<std::string> formats) override {
      std::lock_guard lock {mutex};
      changes.emplace_back(revision, std::move(formats));
      changed.notify_all();
    }

    /**
     * @brief Wait for a number of reported changes.
     *
     * @param count Changes to wait for in total.
     * @param timeout Longest wait.
     * @return The latest change, or no value on timeout.
     */
    std::optional<std::pair<std::uint64_t, std::vector<std::string>>> wait_for(std::size_t count, std::chrono::milliseconds timeout = 5s) {
      std::unique_lock lock {mutex};
      if (!changed.wait_for(lock, timeout, [&]() {
            return changes.size() >= count;
          })) {
        return std::nullopt;
      }
      return changes.back();
    }

    /**
     * @brief Get the number of reported changes.
     *
     * @return Change count.
     */
    std::size_t count() {
      std::lock_guard lock {mutex};
      return changes.size();
    }

  private:
    std::mutex mutex;  ///< Guards `changes`.
    std::condition_variable changed;  ///< Signalled on each change.
    std::vector<std::pair<std::uint64_t, std::vector<std::string>>> changes;  ///< Reported changes.
  };

  /**
   * @brief Check whether a list contains a value.
   *
   * @param list List to search.
   * @param value Value to find.
   * @return `true` when found.
   */
  bool contains(const std::vector<std::string> &list, const std::string &value) {
    return std::ranges::find(list, value) != list.end();
  }

  /**
   * @brief Fake desktop portal with the remote desktop and clipboard interfaces.
   * @details It stands in for the portal and the compositor behind it: it holds one foreign
   *          application's selection or the session's own, serves reads of the foreign one, and
   *          pastes request the session's content. It answers on a thread of its own and, like
   *          GNOME's portal, reports a new owner only after `SetSelection` returned.
   */
  class fake_portal_t {
  public:
    /**
     * @brief Behavior of the portal, set before a backend starts.
     */
    struct config_t {
      std::uint32_t clipboard_version = 1;  ///< Version of the clipboard interface.
      std::uint32_t remote_desktop_version = 2;  ///< Version of the remote desktop interface.
      std::uint32_t start_code = 0;  ///< Response code of `Start`.
      bool grant_clipboard = true;  ///< The user left clipboard access on.
      std::string restore_token;  ///< Restore token `Start` returns; none when empty.
      bool hold_start = false;  ///< `Start` waits for `answer_start`, like an open dialog.
      bool fail_reads = false;  ///< `SelectionRead` fails.
      bool fail_set_selection = false;  ///< `SetSelection` fails.
    };

    /**
     * @brief Serve the portal on the bus in `DBUS_SESSION_BUS_ADDRESS`.
     *
     * @param config Behavior.
     */
    explicit fake_portal_t(config_t config):
        config_ {std::move(config)} {
      context_ = g_main_context_new();
      loop_ = g_main_loop_new(context_, FALSE);
      connection_ = g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"), GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, nullptr);
      node_ = g_dbus_node_info_new_for_xml(portal_xml, nullptr);
      std::promise<void> ready;
      thread_ = std::thread([this, &ready]() {
        g_main_context_push_thread_default(context_);
        register_object(portal_path, "org.freedesktop.portal.RemoteDesktop");
        register_object(portal_path, clipboard_interface);
        g_autoptr(GVariant) reply = g_dbus_connection_call_sync(connection_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.freedesktop.portal.Desktop", 4u), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
        ready.set_value();
        g_main_loop_run(loop_);
        for (const auto id : registrations_) {
          g_dbus_connection_unregister_object(connection_, id);
        }
        g_main_context_pop_thread_default(context_);
      });
      ready.get_future().wait();
    }

    fake_portal_t(const fake_portal_t &) = delete;
    fake_portal_t &operator=(const fake_portal_t &) = delete;

    /**
     * @brief Stop serving and drop the bus name.
     */
    ~fake_portal_t() {
      g_main_context_invoke(
        context_,
        [](gpointer loop) {
          g_main_loop_quit(static_cast<GMainLoop *>(loop));
          return G_SOURCE_REMOVE;
        },
        loop_
      );
      thread_.join();
      for (auto &writer : writers_) {
        writer.join();
      }
      g_dbus_connection_close_sync(connection_, nullptr, nullptr);
      g_object_unref(connection_);
      g_dbus_node_info_unref(node_);
      g_main_loop_unref(loop_);
      g_main_context_unref(context_);
    }

    /**
     * @brief Let a foreign application put content on the clipboard.
     *
     * @param content Native type to bytes; empty to empty the clipboard.
     * @param session Session the change is reported to; the backend's when empty.
     */
    void copy(std::map<std::string, std::string> content, std::string session = {}) {
      // The change is made on the portal's thread so it follows the reports of earlier calls, as
      // a compositor's owner changes do.
      on_portal_thread([&]() {
        std::vector<std::string> types;
        for (const auto &[type, bytes] : content) {
          types.push_back(type);
        }
        bool enabled;
        {
          std::lock_guard lock {mutex_};
          foreign_ = std::move(content);
          own_ = false;
          enabled = clipboard_enabled_;
          if (session.empty()) {
            session = session_;
          }
        }
        if (enabled) {
          emit_owner_changed(session, types, false);
        }
      });
    }

    /**
     * @brief Let an application paste one type of the session's content.
     *
     * @param type Native type.
     * @return Pasted bytes, or no value when the session did not hand any over.
     */
    std::optional<std::string> paste(const std::string &type) {
      std::uint32_t serial;
      std::string session;
      {
        std::lock_guard lock {mutex_};
        serial = ++serial_;
        transfers_[serial] = -1;
        session = session_;
      }
      emit(portal_path, clipboard_interface, "SelectionTransfer", g_variant_new("(osu)", session.c_str(), type.c_str(), serial));
      int fd;
      {
        std::unique_lock lock {mutex_};
        if (!changed_.wait_for(lock, 5s, [&]() {
              return transfers_[serial] >= 0;
            })) {
          transfers_.erase(serial);
          return std::nullopt;
        }
        fd = transfers_[serial];
        transfers_.erase(serial);
      }
      return read_all(fd);
    }

    /**
     * @brief Answer a held `Start`, as when the user answers the dialog.
     */
    void answer_start() {
      std::string request;
      {
        std::lock_guard lock {mutex_};
        request = std::exchange(held_request_, {});
      }
      finish_start(request);
    }

    /**
     * @brief Close the session from the desktop's side.
     */
    void close_session() {
      std::string session;
      {
        std::lock_guard lock {mutex_};
        session = session_;
      }
      emit(session, "org.freedesktop.portal.Session", "Closed", g_variant_new("(a{sv})", nullptr));
    }

    /**
     * @brief Wait until the portal's state satisfies a condition.
     *
     * @param condition Condition on the portal, checked with its state locked.
     * @return `true` when the condition held within five seconds.
     */
    bool wait(const std::function<bool(const fake_portal_t &)> &condition) {
      std::unique_lock lock {mutex_};
      return changed_.wait_for(lock, 5s, [&]() {
        return condition(*this);
      });
    }

    /**
     * @brief Read the portal's state with it locked.
     *
     * @param reader Reader.
     */
    void inspect(const std::function<void(const fake_portal_t &)> &reader) {
      std::lock_guard lock {mutex_};
      reader(*this);
    }

    std::optional<std::uint32_t> selected_types;  ///< Device types the session asked for.
    std::optional<std::uint32_t> persist_mode;  ///< Persist mode the session asked for.
    std::optional<std::string> received_token;  ///< Restore token the session offered.
    bool start_received = false;  ///< `Start` was called.
    bool session_closed = false;  ///< The backend closed the session.
    bool request_closed = false;  ///< The backend closed a pending request.
    std::vector<std::string> offered;  ///< Types of the session's latest selection.
    int set_selections = 0;  ///< Selections the session set.
    int unsets = 0;  ///< Times the session gave up ownership.
    std::vector<std::pair<std::uint32_t, bool>> writes_done;  ///< Finished transfers by serial.

  private:
    /**
     * @brief Run a task on the portal's thread and wait for it.
     *
     * @param task Task to run.
     */
    void on_portal_thread(const std::function<void()> &task) {
      std::promise<void> done;
      std::pair<const std::function<void()> *, std::promise<void> *> job {&task, &done};
      g_main_context_invoke(
        context_,
        [](gpointer data) {
          const auto *job = static_cast<std::pair<const std::function<void()> *, std::promise<void> *> *>(data);
          (*job->first)();
          job->second->set_value();
          return G_SOURCE_REMOVE;
        },
        &job
      );
      done.get_future().wait();
    }

    /**
     * @brief Export one interface of the portal at a path.
     *
     * @param path Object path.
     * @param interface Interface name.
     */
    void register_object(const std::string &path, const char *interface) {
      static const GDBusInterfaceVTable vtable {&on_method, &on_get_property, nullptr, {}};
      registrations_.push_back(g_dbus_connection_register_object(connection_, path.c_str(), g_dbus_node_info_lookup_interface(node_, interface), &vtable, this, nullptr, nullptr));
    }

    /**
     * @brief Emit a signal to the backend.
     *
     * @param path Emitting object.
     * @param interface Interface of the signal.
     * @param name Signal name.
     * @param parameters Arguments; floating references are consumed.
     */
    void emit(const std::string &path, const char *interface, const char *name, GVariant *parameters) {
      std::string client;
      {
        std::lock_guard lock {mutex_};
        client = client_;
      }
      g_dbus_connection_emit_signal(connection_, client.c_str(), path.c_str(), interface, name, parameters, nullptr);
    }

    /**
     * @brief Report a new clipboard owner.
     *
     * @param session Session the change is reported to.
     * @param types Types of the new selection.
     * @param is_owner The session is the new owner.
     */
    void emit_owner_changed(const std::string &session, const std::vector<std::string> &types, const bool is_owner) {
      GVariantBuilder options;
      g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
      if (!types.empty()) {
        std::vector<const gchar *> names;
        for (const auto &type : types) {
          names.push_back(type.c_str());
        }
        g_variant_builder_add(&options, "{sv}", "mime_types", g_variant_new_strv(names.data(), static_cast<gssize>(names.size())));
      }
      g_variant_builder_add(&options, "{sv}", "session_is_owner", g_variant_new_boolean(is_owner));
      emit(portal_path, clipboard_interface, "SelectionOwnerChanged", g_variant_new("(oa{sv})", session.c_str(), &options));
    }

    /**
     * @brief Answer `Start` and enable the clipboard if the user allowed it.
     *
     * @param request Request path of `Start`.
     */
    void finish_start(const std::string &request) {
      GVariantBuilder results;
      g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add(&results, "{sv}", "devices", g_variant_new_uint32(0));
      std::vector<std::string> types;
      bool enabled;
      std::string session;
      {
        std::lock_guard lock {mutex_};
        enabled = config_.start_code == 0 && config_.grant_clipboard && clipboard_requested_;
        clipboard_enabled_ = enabled;
        session = session_;
        for (const auto &[type, bytes] : foreign_) {
          types.push_back(type);
        }
      }
      if (config_.start_code == 0) {
        g_variant_builder_add(&results, "{sv}", "clipboard_enabled", g_variant_new_boolean(enabled));
        if (!config_.restore_token.empty()) {
          g_variant_builder_add(&results, "{sv}", "restore_token", g_variant_new_string(config_.restore_token.c_str()));
        }
      }
      // Like Mutter, report the current owner while the session starts.
      if (enabled && !types.empty()) {
        emit_owner_changed(session, types, false);
      }
      emit(request, "org.freedesktop.portal.Request", "Response", g_variant_new("(ua{sv})", config_.start_code, &results));
    }

    /**
     * @brief Reply with a descriptor.
     *
     * @param invocation Method call.
     * @param fd Descriptor; closed here.
     */
    static void return_fd(GDBusMethodInvocation *invocation, const int fd) {
      g_autoptr(GUnixFDList) fds = g_unix_fd_list_new();
      const auto index = g_unix_fd_list_append(fds, fd, nullptr);
      ::close(fd);
      g_dbus_method_invocation_return_value_with_unix_fd_list(invocation, g_variant_new("(h)", index), fds);
    }

    /**
     * @brief Handle a method call on the portal's thread.
     *
     * @param sender Caller's unique name.
     * @param path Called object.
     * @param method Method name.
     * @param parameters Arguments.
     * @param invocation Method call.
     */
    void handle(const std::string &sender, const std::string &path, const std::string &method, GVariant *parameters, GDBusMethodInvocation *invocation) {
      std::string escaped = sender.substr(1);
      std::ranges::replace(escaped, '.', '_');
      const auto request_for = [&](GVariant *options) {
        const gchar *token = "";
        g_variant_lookup(options, "handle_token", "&s", &token);
        return "/org/freedesktop/portal/desktop/request/" + escaped + "/" + token;
      };
      std::unique_lock lock {mutex_};

      if (method == "Close") {
        (path == session_ ? session_closed : request_closed) = true;
        changed_.notify_all();
        lock.unlock();
        g_dbus_method_invocation_return_value(invocation, nullptr);
      } else if (method == "CreateSession") {
        g_autoptr(GVariant) options = g_variant_get_child_value(parameters, 0);
        const gchar *token = "";
        g_variant_lookup(options, "session_handle_token", "&s", &token);
        client_ = sender;
        session_ = "/org/freedesktop/portal/desktop/session/" + escaped + "/" + token;
        const auto session = session_;
        lock.unlock();
        register_object(session, "org.freedesktop.portal.Session");
        const auto request = request_for(options);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request.c_str()));
        GVariantBuilder results;
        g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&results, "{sv}", "session_handle", g_variant_new_string(session.c_str()));
        emit(request, "org.freedesktop.portal.Request", "Response", g_variant_new("(ua{sv})", 0u, &results));
      } else if (method == "SelectDevices") {
        g_autoptr(GVariant) options = g_variant_get_child_value(parameters, 1);
        std::uint32_t value;
        if (g_variant_lookup(options, "types", "u", &value)) {
          selected_types = value;
        }
        if (g_variant_lookup(options, "persist_mode", "u", &value)) {
          persist_mode = value;
        }
        if (const gchar *token; g_variant_lookup(options, "restore_token", "&s", &token)) {
          received_token = token;
        }
        lock.unlock();
        const auto request = request_for(options);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request.c_str()));
        emit(request, "org.freedesktop.portal.Request", "Response", g_variant_new("(ua{sv})", 0u, nullptr));
      } else if (method == "Start") {
        g_autoptr(GVariant) options = g_variant_get_child_value(parameters, 2);
        const auto request = request_for(options);
        start_received = true;
        changed_.notify_all();
        if (config_.hold_start) {
          held_request_ = request;
        }
        lock.unlock();
        if (config_.hold_start) {
          register_object(request, "org.freedesktop.portal.Request");
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request.c_str()));
        if (!config_.hold_start) {
          finish_start(request);
        }
      } else if (method == "RequestClipboard") {
        clipboard_requested_ = true;
        lock.unlock();
        g_dbus_method_invocation_return_value(invocation, nullptr);
      } else if (method == "SetSelection") {
        if (config_.fail_set_selection) {
          lock.unlock();
          g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.portal.Error.Failed", "Failed to set selection");
          return;
        }
        g_autoptr(GVariant) options = g_variant_get_child_value(parameters, 1);
        std::vector<std::string> types;
        g_autofree const gchar **names = nullptr;
        if (g_variant_lookup(options, "mime_types", "^a&s", &names)) {
          for (auto **name = names; *name; ++name) {
            types.emplace_back(*name);
          }
        }
        const auto had_ownership = own_;
        if (!types.empty()) {
          offered = types;
          own_ = true;
          foreign_.clear();
          ++set_selections;
        } else if (own_) {
          own_ = false;
          ++unsets;
        }
        const auto session = session_;
        changed_.notify_all();
        lock.unlock();
        g_dbus_method_invocation_return_value(invocation, nullptr);
        if (!types.empty() || had_ownership) {
          emit_owner_changed(session, types, !types.empty());
        }
      } else if (method == "SelectionWrite") {
        std::uint32_t serial;
        g_variant_get(parameters, "(&ou)", nullptr, &serial);
        int fds[2];
        if (!transfers_.contains(serial) || pipe2(fds, O_CLOEXEC) != 0) {
          lock.unlock();
          g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.portal.Error.Failed", "No such transfer");
          return;
        }
        transfers_[serial] = fds[0];
        changed_.notify_all();
        lock.unlock();
        return_fd(invocation, fds[1]);
      } else if (method == "SelectionWriteDone") {
        std::uint32_t serial;
        gboolean success;
        g_variant_get(parameters, "(&oub)", nullptr, &serial, &success);
        writes_done.emplace_back(serial, success);
        changed_.notify_all();
        lock.unlock();
        g_dbus_method_invocation_return_value(invocation, nullptr);
      } else if (method == "SelectionRead") {
        const gchar *type;
        g_variant_get(parameters, "(&o&s)", nullptr, &type);
        const auto it = foreign_.find(type);
        int fds[2];
        if (config_.fail_reads || own_ || it == foreign_.end() || pipe2(fds, O_CLOEXEC) != 0) {
          lock.unlock();
          g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.portal.Error.Failed", "Cannot read");
          return;
        }
        writers_.emplace_back([fd = fds[1], bytes = it->second]() {
          sigset_t pipe_signal;
          sigemptyset(&pipe_signal);
          sigaddset(&pipe_signal, SIGPIPE);
          pthread_sigmask(SIG_BLOCK, &pipe_signal, nullptr);
          std::size_t offset = 0;
          while (offset < bytes.size()) {
            const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (written <= 0) {
              break;
            }
            offset += static_cast<std::size_t>(written);
          }
          ::close(fd);
        });
        lock.unlock();
        return_fd(invocation, fds[0]);
      }
    }

    static void on_method(GDBusConnection *, const gchar *sender, const gchar *path, const gchar *, const gchar *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
      static_cast<fake_portal_t *>(data)->handle(sender, path, method, parameters, invocation);
    }

    static GVariant *on_get_property(GDBusConnection *, const gchar *, const gchar *, const gchar *interface, const gchar *, GError **, gpointer data) {
      const auto &config = static_cast<fake_portal_t *>(data)->config_;
      return g_variant_new_uint32(std::string_view {interface} == clipboard_interface ? config.clipboard_version : config.remote_desktop_version);
    }

    config_t config_;  ///< Behavior.
    GMainContext *context_ = nullptr;  ///< Context of the portal's thread.
    GMainLoop *loop_ = nullptr;  ///< Loop of the portal's thread.
    GDBusConnection *connection_ = nullptr;  ///< Connection that owns the portal's name.
    GDBusNodeInfo *node_ = nullptr;  ///< Introspection data.
    std::vector<guint> registrations_;  ///< Exported objects; used on the portal's thread.
    std::thread thread_;  ///< Portal thread.
    std::vector<std::thread> writers_;  ///< Threads that write foreign content into reads.

    std::mutex mutex_;  ///< Guards the state below and the public observations.
    std::condition_variable changed_;  ///< Signalled when the state changes.
    std::string client_;  ///< Unique name of the backend.
    std::string session_;  ///< Session handle.
    std::string held_request_;  ///< Request path of a held `Start`.
    bool clipboard_requested_ = false;  ///< The session asked for the clipboard.
    bool clipboard_enabled_ = false;  ///< The session has clipboard access.
    std::map<std::string, std::string> foreign_;  ///< Foreign selection, by native type.
    bool own_ = false;  ///< The session owns the clipboard.
    std::uint32_t serial_ = 0;  ///< Last transfer serial.
    std::map<std::uint32_t, int> transfers_;  ///< Read ends of pending pastes by serial; -1 until written.
  };

  /**
   * @brief Run the backend against a fake portal on a private bus.
   */
  class PortalClipboardTest: public testing::Test {
  protected:
    /**
     * @brief Start the private bus once for the suite.
     */
    static void SetUpTestSuite() {
      if (!have_program("dbus-daemon")) {
        return;
      }
      if (const auto *address = std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
        original_address = address;
      }
      bus_dir = std::filesystem::temp_directory_path() / ("sunshine-portal-test-" + std::to_string(getpid()));
      std::filesystem::create_directories(bus_dir);
      const auto config = bus_dir / "bus.conf";
      std::ofstream {config} << "<busconfig><type>session</type><listen>unix:tmpdir=" << bus_dir.string() << "</listen>"
                             << "<policy context='default'><allow send_destination='*' eavesdrop='true'/><allow eavesdrop='true'/><allow own='*'/></policy></busconfig>";
      int out[2];
      if (pipe(out) != 0) {
        return;
      }
      daemon = fork();
      if (daemon == 0) {
        setsid();
        dup2(out[1], STDOUT_FILENO);
        const auto null_fd = open("/dev/null", O_RDWR);
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDERR_FILENO);
        execlp("dbus-daemon", "dbus-daemon", "--nofork", "--print-address=1", ("--config-file=" + config.string()).c_str(), nullptr);
        _exit(127);
      }
      ::close(out[1]);
      if (const auto address = read_line(out[0]); address && !address->empty()) {
        setenv("DBUS_SESSION_BUS_ADDRESS", address->c_str(), 1);
        ready = true;
      }
    }

    /**
     * @brief Stop the private bus and restore the environment.
     */
    static void TearDownTestSuite() {
      if (daemon > 0) {
        kill(daemon, SIGTERM);
        waitpid(daemon, nullptr, 0);
        daemon = -1;
      }
      if (original_address) {
        setenv("DBUS_SESSION_BUS_ADDRESS", original_address->c_str(), 1);
      } else {
        unsetenv("DBUS_SESSION_BUS_ADDRESS");
      }
      std::error_code ec;
      std::filesystem::remove_all(bus_dir, ec);
      ready = false;
    }

    /**
     * @brief Skip without a private bus.
     */
    void SetUp() override {
      if (!ready) {
        GTEST_SKIP() << "dbus-daemon is not available";
      }
      std::error_code ec;
      std::filesystem::remove(token_file(), ec);
    }

    /**
     * @brief Stop the backend before the portal goes away.
     */
    void TearDown() override {
      if (backend) {
        backend->stop();
      }
      backend.reset();
      fake.reset();
      std::error_code ec;
      std::filesystem::remove(token_file(), ec);
    }

    /**
     * @brief Get the file the backend keeps its restore token in.
     *
     * @return Path in the suite's temporary directory.
     */
    static std::filesystem::path token_file() {
      return bus_dir / "clipboard_portal_token";
    }

    /**
     * @brief Serve a portal and start a backend against it.
     *
     * @param config Behavior of the portal.
     * @return Result of `start`.
     */
    bool start(fake_portal_t::config_t config = {}) {
      if (!fake) {
        fake = std::make_unique<fake_portal_t>(std::move(config));
      }
      backend = portal::make_clipboard(token_file());
      return backend->start(recorder);
    }

    /**
     * @brief Start a backend with some text on the clipboard and wait until access is allowed.
     */
    void start_allowed() {
      fake = std::make_unique<fake_portal_t>(fake_portal_t::config_t {});
      fake->copy({{text_native, "initial"}});
      ASSERT_TRUE(start());
      ASSERT_TRUE(recorder.wait_for(1));
    }

    /**
     * @brief Let a foreign application copy and wait for the backend to report it.
     *
     * @param content Native type to bytes.
     * @return The reported change.
     */
    std::pair<std::uint64_t, std::vector<std::string>> copy(std::map<std::string, std::string> content) {
      const auto before = recorder.count();
      fake->copy(std::move(content));
      const auto change = recorder.wait_for(before + 1);
      EXPECT_TRUE(change);
      return change.value_or(std::pair<std::uint64_t, std::vector<std::string>> {});
    }

    static inline pid_t daemon = -1;  ///< Private bus daemon.
    static inline bool ready = false;  ///< The private bus runs.
    static inline std::filesystem::path bus_dir;  ///< Directory of the bus socket and restore tokens.
    static inline std::optional<std::string> original_address;  ///< `DBUS_SESSION_BUS_ADDRESS` before the suite.

    recorder_t recorder;  ///< Reported changes.
    std::unique_ptr<fake_portal_t> fake;  ///< Portal the backend talks to.
    std::unique_ptr<platf::clipboard_t> backend;  ///< Backend under test.
  };
}  // namespace

TEST_F(PortalClipboardTest, ForeignTextIsReportedAndReadAsCanonicalText) {
  start_allowed();
  const auto [revision, formats] = copy({{text_native, "h\xC3\xA9llo\r\nworld\rend"}});
  EXPECT_TRUE(contains(formats, text_mime));
  const auto read = backend->read(revision, text_mime, 1024);
  EXPECT_EQ(read.result, platf::clipboard_result_e::ok);
  EXPECT_EQ(read.bytes, "h\xC3\xA9llo\nworld\nend");
}

TEST_F(PortalClipboardTest, SelectionAtStartIsReportedOnceAccessIsAllowed) {
  fake = std::make_unique<fake_portal_t>(fake_portal_t::config_t {.hold_start = true});
  fake->copy({{text_native, "waiting"}});
  ASSERT_TRUE(start());
  ASSERT_TRUE(fake->wait([](const auto &portal) {
    return portal.start_received;
  }));
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(recorder.count(), 0u);
  fake->answer_start();
  const auto change = recorder.wait_for(1);
  ASSERT_TRUE(change);
  EXPECT_EQ(backend->read(change->first, text_mime, 1024).bytes, "waiting");
}

TEST_F(PortalClipboardTest, RichFormatsAreReadWithinTheLimit) {
  start_allowed();
  auto change = copy({{"text/html", "<b>bold</b>"}});
  EXPECT_TRUE(contains(change.second, html_mime));
  EXPECT_EQ(backend->read(change.first, html_mime, 1024).bytes, "<b>bold</b>");

  const std::string png {"\x89PNG\r\n\x1A\nbinary"};
  change = copy({{"image/png", png}});
  EXPECT_TRUE(contains(change.second, png_mime));
  EXPECT_EQ(backend->read(change.first, png_mime, 1024).bytes, png);
  EXPECT_EQ(backend->read(change.first, png_mime, 4).result, platf::clipboard_result_e::too_large);
  EXPECT_EQ(backend->read(change.first, text_mime, 1024).result, platf::clipboard_result_e::stale);
  EXPECT_EQ(backend->read(change.first - 1, png_mime, 1024).result, platf::clipboard_result_e::stale);
}

TEST_F(PortalClipboardTest, PasswordManagerContentIsNotShared) {
  start_allowed();
  const auto [revision, formats] = copy({{text_native, "secret"}, {"x-kde-passwordManagerHint", "secret"}});
  EXPECT_TRUE(formats.empty());
  EXPECT_EQ(backend->read(revision, text_mime, 1024).result, platf::clipboard_result_e::stale);
}

TEST_F(PortalClipboardTest, UriListIsFilesOnlyWithLocalFiles) {
  start_allowed();
  auto change = copy({{"text/uri-list", "file:///tmp/a%20b\r\nhttps://example.com/x\r\n"}});
  EXPECT_TRUE(contains(change.second, files_mime));
  const auto read = backend->read(change.first, files_mime, 1 << 20);
  EXPECT_EQ(read.result, platf::clipboard_result_e::ok);
  EXPECT_EQ(read.paths, std::vector<std::string> {"/tmp/a b"});

  change = copy({{"text/uri-list", "https://example.com/x\r\n"}});
  EXPECT_FALSE(contains(change.second, files_mime));
}

TEST_F(PortalClipboardTest, FailedReadsAreErrors) {
  fake = std::make_unique<fake_portal_t>(fake_portal_t::config_t {.fail_reads = true});
  fake->copy({{text_native, "unreadable"}});
  ASSERT_TRUE(start());
  const auto change = recorder.wait_for(1);
  ASSERT_TRUE(change);
  EXPECT_EQ(backend->read(change->first, text_mime, 1024).result, platf::clipboard_result_e::io_error);
}

TEST_F(PortalClipboardTest, OwnershipServesEveryOfferedTypeWithoutEcho) {
  start_allowed();
  const auto [revision, formats] = copy({{text_native, "before"}});
  const std::map<std::string, std::string, std::less<>> content {
    {text_mime, "from the client"},
    {html_mime, "<i>client</i>"},
    {png_mime, "PNGBYTES"},
  };
  const auto before = recorder.count();
  ASSERT_EQ(backend->take_ownership(revision, content, {}), platf::clipboard_result_e::ok);
  EXPECT_TRUE(fake->wait([](const auto &portal) {
    return portal.set_selections == 1;
  }));
  fake->inspect([](const auto &portal) {
    EXPECT_TRUE(contains(portal.offered, "text/plain;charset=utf-8"));
    EXPECT_TRUE(contains(portal.offered, "text/html"));
    EXPECT_TRUE(contains(portal.offered, "image/png"));
  });
  EXPECT_EQ(fake->paste("text/plain;charset=utf-8"), "from the client");
  EXPECT_EQ(fake->paste("text/html"), "<i>client</i>");
  EXPECT_EQ(fake->paste("image/png"), "PNGBYTES");
  EXPECT_TRUE(fake->wait([](const auto &portal) {
    return portal.writes_done.size() == 3 && std::ranges::all_of(portal.writes_done, [](const auto &done) {
             return done.second;
           });
  }));
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(recorder.count(), before);
  EXPECT_EQ(backend->read(revision, text_mime, 1024).result, platf::clipboard_result_e::stale);
}

TEST_F(PortalClipboardTest, UnofferedTypeIsPastedEmpty) {
  start_allowed();
  const auto [revision, formats] = copy({{text_native, "before"}});
  ASSERT_EQ(backend->take_ownership(revision, {{text_mime, "text only"}}, {}), platf::clipboard_result_e::ok);
  EXPECT_EQ(fake->paste("image/png"), "");
  EXPECT_TRUE(fake->wait([](const auto &portal) {
    return portal.writes_done.size() == 1 && !portal.writes_done.front().second;
  }));
}

TEST_F(PortalClipboardTest, StagedFilesAreOfferedAsUriLists) {
  start_allowed();
  const auto [revision, formats] = copy({{text_native, "before"}});
  ASSERT_EQ(backend->take_ownership(revision, {}, {"/s/a b.txt"}), platf::clipboard_result_e::ok);
  EXPECT_EQ(fake->paste("text/uri-list"), "file:///s/a%20b.txt\r\n");
  EXPECT_EQ(fake->paste("x-special/gnome-copied-files"), "copy\nfile:///s/a%20b.txt");
}

TEST_F(PortalClipboardTest, LargeContentIsWrittenCompletely) {
  start_allowed();
  const auto [revision, formats] = copy({{text_native, "before"}});
  const std::string large(5 * 1048576, 'L');
  ASSERT_EQ(backend->take_ownership(revision, {{text_mime, large}}, {}), platf::clipboard_result_e::ok);
  const auto pasted = fake->paste("text/plain;charset=utf-8");
  ASSERT_TRUE(pasted);
  EXPECT_EQ(pasted->size(), large.size());
}

TEST_F(PortalClipboardTest, ForeignCopyAfterOwnershipIsReportedAndWins) {
  start_allowed();
  const auto [revision, formats] = copy({{text_native, "before"}});
  ASSERT_EQ(backend->take_ownership(revision, {{text_mime, "ours"}}, {}), platf::clipboard_result_e::ok);
  const auto change = copy({{text_native, "theirs"}});
  EXPECT_GT(change.first, revision);
  EXPECT_EQ(backend->take_ownership(revision, {{text_mime, "late"}}, {}), platf::clipboard_result_e::stale);
  EXPECT_EQ(backend->read(change.first, text_mime, 1024).bytes, "theirs");
  ASSERT_EQ(backend->take_ownership(change.first, {{text_mime, "again"}}, {}), platf::clipboard_result_e::ok);
  EXPECT_EQ(fake->paste("text/plain;charset=utf-8"), "again");
}

TEST_F(PortalClipboardTest, FailedOwnershipIsAnError) {
  fake = std::make_unique<fake_portal_t>(fake_portal_t::config_t {.fail_set_selection = true});
  fake->copy({{text_native, "before"}});
  ASSERT_TRUE(start());
  const auto change = recorder.wait_for(1);
  ASSERT_TRUE(change);
  EXPECT_EQ(backend->take_ownership(change->first, {{text_mime, "refused"}}, {}), platf::clipboard_result_e::io_error);
  EXPECT_EQ(backend->read(change->first, text_mime, 1024).bytes, "before");
}

TEST_F(PortalClipboardTest, ReleaseEmptiesTheClipboardWithoutAReport) {
  start_allowed();
  const auto [revision, formats] = copy({{text_native, "before"}});
  ASSERT_EQ(backend->take_ownership(revision, {{text_mime, "ours"}}, {}), platf::clipboard_result_e::ok);
  const auto before = recorder.count();
  backend->release();
  EXPECT_TRUE(fake->wait([](const auto &portal) {
    return portal.unsets == 1;
  }));
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(recorder.count(), before);
  backend->release();
  const auto after = copy({{text_native, "after"}});
  EXPECT_GT(after.first, revision);
  fake->inspect([](const auto &portal) {
    EXPECT_EQ(portal.unsets, 1);
  });
}

TEST_F(PortalClipboardTest, EmptiedClipboardIsReported) {
  start_allowed();
  const auto [revision, formats] = copy({});
  EXPECT_TRUE(formats.empty());
  EXPECT_EQ(backend->read(revision, text_mime, 1024).result, platf::clipboard_result_e::stale);
}

TEST_F(PortalClipboardTest, ChangesForOtherSessionsAreIgnored) {
  start_allowed();
  const auto before = recorder.count();
  fake->copy({{text_native, "elsewhere"}}, "/org/freedesktop/portal/desktop/session/1_1/other");
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(recorder.count(), before);
}

TEST_F(PortalClipboardTest, SessionAsksForTheKeyboardAndKeepsTheRestoreToken) {
  ASSERT_TRUE(start({.restore_token = "first"}));
  ASSERT_TRUE(eventually([&]() {
    return backend->take_ownership(0, {{text_mime, "x"}}, {}) == platf::clipboard_result_e::ok;
  }));
  fake->inspect([](const auto &portal) {
    EXPECT_EQ(portal.selected_types, 1u);
    EXPECT_EQ(portal.persist_mode, 2u);
    EXPECT_FALSE(portal.received_token);
  });
  backend->stop();
  EXPECT_TRUE(fake->wait([](const auto &portal) {
    return portal.session_closed && portal.unsets == 1;
  }));
  std::ifstream first {token_file()};
  EXPECT_EQ(std::string(std::istreambuf_iterator<char> {first}, {}), "first");

  backend.reset();
  fake.reset();
  ASSERT_TRUE(start({.restore_token = "second"}));
  ASSERT_TRUE(fake->wait([](const auto &portal) {
    return portal.received_token == "first";
  }));
  ASSERT_TRUE(eventually([&]() {
    std::ifstream file {token_file()};
    return std::string(std::istreambuf_iterator<char> {file}, {}) == "second";
  }));
  backend->stop();

  backend.reset();
  fake.reset();
  ASSERT_TRUE(start());
  ASSERT_TRUE(eventually([&]() {
    return !std::filesystem::exists(token_file());
  }));
}

TEST_F(PortalClipboardTest, PortalWithoutPersistenceIsNotAskedToRemember) {
  std::ofstream {token_file()} << "stale";
  ASSERT_TRUE(start({.remote_desktop_version = 1}));
  ASSERT_TRUE(fake->wait([](const auto &portal) {
    return portal.start_received;
  }));
  fake->inspect([](const auto &portal) {
    EXPECT_FALSE(portal.persist_mode);
    EXPECT_FALSE(portal.received_token);
  });
}

TEST_F(PortalClipboardTest, ClipboardIsLeftAloneUntilAccessIsAllowed) {
  ASSERT_TRUE(start({.hold_start = true}));
  ASSERT_TRUE(fake->wait([](const auto &portal) {
    return portal.start_received;
  }));
  EXPECT_EQ(backend->take_ownership(0, {{text_mime, "early"}}, {}), platf::clipboard_result_e::not_permitted);
  EXPECT_EQ(backend->read(0, text_mime, 1024).result, platf::clipboard_result_e::io_error);
  backend->release();
  fake->inspect([](const auto &portal) {
    EXPECT_EQ(portal.set_selections, 0);
  });
}

TEST_F(PortalClipboardTest, DeniedAccessEndsTheSession) {
  ASSERT_TRUE(start({.start_code = 1}));
  EXPECT_TRUE(fake->wait([](const auto &portal) {
    return portal.session_closed;
  }));
  EXPECT_EQ(backend->take_ownership(0, {{text_mime, "denied"}}, {}), platf::clipboard_result_e::not_permitted);
}

TEST_F(PortalClipboardTest, FailedStartEndsTheSession) {
  ASSERT_TRUE(start({.start_code = 2}));
  EXPECT_TRUE(fake->wait([](const auto &portal) {
    return portal.session_closed;
  }));
  EXPECT_EQ(backend->take_ownership(0, {{text_mime, "failed"}}, {}), platf::clipboard_result_e::not_permitted);
}

TEST_F(PortalClipboardTest, ClipboardTurnedOffEndsTheSession) {
  ASSERT_TRUE(start({.grant_clipboard = false}));
  EXPECT_TRUE(fake->wait([](const auto &portal) {
    return portal.session_closed;
  }));
  EXPECT_EQ(backend->take_ownership(0, {{text_mime, "off"}}, {}), platf::clipboard_result_e::not_permitted);
}

TEST_F(PortalClipboardTest, SessionClosedByTheDesktopStopsSharing) {
  start_allowed();
  const auto [revision, formats] = copy({{text_native, "before"}});
  fake->close_session();
  EXPECT_TRUE(eventually([&]() {
    return backend->take_ownership(revision, {{text_mime, "closed"}}, {}) == platf::clipboard_result_e::not_permitted;
  }));
  EXPECT_EQ(backend->read(revision, text_mime, 1024).result, platf::clipboard_result_e::io_error);
}

TEST_F(PortalClipboardTest, StopWhileTheUserDecidesClosesTheRequest) {
  ASSERT_TRUE(start({.hold_start = true}));
  ASSERT_TRUE(fake->wait([](const auto &portal) {
    return portal.start_received;
  }));
  backend->stop();
  fake->inspect([](const auto &portal) {
    EXPECT_TRUE(portal.request_closed);
    EXPECT_TRUE(portal.session_closed);
  });
}

TEST_F(PortalClipboardTest, StartFailsWithoutClipboardPortal) {
  EXPECT_FALSE(start({.clipboard_version = 0}));
  backend->stop();
}

TEST_F(PortalClipboardTest, StartFailsWithoutSessionBus) {
  const std::string address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
  setenv("DBUS_SESSION_BUS_ADDRESS", ("unix:path=" + (bus_dir / "missing").string()).c_str(), 1);
  auto missing = portal::make_clipboard(token_file());
  EXPECT_FALSE(missing->start(recorder));
  setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), 1);
}
#endif
