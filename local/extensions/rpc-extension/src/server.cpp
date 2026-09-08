// server.cpp — network-facing endpoint (design-001.org TODO 3 "Implement
// dispatch and object lifetime").
//
//   external client -> Unix-domain socket -> bounded request queue
//
// Thread rules (design-001.org, do not regress):
//   - Every method here runs on a gRPC worker thread.  Workers NEVER call
//     the Emacs API, dereference Lisp handles, or retain emacs_env*.
//     OpenSession touches only session metadata; everything else is turned
//     into an env-free RequestJob and queued.
//   - Handlers block on the job's completion Slot; the Emacs-thread drain
//     (dispatch.cpp) publishes results.  Waiting respects the client's
//     deadline and cancellation: a handler that gives up marks the slot
//     cancelled so the drain skips execution.  Cancellation before execution
//     can prevent execution; after execution begins there is no rollback.
//   - Queue full = Emacs is behind (decision 2026-09-08): the enqueue waits
//     for space instead of failing fast.  The client's deadline and the
//     bridge stopping are the only things that interrupt that wait.
//   - stop_and_join wakes every blocked worker BEFORE server->Shutdown()
//     (Shutdown waits for handlers; handlers wait for a drain that stops
//     running at stop — waking them first avoids the deadlock).

#include "bridge_internal.h"

#include <emacs_rpc.pb.h>
#include <emacs_rpc.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace emacs_rpc_bridge {

using PbValue = emacs::rpc::v1::Value;
using PbError = emacs::rpc::v1::Error;
using PbCallOp = emacs::rpc::v1::CallOp;
using PbCallResponse = emacs::rpc::v1::CallResponse;

namespace {

// gRPC (1.83) reports call deadlines as system_clock time points; a call
// with no deadline set reports time_point::max().
using Deadline = std::chrono::time_point<std::chrono::system_clock>;
constexpr std::chrono::microseconds kWaitSlice(250000);

bool deadline_is_set(Deadline d) { return d != Deadline::max(); }

// Smallest of (250 ms, remaining time until deadline); 250 ms when the
// deadline is not set.  Slicing lets the waiters observe cancellation.
std::chrono::microseconds wait_slice(Deadline deadline) {
  if (!deadline_is_set(deadline)) return kWaitSlice;
  auto rem = std::chrono::duration_cast<std::chrono::microseconds>(
      deadline - std::chrono::system_clock::now());
  if (rem <= std::chrono::microseconds(0))
    return std::chrono::microseconds(1);  // expired
  return std::min(kWaitSlice, rem);
}

// ---- WireValue <-> proto Value conversions (env-free) ----------------------

WireValue proto_to_wire(const PbValue &v) {
  WireValue w;
  switch (v.kind_case()) {
    case PbValue::kTrueValue: w.kind = WireValue::Kind::kTrue; break;
    case PbValue::kInteger: w.kind = WireValue::Kind::kInteger;
                            w.integer = v.integer(); break;
    case PbValue::kReal: w.kind = WireValue::Kind::kReal;
                         w.real = v.real(); break;
    case PbValue::kText: w.kind = WireValue::Kind::kText;
                         w.bytes = v.text(); break;
    case PbValue::kUnibyte: w.kind = WireValue::Kind::kUnibyte;
                            w.bytes = v.unibyte(); break;
    case PbValue::kSymbolName: w.kind = WireValue::Kind::kSymbol;
                               w.bytes = v.symbol_name(); break;
    case PbValue::kHandle: w.kind = WireValue::Kind::kHandle;
                           w.handle_id = v.handle().id(); break;
    case PbValue::kNilValue:
    case PbValue::KIND_NOT_SET: w.kind = WireValue::Kind::kNil; break;
  }
  return w;
}

void wire_to_proto(const WireValue &w, PbValue *out) {
  switch (w.kind) {
    case WireValue::Kind::kNil: out->set_nil_value(true); break;
    case WireValue::Kind::kTrue: out->set_true_value(true); break;
    case WireValue::Kind::kInteger: out->set_integer(w.integer); break;
    case WireValue::Kind::kReal: out->set_real(w.real); break;
    case WireValue::Kind::kText: out->set_text(w.bytes); break;
    case WireValue::Kind::kUnibyte: out->set_unibyte(w.bytes); break;
    case WireValue::Kind::kSymbol: out->set_symbol_name(w.bytes); break;
    case WireValue::Kind::kHandle:
      out->mutable_handle()->set_id(w.handle_id);
      // Handle kind is advisory only; the bridge does not track kinds (yet).
      break;
  }
}

void wire_error_to_proto(const WireError &e, PbError *out) {
  out->set_symbol(e.symbol.empty() ? "error" : e.symbol);
  if (!e.message.empty()) out->set_message(e.message);
  if (e.data.has_value()) wire_to_proto(*e.data, out->mutable_data());
}

// ---- queue enqueue with wait-backpressure + client deadline ---------------

enum class EnqueueOutcome { kQueued, kStopping, kTimedOut, kCancelled };

// Worker-thread entry.  Waits for a free slot while the queue is full
// (backpressure decision) but gives up when the bridge stops or the client's
// deadline / cancellation arrives.
EnqueueOutcome enqueue(ServerCore::Impl *impl,
                       std::shared_ptr<RequestJob> job,
                       grpc::ServerContext *ctx) {
  std::unique_lock<std::mutex> lk(impl->qm);
  Deadline deadline = ctx->deadline();
  for (;;) {
    if (impl->stopping) return EnqueueOutcome::kStopping;
    if (ctx->IsCancelled()) return EnqueueOutcome::kCancelled;
    if (!deadline_is_set(deadline) ||
        deadline > std::chrono::system_clock::now()) {
      if (impl->queue.size() < impl->capacity) {
        impl->queue.push_back(std::move(job));
        return EnqueueOutcome::kQueued;
      }
    } else {
      return EnqueueOutcome::kTimedOut;
    }
    impl->qspace.wait_for(lk, wait_slice(deadline));
  }
}

// Wait for the drain to publish the job's result, respecting the deadline
// and client cancellation.  Returns true only when the job ran to
// completion (slot->done); a false return means this handler gave up and
// marked the slot cancelled so the drain will skip execution.
bool wait_for_completion(grpc::ServerContext *ctx,
                         const std::shared_ptr<Slot> &slot) {
  std::unique_lock<std::mutex> lk(slot->m);
  Deadline deadline = ctx->deadline();
  while (!slot->done) {
    if (slot->cancelled) return false;  // already given up
    if (ctx->IsCancelled()) {
      slot->cancelled = true;
      return false;
    }
    if (deadline_is_set(deadline) &&
        deadline <= std::chrono::system_clock::now()) {
      slot->cancelled = true;  // drain must not run the job now
      return false;
    }
    slot->cv.wait_for(lk, wait_slice(deadline));
  }
  return true;
}

grpc::Status give_up_status(grpc::ServerContext *ctx, const char *queued) {
  if (ctx->IsCancelled())
    return grpc::Status(grpc::StatusCode::CANCELLED,
                        "call cancelled by client");
  return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, queued);
}

