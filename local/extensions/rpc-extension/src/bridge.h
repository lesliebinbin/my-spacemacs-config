// bridge.h — env-free shared types for the Emacs RPC bridge.
//
// Everything here is plain C++ with no Lisp objects and no emacs_env, so
// network threads may build, queue, and consume these freely.  Only
// dispatch.cpp ever converts between these types and emacs_value, always on
// the Emacs thread.  See runbook/emacs-rpc-bridge/design-001.org TODO 3.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Single source of truth for emacs_env / emacs_value.  This header declares
// only types and function tables (no code, no link dependency), so including
// it in the env-free translation units costs nothing and guarantees the
// bridge files and emacs-module.h always name the same types — hand-mirrored
// typedefs produced a namespace/global split that C++ refuses to convert.
#include <emacs-module.h>

namespace emacs_rpc_bridge {

constexpr int kProtocolVersion = 1;

// Plain representation of one Lisp value crossing the bridge.
struct WireValue {
  enum class Kind { kNil, kTrue, kInteger, kReal, kText, kUnibyte, kSymbol,
                    kHandle } kind = Kind::kNil;
  int64_t integer = 0;
  double real = 0.0;
  std::string bytes;        // kText / kUnibyte / kSymbol: payload
  uint64_t handle_id = 0;   // kHandle: session-scoped opaque id
};

// Structured Emacs signal/throw.  Distinct from transport failures, which
// are gRPC statuses on the wire.  symbol starts EMPTY so that an unfilled
// WireError (e.g. a Slot's fatal that never fired) reads as "not set";
// wire_error_to_proto substitutes "error" when emitting an empty symbol.
struct WireError {
  std::string symbol;                  // "" = unset (see server.cpp callers)
  std::string message;                 // readable fallback
  std::optional<WireValue> data;
};

// One executed op: exactly one of value / error is set.
struct CallResult {
  std::optional<WireValue> value;
  WireError error;
  bool is_error() const { return !value.has_value(); }
};

struct CallOpJob {
  std::string fn;
  std::vector<WireValue> args;
};

// Completion slot shared between a waiting gRPC handler thread and the drain
// on the Emacs thread.  Plain data only.
struct Slot {
  std::mutex m;
  std::condition_variable cv;
  bool cancelled = false;   // handler gave up (client deadline): skip if queued
  bool done = false;
  WireError fatal;                    // job failed before any op ran
  std::vector<CallResult> results;    // per-op results (kCall: 0 or 1 entries)
};

// One queued unit of work.
struct RequestJob {
  enum class Type { kCall, kWithBuffer, kReleaseHandles, kCloseSession };
  Type type = Type::kCall;
  std::string session;                 // owning session token
  uint64_t correlation_id = 0;
  CallOpJob call;                      // kCall
  std::vector<CallOpJob> calls;        // kWithBuffer
  uint64_t buffer_handle = 0;          // kWithBuffer
  std::vector<uint64_t> release_ids;   // kReleaseHandles
  std::shared_ptr<Slot> slot;
};

// Session-scoped handle table.  Rules (design-001.org TODO 3):
//   - Methods that take emacs_value / env touch global refs and must be
//     called ONLY on the Emacs thread (from the drain or from stop).
//   - create/touch/exists/session_count touch metadata only and may run on
//     any thread (OpenSession and the request handlers run on gRPC
//     workers).
//   - Sessions carry an inactivity timestamp refreshed by every request;
//     expire_older_than (Emacs thread, from the drain) enforces the lease.
class SessionRegistry {
 public:
  bool create(const std::string &id);   // any thread
  bool touch(const std::string &id);    // any thread; true if the session
                                        // exists, and refreshes its lease
  bool exists(const std::string &id);   // any thread
  size_t session_count() const;         // any thread

  // Closes every session idle for more than MAX_IDLE (lease enforcement,
  // design-001.org TODO 3 "inactivity leases" + TODO 5).  Returns how many
  // were closed.  Emacs-thread only: frees global refs via ENV.
  size_t expire_older_than(emacs_env *env,
                           std::chrono::steady_clock::duration max_idle);

  // --- Emacs-thread-only (valid env required) ---
  uint64_t add_handle(emacs_env *env, const std::string &session_id,
                      emacs_value value, std::string *error);
  emacs_value lookup(const std::string &session_id, uint64_t handle_id,
                     bool *found);
  void free_handles(emacs_env *env, const std::string &session_id,
                    const std::vector<uint64_t> &ids);
  void close_session(emacs_env *env, const std::string &session_id);
  void close_all(emacs_env *env);

 private:
  struct Entry;
  std::shared_ptr<Entry> find_locked(const std::string &id);
  mutable std::mutex m_;
  std::map<std::string, std::shared_ptr<Entry>> by_id_;
};

// The one object the module owns.  start() spawns the serving thread and
// returns quickly; drain_once() must be called ONLY on the Emacs thread
// (the Elisp timer does this); stop_and_join() is called on the Emacs thread
// during shutdown and frees remaining references before Emacs goes away.
class ServerCore {
 public:
  ServerCore();
  ~ServerCore();

  // Spawns the gRPC serving thread.  Returns immediately; actual readiness
  // is visible via running().  Knobs, all fixed at start:
  //   LEASE_SECONDS   idle sessions are closed after this long (0 = never)
  //   MAX_SESSIONS    concurrent-session quota (0 = unlimited)
  //   MAX_MESSAGE_BYTES  inbound message-size cap, enforced by gRPC
  //                      (0 = gRPC's own default)
  bool start(const std::string &socket_path, size_t queue_depth,
             size_t max_per_tick, const std::string &emacs_version,
             int64_t lease_seconds, size_t max_sessions,
             size_t max_message_bytes);
  // Stops accepting, completes/cancels pending work, joins the serving
  // thread, and frees all remaining session references (env required).
  void stop_and_join(emacs_env *env);

  // Executes up to the configured per-tick bound of queued requests.
  size_t drain_once(emacs_env *env);

  bool running() const;

  // Readable reason for a failed start (empty string when none).
  std::string last_error() const;

 public:
  struct Impl;  // defined in bridge_internal.h — public so the bridge's own
                // translation units can name it; nothing else sees the
                // definition.
 private:
  std::unique_ptr<Impl> impl_;
};

std::string random_token(size_t bytes = 16);

}  // namespace emacs_rpc_bridge
