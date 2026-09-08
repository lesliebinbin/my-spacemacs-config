// emacs-rpc-bridge — a C++20 native module for Emacs.
//
// Registers a small set of native functions plus two load-time demos:
//
//   emacs-rpc-bridge-native-start  SOCKET-FILE
//                                  [QUEUE-DEPTH MAX-PER-TICK
//                                   LEASE-SECONDS MAX-SESSIONS
//                                   MAX-MESSAGE-BYTES]
//   emacs-rpc-bridge-native-drain
//   emacs-rpc-bridge-native-stop
//   emacs-rpc-bridge-native-running-p
//   emacs-rpc-bridge-native-last-error
//
// Only native-start creates anything: loading the module never starts the
// bridge (no automatic startup).  The Elisp adapter (lisp/emacs-rpc-bridge.el)
// owns the drain timer and the defcustoms; the drain timer calls
// native-drain, and stop cancels the timer before calling native-stop.
//
// The server (server.cpp) and its Emacs-thread execution (dispatch.cpp) never
// enter this file; drain/start/stop are thin wrappers over ServerCore.
//
// Build with `mise build` (CMake, see CMakeLists.txt).  The result,
// build/libemacs-rpc-bridge.so, is dlopened by Emacs.

#include <emacs-module.h>  // extern "C" linkage is applied by this header

#include "bridge.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>

// Emacs only loads modules that declare themselves GPL-compatible.  Presence
// of the symbol is what matters, not its value; the initializer turns this
// into a definition (a bare `extern "C" int x;' declaration would emit no
// symbol at all).  The extern "C" block gives the variable C linkage without
// an extern storage-class specifier, keeping the plain C name instead of a
// mangled C++ one.
extern "C" {
int plugin_is_GPL_compatible = 1;
}