grpc::Status unknown_session() {
  return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                      "unknown session");
}

}  // namespace

// The emacs.rpc.v1 service.  Owned by ServerCore::Impl and destroyed only
// after the serving thread joined, so the raw Impl* stays valid for the
// lifetime of every handler.
class EmacsRpcServiceImpl final : public emacs::rpc::v1::EmacsRpc::Service {
 public:
  explicit EmacsRpcServiceImpl(ServerCore::Impl *impl) : impl_(impl) {}

  grpc::Status OpenSession(grpc::ServerContext *ctx,
                           const emacs::rpc::v1::OpenSessionRequest *req,
                           emacs::rpc::v1::OpenSessionResponse *resp) override {
    (void)ctx;
    if (req->protocol_version() < 1)
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "unsupported protocol version");
    // Session quota (design-001.org TODO 5): concurrent sessions are
    // capped so disconnected clients cannot pile up without bound.  Two
    // workers may race past the check by one session — a soft quota.
    if (impl_->max_sessions > 0 &&
        impl_->registry->session_count() >= impl_->max_sessions)
      return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                          "too many open sessions");
    int32_t version =
        std::min<int32_t>(req->protocol_version(), kProtocolVersion);
    // Token collision is astronomically unlikely; retry rather than fail.
    std::string session_id;
    do {
      session_id = random_token(16);
    } while (!impl_->registry->create(session_id));
    resp->set_session_id(session_id);
    resp->set_protocol_version(version);
    resp->set_emacs_version(impl_->emacs_version);
    return grpc::Status::OK;
  }

  grpc::Status Call(grpc::ServerContext *ctx,
                    const emacs::rpc::v1::CallRequest *req,
                    emacs::rpc::v1::CallResponse *resp) override {
    // touch() doubles as the exists() check and refreshes the lease.
    if (!impl_->registry->touch(req->session_id())) return unknown_session();

    auto job = std::make_shared<RequestJob>();
    job->type = RequestJob::Type::kCall;
    job->session = req->session_id();
    job->correlation_id = req->correlation_id();
    job->slot = std::make_shared<Slot>();
    job->call.fn = req->op().function_name();
    for (const PbValue &a : req->op().arguments())
      job->call.args.push_back(proto_to_wire(a));

    if (EnqueueOutcome outcome = enqueue(impl_, job, ctx);
        outcome != EnqueueOutcome::kQueued)
      return enqueue_status(outcome);
    if (!wait_for_completion(ctx, job->slot))
      return give_up_status(ctx, "timed out waiting for Emacs dispatch");

    resp->set_correlation_id(req->correlation_id());
    // A fatal (e.g. session closed between enqueue and drain) surfaces as a
    // structured op error rather than a transport failure.
    if (job->slot->fatal.symbol.empty() && job->slot->results.size() == 1) {
      const CallResult &r = job->slot->results[0];
      if (r.is_error()) {
        wire_error_to_proto(r.error, resp->mutable_error());
      } else if (r.value.has_value()) {
        wire_to_proto(*r.value, resp->mutable_value());
      }
    } else {
      wire_error_to_proto(job->slot->fatal, resp->mutable_error());
    }
    return grpc::Status::OK;
  }

  grpc::Status WithBuffer(grpc::ServerContext *ctx,
                          const emacs::rpc::v1::WithBufferRequest *req,
                          emacs::rpc::v1::WithBufferResponse *resp) override {
    if (!impl_->registry->touch(req->session_id())) return unknown_session();
    bool found = false;
    impl_->registry->lookup(req->session_id(), req->buffer().id(), &found);
    if (!found)
      return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                          "unknown buffer handle");
    if (req->calls_size() > 1000)
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "too many calls in WithBuffer");

    auto job = std::make_shared<RequestJob>();
    job->type = RequestJob::Type::kWithBuffer;
    job->session = req->session_id();
    job->correlation_id = req->correlation_id();
    job->buffer_handle = req->buffer().id();
    job->slot = std::make_shared<Slot>();
    for (const PbCallOp &op : req->calls()) {
      CallOpJob cop;
      cop.fn = op.function_name();
      for (const PbValue &a : op.arguments())
        cop.args.push_back(proto_to_wire(a));
      job->calls.push_back(std::move(cop));
    }

    if (EnqueueOutcome outcome = enqueue(impl_, job, ctx);
        outcome != EnqueueOutcome::kQueued)
      return enqueue_status(outcome);
    if (!wait_for_completion(ctx, job->slot))
      return give_up_status(ctx, "timed out waiting for Emacs dispatch");

    resp->set_correlation_id(req->correlation_id());
    for (const CallResult &r : job->slot->results) {
      PbCallResponse *cr = resp->add_results();
      cr->set_correlation_id(req->correlation_id());
      if (r.is_error()) {
        wire_error_to_proto(r.error, cr->mutable_error());
      } else if (r.value.has_value()) {
        wire_to_proto(*r.value, cr->mutable_value());
      }
    }
    if (!job->slot->fatal.symbol.empty())
      wire_error_to_proto(job->slot->fatal, resp->mutable_error());
    return grpc::Status::OK;
  }

  grpc::Status ReleaseHandles(grpc::ServerContext *ctx,
                              const emacs::rpc::v1::ReleaseHandlesRequest *req,
                              emacs::rpc::v1::ReleaseHandlesResponse *resp) override {
    (void)resp;
    if (!impl_->registry->touch(req->session_id())) return unknown_session();

    auto job = std::make_shared<RequestJob>();
    job->type = RequestJob::Type::kReleaseHandles;
    job->session = req->session_id();
    job->slot = std::make_shared<Slot>();
    for (const auto &h : req->handles()) job->release_ids.push_back(h.id());

    if (EnqueueOutcome outcome = enqueue(impl_, job, ctx);
        outcome != EnqueueOutcome::kQueued)
      return enqueue_status(outcome);
    if (!wait_for_completion(ctx, job->slot))
      return give_up_status(ctx, "timed out waiting for Emacs dispatch");
    return grpc::Status::OK;
  }

  grpc::Status CloseSession(grpc::ServerContext *ctx,
                            const emacs::rpc::v1::CloseSessionRequest *req,
                            emacs::rpc::v1::CloseSessionResponse *resp) override {
    (void)resp;
    if (!impl_->registry->touch(req->session_id())) return unknown_session();

    auto job = std::make_shared<RequestJob>();
    job->type = RequestJob::Type::kCloseSession;
    job->session = req->session_id();
    job->slot = std::make_shared<Slot>();

    if (EnqueueOutcome outcome = enqueue(impl_, job, ctx);
        outcome != EnqueueOutcome::kQueued)
      return enqueue_status(outcome);
    if (!wait_for_completion(ctx, job->slot))
      return give_up_status(ctx, "timed out waiting for Emacs dispatch");
    return grpc::Status::OK;
  }

 private:
  static grpc::Status enqueue_status(EnqueueOutcome outcome) {
    switch (outcome) {
      case EnqueueOutcome::kStopping:
        return grpc::Status(grpc::StatusCode::CANCELLED,
                            "bridge is shutting down");
      case EnqueueOutcome::kTimedOut:
        return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                            "queue full; timed out waiting for space");
      default:
        return grpc::Status(grpc::StatusCode::CANCELLED,
                            "call cancelled by client");
    }
  }

  ServerCore::Impl *impl_;
};

