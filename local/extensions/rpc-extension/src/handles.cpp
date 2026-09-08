// handles.cpp — session-scoped handle table implementation
// (design-001.org TODO 3 "Implement dispatch and object lifetime").
//
// Handles map an opaque, session-scoped uint64 (as seen on the wire) to an
// Emacs global reference (emacs_value created with make_global_ref).
//
// Thread rules, per design:
//   - Global refs are made and freed ONLY in a valid Emacs context, i.e. on
//     the Emacs thread inside a callback.  Methods taking emacs_env are
//     therefore only called from the drain or from stop.
//   - Session creation (OpenSession on a worker thread) and existence checks
//     touch metadata only and are safe from any thread under the mutex.
//   - A global ref keeps an object alive against GC but does NOT prevent
//     kill-buffer and friends; callers must tolerate dead objects and turn
//     the resulting Lisp signal into a structured error.

#include "bridge.h"

#include <emacs-module.h>

#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "bridge_internal.h"

namespace emacs_rpc_bridge {

std::string random_token(size_t bytes) {
  static std::mt19937_64 rng{std::random_device{}()};
  static const char kHex[] = "0123456789abcdef";
  std::string s;
  s.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; ++i) {
    uint64_t v = rng();
    s.push_back(kHex[v & 0xF]);
    s.push_back(kHex[(v >> 4) & 0xF]);
  }
  return s;
}

struct SessionRegistry::Entry {
  std::string id;
  uint64_t next_handle = 1;
  std::map<uint64_t, emacs_value> handles;
  bool closed = false;
  std::chrono::steady_clock::time_point last_active;
};

std::shared_ptr<SessionRegistry::Entry> SessionRegistry::find_locked(
    const std::string &id) {
  auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : it->second;
}

bool SessionRegistry::create(const std::string &id) {
  std::lock_guard<std::mutex> lk(m_);
  if (by_id_.count(id)) return false;  // collision; caller retries with a
                                       // fresh token
  auto e = std::make_shared<Entry>();
  e->id = id;
  e->last_active = std::chrono::steady_clock::now();
  by_id_[id] = std::move(e);
  return true;
}

bool SessionRegistry::touch(const std::string &id) {
  std::lock_guard<std::mutex> lk(m_);
  auto it = by_id_.find(id);
  if (it == by_id_.end()) return false;
  it->second->last_active = std::chrono::steady_clock::now();
  return true;
}

bool SessionRegistry::exists(const std::string &id) {
  std::lock_guard<std::mutex> lk(m_);
  return by_id_.count(id) != 0;
}

size_t SessionRegistry::expire_older_than(
    emacs_env *env, std::chrono::steady_clock::duration max_idle) {
  // Collect ids under the lock, then close outside it (close_session
  // re-locks and touches only that entry; ids are unique forever, so a
  // session created between collect and close cannot be the same entry).
  auto cutoff = std::chrono::steady_clock::now() - max_idle;
  std::vector<std::string> expired;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (const auto &[sid, e] : by_id_)
      if (e->last_active < cutoff) expired.push_back(sid);
  }
  for (const std::string &sid : expired) close_session(env, sid);
  return expired.size();
}

size_t SessionRegistry::session_count() const {
  std::lock_guard<std::mutex> lk(m_);
  return by_id_.size();
}

uint64_t SessionRegistry::add_handle(emacs_env *env,
                                     const std::string &session_id,
                                     emacs_value value, std::string *error) {
  std::lock_guard<std::mutex> lk(m_);
  auto e = find_locked(session_id);
  if (!e) {
    if (error) *error = "unknown session";
    return 0;
  }
  emacs_value g = env->make_global_ref(env, value);
  uint64_t id = e->next_handle++;
  e->handles[id] = g;
  return id;
}

emacs_value SessionRegistry::lookup(const std::string &session_id,
                                    uint64_t handle_id, bool *found) {
  *found = false;
  std::lock_guard<std::mutex> lk(m_);
  auto e = find_locked(session_id);
  if (!e) return nullptr;
  auto h = e->handles.find(handle_id);
  if (h == e->handles.end()) return nullptr;
  *found = true;
  return h->second;
}

void SessionRegistry::free_handles(emacs_env *env,
                                   const std::string &session_id,
                                   const std::vector<uint64_t> &ids) {
  std::lock_guard<std::mutex> lk(m_);
  auto e = find_locked(session_id);
  if (!e) return;
  for (uint64_t id : ids) {  // releasing an unknown id is not an error
    auto h = e->handles.find(id);
    if (h != e->handles.end()) {
      env->free_global_ref(env, h->second);
      e->handles.erase(h);
    }
  }
}

void SessionRegistry::close_session(emacs_env *env,
                                    const std::string &session_id) {
  std::lock_guard<std::mutex> lk(m_);
  auto it = by_id_.find(session_id);
  if (it == by_id_.end()) return;
  for (auto &[id, g] : it->second->handles) {
    (void)id;
    env->free_global_ref(env, g);
  }
  it->second->handles.clear();
  by_id_.erase(it);
}

void SessionRegistry::close_all(emacs_env *env) {
  std::lock_guard<std::mutex> lk(m_);
  for (auto &[sid, e] : by_id_) {
    (void)sid;
    for (auto &[id, g] : e->handles) {
      (void)id;
      env->free_global_ref(env, g);
    }
    e->handles.clear();
  }
  by_id_.clear();
}

}  // namespace emacs_rpc_bridge