namespace {

using emacs_rpc_bridge::ServerCore;

// Lisp symbol prefix under which the functions are registered: the module
// exports emacs-rpc-bridge-native-start, never a bare "native-start" that
// could collide with another package.
const std::string kSymbolPrefix = "emacs-rpc-bridge-";

// One ServerCore per Emacs process, created lazily on the first
// native-start.  A static C++ object in the module would be destroyed only
// at process exit, after which no Emacs API call is legal anyway.
std::unique_ptr<ServerCore> g_server;

const intmax_t kDefaultQueueDepth = 100;      // defcustom default (lisp side)
const intmax_t kDefaultMaxPerTick = 20;       // defcustom default (lisp side)
const intmax_t kDefaultLeaseSeconds = 300;    // idle-session lease (0 = off)
const intmax_t kDefaultMaxSessions = 32;      // session quota (0 = unlimited)
const intmax_t kDefaultMaxMessageBytes = 0;   // 0 = gRPC default (4 MiB)

// ---- exit-state helpers -----------------------------------------------------

bool exit_pending(emacs_env *env) {
  return env->non_local_exit_check(env) != emacs_funcall_exit_return;
}

// Signal MSG by calling (error MSG); leaves a pending non-local exit so the
// module callback that returns nullptr right after propagates the signal.
void signal_error(emacs_env *env, const std::string &msg) {
  emacs_value fn = env->intern(env, "error");
  emacs_value str = env->make_string(env, msg.data(),
                                     static_cast<ptrdiff_t>(msg.size()));
  emacs_value args[] = {str};
  env->funcall(env, fn, 1, args);
}

bool is_lisp_type(emacs_env *env, emacs_value v, const char *name) {
  return env->eq(env, env->type_of(env, v), env->intern(env, name));
}

// ---- string helpers ---------------------------------------------------------

emacs_value make_emacs_string(emacs_env *env, const std::string &s) {
  return env->make_string(env, s.data(), static_cast<ptrdiff_t>(s.size()));
}

std::string emacs_string_arg(emacs_env *env, emacs_value value) {
  // First call with a null buffer asks for the size in bytes, counting the
  // trailing NUL that copy_string_contents always writes.
  ptrdiff_t size = 0;
  if (!env->copy_string_contents(env, value, nullptr, &size)) {
    if (exit_pending(env)) env->non_local_exit_clear(env);
    return {};
  }
  std::string s(static_cast<std::size_t>(size) - 1, '\0');
  env->copy_string_contents(env, value, s.data(), &size);
  return s;
}

// ---- exported functions -----------------------------------------------------
//
// Every function exported by a module has this exact callback shape: the
// environment, the number of arguments actually passed, the argument vector,
// and user data from make_function.  When compiled as C++17 or newer the API
// header types these callbacks `noexcept' ("must not throw C++ exceptions"),
// hence the noexcept below; every body catches its own exceptions.  The
// callbacks are only reachable through emacs_module_init below, so they live
// in this anonymous namespace (internal linkage) instead of polluting the
// .so's symbol table.

emacs_value F_hello(emacs_env *env, ptrdiff_t nargs,
                    emacs_value args[], void *data) noexcept {
  (void)nargs; (void)args; (void)data;
  return make_emacs_string(env, "Hello from a C++20 Emacs module");
}

emacs_value F_shout(emacs_env *env, ptrdiff_t nargs,
                    emacs_value args[], void *data) noexcept {
  (void)nargs; (void)data;
  std::string s = emacs_string_arg(env, args[0]);
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return make_emacs_string(env, s);
}

// Read a fixnum argument; signals wrong-type-argument unless it is one.
intmax_t fixnum_arg(emacs_env *env, emacs_value v, const char *what) {
  if (!is_lisp_type(env, v, "integer")) {
    signal_error(env, std::string(what) + " must be an integer");
    return 0;
  }
  return env->extract_integer(env, v);
}

// (emacs-version) on the Emacs thread; empty string on failure.
std::string emacs_version_string(emacs_env *env) {
  emacs_value fn = env->intern(env, "emacs-version");
  emacs_value out = env->funcall(env, fn, 0, nullptr);
  if (exit_pending(env)) return {};
  return emacs_string_arg(env, out);
}

emacs_value F_native_start(emacs_env *env, ptrdiff_t nargs,
                           emacs_value args[], void *data) noexcept {
  (void)data;
  try {
    if (!is_lisp_type(env, args[0], "string")) {
      signal_error(env, "SOCKET-FILE must be a string");
      return nullptr;
    }
    std::string socket_path = emacs_string_arg(env, args[0]);
    if (socket_path.empty() || socket_path[0] != '/' ||
        socket_path.size() > 100) {
      signal_error(env,
                   "socket path must be an absolute path of at most 100 "
                   "characters (Unix sun_path limit)");
      return nullptr;
    }
    if (g_server && g_server->running()) {
      signal_error(env, "bridge is already running");
      return nullptr;
    }

    intmax_t queue_depth = kDefaultQueueDepth;
    intmax_t max_per_tick = kDefaultMaxPerTick;
    intmax_t lease_seconds = kDefaultLeaseSeconds;
    intmax_t max_sessions = kDefaultMaxSessions;
    intmax_t max_message_bytes = kDefaultMaxMessageBytes;
    if (nargs >= 2) {
      queue_depth = fixnum_arg(env, args[1], "QUEUE-DEPTH");
      if (exit_pending(env)) return nullptr;
      if (queue_depth < 1 || queue_depth > 65536) {
        signal_error(env, "QUEUE-DEPTH out of range (1..65536)");
        return nullptr;
      }
    }
    if (nargs >= 3) {
      max_per_tick = fixnum_arg(env, args[2], "MAX-PER-TICK");
      if (exit_pending(env)) return nullptr;
      if (max_per_tick < 1 || max_per_tick > 4096) {
        signal_error(env, "MAX-PER-TICK out of range (1..4096)");
        return nullptr;
      }
    }
    if (nargs >= 4) {
      lease_seconds = fixnum_arg(env, args[3], "LEASE-SECONDS");
      if (exit_pending(env)) return nullptr;
      if (lease_seconds < 0 || lease_seconds > 86400 * 365) {
        signal_error(env, "LEASE-SECONDS out of range (0..31536000)");
        return nullptr;
      }
    }
    if (nargs >= 5) {
      max_sessions = fixnum_arg(env, args[4], "MAX-SESSIONS");
      if (exit_pending(env)) return nullptr;
      if (max_sessions < 0 || max_sessions > 65536) {
        signal_error(env, "MAX-SESSIONS out of range (0..65536)");
        return nullptr;
      }
    }
    if (nargs >= 6) {
      max_message_bytes = fixnum_arg(env, args[5], "MAX-MESSAGE-BYTES");
      if (exit_pending(env)) return nullptr;
      if (max_message_bytes < 0 ||
          max_message_bytes > static_cast<intmax_t>(1) << 30) {
        signal_error(env, "MAX-MESSAGE-BYTES out of range (0..1073741824)");
        return nullptr;
      }
    }

    std::string version = emacs_version_string(env);
    if (!g_server) g_server = std::make_unique<ServerCore>();
    bool started = g_server->start(socket_path,
                                   static_cast<size_t>(queue_depth),
                                   static_cast<size_t>(max_per_tick),
                                   version,
                                   lease_seconds,
                                   static_cast<size_t>(max_sessions),
                                   static_cast<size_t>(max_message_bytes));
    if (!started) {
      signal_error(env, "bridge could not start (already running)");
      return nullptr;
    }
    return env->intern(env, "t");
  } catch (const std::exception &e) {
    signal_error(env, e.what());
  }
  return nullptr;
}

emacs_value F_native_drain(emacs_env *env, ptrdiff_t nargs,
                           emacs_value args[], void *data) noexcept {
  (void)nargs; (void)args; (void)data;
  size_t n = g_server ? g_server->drain_once(env) : 0;
  return env->make_integer(env, static_cast<intmax_t>(n));
}

emacs_value F_native_stop(emacs_env *env, ptrdiff_t nargs,
                          emacs_value args[], void *data) noexcept {
  (void)nargs; (void)args; (void)data;
  if (g_server) g_server->stop_and_join(env);
  return env->intern(env, "t");
}

emacs_value F_native_running_p(emacs_env *env, ptrdiff_t nargs,
                               emacs_value args[], void *data) noexcept {
  (void)nargs; (void)args; (void)data;
  bool running = g_server && g_server->running();
  return env->intern(env, running ? "t" : "nil");
}

emacs_value F_native_last_error(emacs_env *env, ptrdiff_t nargs,
                                emacs_value args[], void *data) noexcept {
  (void)nargs; (void)args; (void)data;
  std::string err = g_server ? g_server->last_error() : "";
  return err.empty() ? env->intern(env, "nil") : make_emacs_string(env, err);
}

// ---- registration -----------------------------------------------------------
//
// A module has no dedicated registration table: functions are added by
// calling ordinary Lisp, exactly as in the sample module of the Emacs Lisp
// manual.  defalias is invoked as
//
//     (defalias 'emacs-rpc-bridge-native-start <function-object>)
//
// so the callback becomes a first-class function the user can call, advise,
// and wrap in interactive commands.  The docstring is attached through
// make_function at the call site below.

void defun(emacs_env *env, const std::string &suffix, emacs_value fn) {
  emacs_value Qdefalias = env->intern(env, "defalias");
  emacs_value symbol = env->intern(env, (kSymbolPrefix + suffix).c_str());
  emacs_value args[] = {symbol, fn};
  env->funcall(env, Qdefalias, 2, args);
}

emacs_value make_fun(emacs_env *env, int min, int max,
                     emacs_value (*fn)(emacs_env *, ptrdiff_t, emacs_value *,
                                       void *) noexcept,
                     const char *doc) {
  return env->make_function(env, min, max, fn, doc, nullptr);
}

}  // namespace

