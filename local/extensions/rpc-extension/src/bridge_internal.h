// bridge_internal.h — ServerCore::Impl: bounded request queue + run state.
//
// Env-free: no Lisp objects here.  gRPC headers are pulled in only for the
// server pointer; the service implementation lives in server.cpp and the
// Emacs-thread execution in dispatch.cpp.

#pragma once

#include "bridge.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace grpc {
class Server;
}

namespace emacs_rpc_bridge {

class EmacsRpcServiceImpl;  // defined in server.cpp

struct ServerCore::Impl {
  // ---- bounded queue with wait-backpressure (decision 2026-09-08) ----
  // Enqueueing happens on gRPC worker threads and may wait for space (the
  // backpressure decision) while respecting the client deadline; that logic
  // needs the gRPC context, so it lives in server.cpp as `enqueue_*`.
  // Draining happens on the Emacs thread and never blocks.
  std::mutex qm;
  std::condition_variable qspace;  // signaled when a slot frees up
  std::deque<std::shared_ptr<RequestJob>> queue;
  size_t capacity = 100;           // guarded by qm
  bool stopping = false;           // guarded by qm

  // ---- registry of sessions (handles.cpp) ----
  std::shared_ptr<SessionRegistry> registry;

  // ---- serving state ----
  std::string socket_path;         // fixed at start, read by serving thread
  std::string emacs_version;       // fixed at start, read by workers
  size_t max_per_tick = 20;        // fixed at start (Emacs-thread reads)
  int64_t lease_seconds = 300;     // idle-session lease; 0 disables expiry
  size_t max_sessions = 32;        // concurrent-session quota; 0 unlimited
  size_t max_message_bytes = 0;    // inbound cap; 0 = gRPC default (4 MiB)
  std::atomic<bool> server_up{false};    // set by serving thread
  std::atomic<bool> start_failed{false}; // set by serving thread
  std::string start_error;         // guarded by qm, written by serving thread
  std::thread serving_thread;      // runs BuildAndStart + Wait
  std::unique_ptr<grpc::Server> server;          // joined before destroyed
  std::unique_ptr<EmacsRpcServiceImpl> service;  // destroyed after server

  // ---- Emacs-thread entry: pop one job without waiting ----
  std::shared_ptr<RequestJob> try_pop() {
    std::lock_guard<std::mutex> lk(qm);
    if (queue.empty()) return {};
    auto job = std::move(queue.front());
    queue.pop_front();
    qspace.notify_one();  // a handler waiting for space may now enqueue
    return job;
  }

  // Called (Emacs thread) before shutdown: no further enqueues, and any
  // worker blocked in enqueue() for space wakes up and gives up.
  void begin_stop() {
    std::lock_guard<std::mutex> lk(qm);
    stopping = true;
    qspace.notify_all();
  }

  void note_start_error(const std::string &err) {
    std::lock_guard<std::mutex> lk(qm);
    start_error = err;
    start_failed = true;
  }
};

// Wake the handlers waiting on the completion slots of every still-queued
// job (they gave up: the bridge is stopping).  Must run on the Emacs thread
// between begin_stop() and server->Shutdown(), otherwise Shutdown would wait
// forever for handlers that are waiting for a drain that will never come.
inline void cancel_queued(ServerCore::Impl *impl) {
  std::deque<std::shared_ptr<RequestJob>> leftover;
  {
    std::lock_guard<std::mutex> lk(impl->qm);
    leftover.swap(impl->queue);
  }
  for (auto &job : leftover) {
    std::lock_guard<std::mutex> lk(job->slot->m);
    if (!job->slot->done && !job->slot->cancelled) {
      job->slot->cancelled = true;
      job->slot->cv.notify_all();
    }
  }
}

}  // namespace emacs_rpc_bridge