// ---- ServerCore ------------------------------------------------------------

ServerCore::ServerCore() : impl_(std::make_unique<Impl>()) {
  impl_->registry = std::make_shared<SessionRegistry>();
}

namespace {

// Ordered shutdown of the serving side, without touching Lisp:
//   1. begin_stop(): no new enqueues; wake workers waiting for queue space.
//   2. cancel_queued(): wake handlers waiting on the completion slots of
//      queued jobs — otherwise server->Shutdown() would block forever on
//      handlers waiting for a drain that no longer runs.
//   3. server->Shutdown() (from the Emacs thread — never from a gRPC one).
//   4. join the serving thread.
// Must run on the Emacs thread (same thread as drain/stop).
void shutdown_serving(ServerCore::Impl *impl) {
  if (!impl->serving_thread.joinable()) return;
  impl->begin_stop();
  cancel_queued(impl);
  if (impl->server) impl->server->Shutdown();
  impl->serving_thread.join();
  impl->server.reset();
  impl->service.reset();  // no handler can be running once joined
  impl->server_up = false;
  impl->start_failed = false;
}

}  // namespace

ServerCore::~ServerCore() {
  // Only reachable if the module is torn down (Emacs never unloads modules,
  // so this is normally the process exit path).  No env here: references in
  // the registry are not freed, which is moot at process exit.
  shutdown_serving(impl_.get());
}