// ---- module entry point -----------------------------------------------------
//
// The only symbol Emacs looks up in the .so (besides
// plugin_is_GPL_compatible).  Emacs calls it once when module-load succeeds;
// returning nonzero aborts the load.

extern "C" int emacs_module_init(struct emacs_runtime *ert) {
  emacs_env *env = ert->get_environment(ert);

  // make_function(env, min-args, max-args, callback, docstring, user-data)
  // fixes the callback's arity; defun then binds it to a Lisp symbol.
  defun(env, "hello",
        make_fun(env, 0, 0, F_hello,
                 "Return a greeting string from the module."));
  defun(env, "shout",
        make_fun(env, 1, 1, F_shout,
                 "Return ARG converted to upper case."));

  defun(env, "native-start",
        make_fun(env, 1, 6, F_native_start,
                 "Start the gRPC bridge on Unix socket SOCKET-FILE.\n\n"
                 "Optional QUEUE-DEPTH (default 100) bounds the request "
                 "queue; MAX-PER-TICK (default 20) bounds requests drained "
                 "per timer tick; LEASE-SECONDS (default 300, 0 disables) "
                 "closes idle sessions; MAX-SESSIONS (default 32, 0 "
                 "unlimited) caps concurrent sessions; MAX-MESSAGE-BYTES "
                 "(default 0 = gRPC default 4 MiB) caps inbound messages.  "
                 "Returns t once started; the bridge finishes binding "
                 "asynchronously — poll `emacs-rpc-bridge-native-running-p' "
                 "and `emacs-rpc-bridge-native-last-error'.  Starts nothing "
                 "at module load; this function must be called explicitly."));
  defun(env, "native-drain",
        make_fun(env, 0, 0, F_native_drain,
                 "Execute queued bridge requests on this thread.\n\n"
                 "Called from the drain timer on Emacs's main thread; "
                 "executes at most the configured per-tick bound and "
                 "returns the number executed."));
  defun(env, "native-stop",
        make_fun(env, 0, 0, F_native_stop,
                 "Stop the bridge: stop accepting, cancel pending work, "
                 "join the server thread, and free remaining session "
                 "references.  Safe to call when not running."));
  defun(env, "native-running-p",
        make_fun(env, 0, 0, F_native_running_p,
                 "Return non-nil while the bridge server is bound."));
  defun(env, "native-last-error",
        make_fun(env, 0, 0, F_native_last_error,
                 "Return the readable reason for a failed start, or nil."));

  return 0;
}