bool ServerCore::start(const std::string &socket_path, size_t queue_depth,
                       size_t max_per_tick, const std::string &emacs_version,
                       int64_t lease_seconds, size_t max_sessions,
                       size_t max_message_bytes) {
  // A serving thread left over from a failed earlier start has exited.
  if (impl_->serving_thread.joinable()) impl_->serving_thread.join();
  if (running()) return false;

  {
    std::lock_guard<std::mutex> lk(impl_->qm);
    impl_->queue.clear();
    impl_->stopping = false;
    impl_->capacity = queue_depth;
    impl_->max_per_tick = max_per_tick;
    impl_->lease_seconds = lease_seconds;
    impl_->max_sessions = max_sessions;
    impl_->max_message_bytes = max_message_bytes;
    impl_->emacs_version = emacs_version;
    impl_->socket_path = socket_path;
    impl_->start_error.clear();
  }
  impl_->server_up = false;
  impl_->start_failed = false;
  impl_->service = std::make_unique<EmacsRpcServiceImpl>(impl_.get());

  // A stale socket from a crashed run would make the bind fail.
  ::unlink(socket_path.c_str());

  impl_->serving_thread = std::thread([impl = impl_.get()] {
    try {
      grpc::ServerBuilder builder;
      builder.AddListeningPort("unix:" + impl->socket_path,
                               grpc::InsecureServerCredentials());
      // Message-size limit (TODO 5): oversized requests are rejected by
      // gRPC itself with RESOURCE_EXHAUSTED, before they touch the queue
      // or the Emacs thread.  All fields above were set before this
      // thread started (happens-before via std::thread creation).
      if (impl->max_message_bytes > 0)
        builder.SetMaxReceiveMessageSize(
            static_cast<int>(impl->max_message_bytes));
      builder.RegisterService(impl->service.get());
      auto srv = builder.BuildAndStart();
      if (!srv) {
        impl->note_start_error("gRPC BuildAndStart failed (socket in use?)");
        return;
      }
      impl->server = std::move(srv);
      impl->server_up = true;
      impl->server->Wait();  // returns once Shutdown() completes
    } catch (const std::exception &e) {
      impl->note_start_error(std::string("server thread: ") + e.what());
    }
  });
  return true;
}

void ServerCore::stop_and_join(emacs_env *env) {
  shutdown_serving(impl_.get());
  // Free every remaining global reference while Emacs is still alive.
  impl_->registry->close_all(env);
}

bool ServerCore::running() const { return impl_->server_up.load(); }

std::string ServerCore::last_error() const {
  std::lock_guard<std::mutex> lk(impl_->qm);
  return impl_->start_error;
}

}  // namespace emacs_rpc_bridge
