// dispatch.cpp — Emacs-thread execution of queued requests (design-001.org
// TODO 3 "Implement dispatch and object lifetime").
//
// drain_once() runs on Emacs's main thread with a fresh valid environment
// (the Elisp timer calls the registered native drain function).  It pops up
// to the per-tick bound of jobs and executes them:
//
//   - kCall: intern the function symbol, convert WireValue arguments to
//     emacs_value, funcall, classify the result, capture non-local exits as
//     structured WireErrors.  A non-local exit must be extracted and cleared
//     before any further normal Emacs API call.
//   - kWithBuffer: funcall the Lisp helper emacs-rpc-bridge--exec-with-buffer
//     (defined in lisp/emacs-rpc-bridge.el) with the buffer object and a
//     vector of [fn args] pairs; the helper runs the sequence under
//     save-current-buffer, stopping at the first error, and returns a vector
//     of per-op entries that this file decodes.
//   - kReleaseHandles / kCloseSession: release global refs.
//
// Conversion rules: nil / t / fixnum / float / UTF-8 string / symbol are
// scalars on the wire; everything else (buffers, vectors, conses, markers,
// ...) becomes a session handle backed by a global reference.  Arguments
// name such objects by handle.  Error data is converted scalars-only so a
// signal can never leak an unreferenced handle.
//
// C++ exceptions are caught here: this code runs under a noexcept module
// callback, and one escaping would terminate Emacs.

#include "bridge_internal.h"

#include <emacs-module.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace emacs_rpc_bridge {

namespace {

// Lisp helper executing a WithBuffer sequence; see the contract in the file
// comment and in lisp/emacs-rpc-bridge.el.
constexpr const char *kExecWithBuffer = "emacs-rpc-bridge--exec-with-buffer";

constexpr size_t kMaxWithBufferOps = 1000;  // same bound as server.cpp
constexpr size_t kMaxUnibyteArg = 2048;     // per-call (unibyte-string) limit

// ---- exit-state helpers -----------------------------------------------------
//
// The module API reports non-local exits through non_local_exit_check/get;
// an exit must be cleared before more Emacs API calls.  capture_error()
// below both extracts and clears.

bool exit_pending(emacs_env *env) {
  return env->non_local_exit_check(env) != emacs_funcall_exit_return;
}

// funcall, returning the raw result; on non-local exit *out is untouched and
// the exit is left pending for the caller to capture with capture_error().
void raw_call(emacs_env *env, emacs_value fn, ptrdiff_t nargs,
              emacs_value *args, emacs_value *out) {
  emacs_value r = env->funcall(env, fn, nargs, args);
  if (exit_pending(env)) return;
  *out = r;
}

// Clear any pending exit (call when the exit is known harmless, e.g. after a
// best-effort convenience funcall).
void clear_exit(emacs_env *env) {
  if (exit_pending(env)) env->non_local_exit_clear(env);
}

bool is_lisp_type(emacs_env *env, emacs_value v, const char *name) {
  return env->eq(env, env->type_of(env, v), env->intern(env, name));
}

// Best-effort UTF-8 name of a symbol; empty on failure.
std::string symbol_name(emacs_env *env, emacs_value sym) {
  emacs_value fn = env->intern(env, "symbol-name");
  emacs_value out = nullptr;
  raw_call(env, fn, 1, &sym, &out);
  if (exit_pending(env)) {
    clear_exit(env);
    return {};
  }
  ptrdiff_t size = 0;
  if (!env->copy_string_contents(env, out, nullptr, &size)) {
    clear_exit(env);
    return {};
  }
  std::string s(static_cast<std::size_t>(size) - 1, '\0');
  env->copy_string_contents(env, out, s.data(), &size);
  clear_exit(env);
  return s;
}

// ---- wire <-> emacs_value conversions --------------------------------------

// Type of one classified scalar result.
enum class Scalar {
  kNotScalar,
  kNil,
  kTrue,
  kInteger,
  kReal,
  kText,       // multibyte, UTF-8
  kUnibyte,    // raw bytes
  kSymbol,
};

// Classify v as a wire scalar (no global refs, no side effects).  Symbols
// nil and t are wire scalars; other symbols are kSymbol.
Scalar classify(emacs_env *env, emacs_value v) {
  if (!is_lisp_type(env, v, "symbol")) {
    if (is_lisp_type(env, v, "integer")) return Scalar::kInteger;
    if (is_lisp_type(env, v, "float")) return Scalar::kReal;
    if (is_lisp_type(env, v, "string")) {
      emacs_value fn = env->intern(env, "multibyte-string-p");
      emacs_value out = nullptr;
      raw_call(env, fn, 1, &v, &out);
      if (exit_pending(env)) {
        clear_exit(env);
        return Scalar::kText;  // conservative: treat as text
      }
      return env->is_not_nil(env, out) ? Scalar::kText : Scalar::kUnibyte;
    }
    return Scalar::kNotScalar;
  }
  if (env->eq(env, v, env->intern(env, "nil"))) return Scalar::kNil;
  if (env->eq(env, v, env->intern(env, "t"))) return Scalar::kTrue;
  return Scalar::kSymbol;
}

std::string emacs_string(emacs_env *env, emacs_value v) {
  ptrdiff_t size = 0;
  if (!env->copy_string_contents(env, v, nullptr, &size)) {
    clear_exit(env);
    return {};
  }
  std::string s(static_cast<std::size_t>(size) - 1, '\0');
  env->copy_string_contents(env, v, s.data(), &size);
  clear_exit(env);
  return s;
}

// Raw bytes of a unibyte string: chars 0..255 are its bytes, so
// (vconcat (string-to-list S)) yields a vector of ints with no string
// encoding round-trip in between.
std::string unibyte_bytes(emacs_env *env, emacs_value v) {
  emacs_value fn, out = nullptr;
  fn = env->intern(env, "string-to-list");
  raw_call(env, fn, 1, &v, &out);
  if (exit_pending(env)) {
    clear_exit(env);
    return {};
  }
  fn = env->intern(env, "vconcat");
  raw_call(env, fn, 1, &out, &out);
  if (exit_pending(env)) {
    clear_exit(env);
    return {};
  }
  ptrdiff_t n = env->vec_size(env, out);
  if (exit_pending(env)) {
    clear_exit(env);
    return {};
  }
  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(n));
  for (ptrdiff_t i = 0; i < n; ++i) {
    emacs_value e = env->vec_get(env, out, i);
    if (exit_pending(env)) {
      clear_exit(env);
      return {};
    }
    intmax_t c = env->extract_integer(env, e);
    if (exit_pending(env) || c < 0 || c > 255) {
      clear_exit(env);
      return {};
    }
    bytes.push_back(static_cast<char>(c));
  }
  return bytes;
}

// Convert an op result to a WireValue.  Scalars convert directly; every
// other live object becomes a session handle (global ref added now, freed on
// ReleaseHandles / CloseSession).  Only valid on the Emacs thread.
bool value_to_wire(emacs_env *env, SessionRegistry *registry,
                   const std::string &session, emacs_value v,
                   WireValue *out, WireError *err) {
  Scalar k = classify(env, v);
  switch (k) {
    case Scalar::kNil: out->kind = WireValue::Kind::kNil; return true;
    case Scalar::kTrue: out->kind = WireValue::Kind::kTrue; return true;
    case Scalar::kInteger:  // type_of guaranteed a fixnum; see classify()
      out->kind = WireValue::Kind::kInteger;
      out->integer = static_cast<int64_t>(env->extract_integer(env, v));
      return true;
    case Scalar::kReal:
      out->kind = WireValue::Kind::kReal;
      out->real = env->extract_float(env, v);  // returns double directly
      return true;
    case Scalar::kText:
      out->kind = WireValue::Kind::kText;
      out->bytes = emacs_string(env, v);
      return true;
    case Scalar::kUnibyte:
      out->kind = WireValue::Kind::kUnibyte;
      out->bytes = unibyte_bytes(env, v);
      return true;
    case Scalar::kSymbol: {
      std::string name = symbol_name(env, v);
      if (name.empty()) {
        err->symbol = "internal-error";
        err->message = "could not read symbol name";
        return false;
      }
      out->kind = WireValue::Kind::kSymbol;
      out->bytes = std::move(name);
      return true;
    }
    case Scalar::kNotScalar: {
      std::string herr;
      uint64_t id = registry->add_handle(env, session, v, &herr);
      if (id == 0) {
        err->symbol = "internal-error";
        err->message = herr.empty() ? "could not create handle" : herr;
        return false;
      }
      out->kind = WireValue::Kind::kHandle;
      out->handle_id = id;
      return true;
    }
  }
  err->symbol = "internal-error";
  err->message = "unconvertible result";
  return false;
}

// Extract and clear a pending non-local exit into a structured WireError.
// Defined below; used by wire_to_value's unibyte path.
void capture_error(emacs_env *env, WireError *err);

// Convert a WireValue argument to an emacs_value.  Handle arguments are
// resolved against the session; a released/unknown handle is a per-op error.
bool wire_to_value(emacs_env *env, SessionRegistry *registry,
                   const std::string &session, const WireValue &w,
                   emacs_value *out, WireError *err) {
  switch (w.kind) {
    case WireValue::Kind::kNil:
      *out = env->intern(env, "nil");
      return true;
    case WireValue::Kind::kTrue:
      *out = env->intern(env, "t");
      return true;
    case WireValue::Kind::kInteger:
      *out = env->make_integer(env, static_cast<intmax_t>(w.integer));
      return true;
    case WireValue::Kind::kReal:
      *out = env->make_float(env, w.real);
      return true;
    case WireValue::Kind::kText:
      *out = env->make_string(env, w.bytes.data(),
                              static_cast<ptrdiff_t>(w.bytes.size()));
      return true;
    case WireValue::Kind::kSymbol:
      *out = env->intern(env, w.bytes.c_str());
      return true;
    case WireValue::Kind::kUnibyte: {
      if (w.bytes.size() > kMaxUnibyteArg) {
        err->symbol = "args-out-of-range";
        err->message = "unibyte argument too long";
        return false;
      }
      // (unibyte-string B0 B1 ...) builds the exact byte string; each byte
      // is passed as an integer so no encoding round-trip is involved.
      emacs_value fn = env->intern(env, "unibyte-string");
      std::vector<emacs_value> argv;
      argv.reserve(w.bytes.size());
      for (unsigned char c : w.bytes)
        argv.push_back(env->make_integer(env, c));
      raw_call(env, fn, static_cast<ptrdiff_t>(argv.size()), argv.data(),
               out);
      if (exit_pending(env)) {
        capture_error(env, err);
        return false;
      }
      return true;
    }
    case WireValue::Kind::kHandle: {
      bool found = false;
      emacs_value v = registry->lookup(session, w.handle_id, &found);
      if (!found) {
        err->symbol = "wrong-type-argument";
        err->message = "unknown or released handle " +
                       std::to_string(w.handle_id);
        return false;
      }
      *out = v;
      return true;
    }
  }
  err->symbol = "internal-error";
  err->message = "unconvertible argument";
  return false;
}

// ---- error capture ----------------------------------------------------------

// Extract and clear a pending non-local exit into a structured WireError.
// Error data is converted scalars-only so capturing an error never leaks a
// handle.  A readable message is asked from error-message-string on
// (cons SYMBOL DATA); that is a convenience funcall whose own failure is
// discarded in favor of the bare symbol name.
void capture_error(emacs_env *env, WireError *err) {
  emacs_value sym = nullptr, data = nullptr;
  env->non_local_exit_get(env, &sym, &data);
  env->non_local_exit_clear(env);

  std::string name = symbol_name(env, sym);
  err->symbol = name.empty() ? "error" : std::move(name);
  err->message = err->symbol;

  // Build the readable message only when there is data (e.g. (error "msg"))
  if (data != nullptr && !env->eq(env, data, env->intern(env, "nil"))) {
    emacs_value cons = env->intern(env, "cons");
    emacs_value consed = nullptr;
    {
      emacs_value args[2] = {sym, data};
      raw_call(env, cons, 2, args, &consed);
    }
    if (!exit_pending(env)) {
      emacs_value fns = env->intern(env, "error-message-string");
      emacs_value msgv = nullptr;
      raw_call(env, fns, 1, &consed, &msgv);
      if (!exit_pending(env) && is_lisp_type(env, msgv, "string")) {
        std::string msg = emacs_string(env, msgv);
        if (!msg.empty()) err->message = std::move(msg);
      } else {
        clear_exit(env);
      }
    } else {
      clear_exit(env);
    }
  }
  // Representable scalar data (usually absent — most error data is a list).
  if (data != nullptr && !env->eq(env, data, env->intern(env, "nil"))) {
    WireValue w;
    WireError unused;
    Scalar k = classify(env, data);
    if (k != Scalar::kNotScalar &&
        value_to_wire(env, nullptr, "", data, &w, &unused))
      err->data = std::move(w);
  }
}

// ---- execution --------------------------------------------------------------

// Result of converting one CallOpJob into callable emacs_values.
struct ReadyOp {
  std::vector<emacs_value> args;  // empty => conversion failed, see err
  WireError err;
  bool ok() const { return err.symbol.empty(); }
};

// Intern the function and convert the arguments for one op.
bool prepare_op(emacs_env *env, SessionRegistry *registry,
                const std::string &session, const CallOpJob &op,
                emacs_value *fn, std::vector<emacs_value> *args,
                WireError *err) {
  *fn = env->intern(env, op.fn.c_str());
  if (exit_pending(env)) {
    capture_error(env, err);
    return false;
  }
  args->reserve(op.args.size());
  for (const WireValue &a : op.args) {
    emacs_value v = nullptr;
    WireError aerr;
    if (!wire_to_value(env, registry, session, a, &v, &aerr)) {
      *err = std::move(aerr);
      return false;
    }
    if (exit_pending(env)) {  // make_* can fail in principle
      capture_error(env, err);
      return false;
    }
    args->push_back(v);
  }
  return true;
}

// funcall one op and classify: value, or captured WireError.
void run_call(emacs_env *env, SessionRegistry *registry,
              const std::string &session, const CallOpJob &op,
              CallResult *out) {
  emacs_value fn = nullptr;
  std::vector<emacs_value> args;
  WireError err;
  if (!prepare_op(env, registry, session, op, &fn, &args, &err)) {
    out->error = std::move(err);
    return;
  }
  emacs_value ret = nullptr;
  raw_call(env, fn, static_cast<ptrdiff_t>(args.size()), args.data(), &ret);
  if (exit_pending(env)) {
    WireError cap;
    capture_error(env, &cap);
    out->error = std::move(cap);
    return;
  }
  WireValue w;
  WireError werr;
  if (!value_to_wire(env, registry, session, ret, &w, &werr)) {
    out->error = std::move(werr);
    return;
  }
  out->value = std::move(w);
}

// emacs_value helpers that build small values, exit-guarded.
emacs_value make_vector_of(emacs_env *env, ptrdiff_t n, emacs_value *elems) {
  emacs_value fn = env->intern(env, "vector");
  emacs_value out = nullptr;
  raw_call(env, fn, n, elems, &out);
  return out;
}

// Execute one WithBuffer job via the Lisp helper.  On success *out receives
// the helper's result vector; on helper-level failure *fatal is set.
bool run_with_buffer(emacs_env *env, SessionRegistry *registry,
                     const std::string &session, uint64_t buffer_handle,
                     const std::vector<CallOpJob> &ops,
                     emacs_value *helper_result, WireError *fatal) {
  if (!registry->exists(session)) {
    fatal->symbol = "unknown-session";
    fatal->message = "session closed before dispatch";
    return false;
  }
  bool found = false;
  emacs_value buffer = registry->lookup(session, buffer_handle, &found);
  if (!found) {
    fatal->symbol = "wrong-type-argument";
    fatal->message = "buffer handle no longer valid";
    return false;
  }

  // calls := (vector [fn args] [fn args] ...) — one vector per op.
  std::vector<emacs_value> op_vectors;
  op_vectors.reserve(ops.size());
  for (const CallOpJob &op : ops) {
    WireError err;
    emacs_value fn = nullptr;
    std::vector<emacs_value> args;
    if (!prepare_op(env, registry, session, op, &fn, &args, &err)) {
      *fatal = std::move(err);
      return false;
    }
    emacs_value args_vec = make_vector_of(
        env, static_cast<ptrdiff_t>(args.size()), args.data());
    if (exit_pending(env)) {
      capture_error(env, fatal);
      return false;
    }
    emacs_value inner[2] = {fn, args_vec};
    emacs_value pair = make_vector_of(env, 2, inner);
    if (exit_pending(env)) {
      capture_error(env, fatal);
      return false;
    }
    op_vectors.push_back(pair);
  }
  emacs_value calls = make_vector_of(
      env, static_cast<ptrdiff_t>(op_vectors.size()), op_vectors.data());
  if (exit_pending(env)) {
    capture_error(env, fatal);
    return false;
  }

  emacs_value helper = env->intern(env, kExecWithBuffer);
  emacs_value args[2] = {buffer, calls};
  raw_call(env, helper, 2, args, helper_result);
  if (exit_pending(env)) {
    capture_error(env, fatal);
    return false;
  }
  return true;
}

// Decode the helper's result vector into per-op results.  Contract (see
// lisp/emacs-rpc-bridge.el):
//   entry = [ok VALUE] | [error "sym" "msg"] | [fatal "sym" "msg"]
// A fatal entry stops the sequence and fills *fatal (partial results are
// kept); an error entry stops and is reported as the last result.
void decode_helper_result(emacs_env *env, SessionRegistry *registry,
                          const std::string &session,
                          emacs_value helper_result,
                          std::vector<CallResult> *results,
                          WireError *fatal) {
  if (exit_pending(env)) {  // defensive: caller cleared already
    capture_error(env, fatal);
    return;
  }
  ptrdiff_t n = env->vec_size(env, helper_result);
  if (exit_pending(env)) {
    capture_error(env, fatal);
    return;
  }
  auto qok = env->intern(env, "ok");
  auto qfatal = env->intern(env, "fatal");
  for (ptrdiff_t i = 0; i < n; ++i) {
    emacs_value entry = env->vec_get(env, helper_result, i);
    if (exit_pending(env)) {
      capture_error(env, fatal);
      return;
    }
    if (!is_lisp_type(env, entry, "vector")) {
      fatal->symbol = "internal-error";
      fatal->message = "malformed helper result entry";
      return;
    }
    ptrdiff_t es = env->vec_size(env, entry);
    if (exit_pending(env)) {
      capture_error(env, fatal);
      return;
    }
    if (es < 1) {
      fatal->symbol = "internal-error";
      fatal->message = "malformed helper result entry";
      return;
    }
    emacs_value status = env->vec_get(env, entry, 0);
    if (exit_pending(env)) {
      capture_error(env, fatal);
      return;
    }
    if (env->eq(env, status, qfatal) && es >= 3) {
      WireError e;
      e.symbol = emacs_string(env, env->vec_get(env, entry, 1));
      e.message = emacs_string(env, env->vec_get(env, entry, 2));
      *fatal = std::move(e);
      return;
    }
    if (env->eq(env, status, qok) && es >= 2) {
      emacs_value v = env->vec_get(env, entry, 1);
      if (exit_pending(env)) {
        capture_error(env, fatal);
        return;
      }
      CallResult r;
      WireValue w;
      WireError werr;
      if (value_to_wire(env, registry, session, v, &w, &werr)) {
        r.value = std::move(w);
      } else {
        r.error = std::move(werr);
      }
      results->push_back(std::move(r));
      continue;
    }
    // [error "sym" "msg"] — or a malformed entry treated as an error.
    WireError e;
    e.symbol = es >= 2 ? emacs_string(env, env->vec_get(env, entry, 1))
                       : std::string("error");
    e.message = es >= 3 ? emacs_string(env, env->vec_get(env, entry, 2))
                        : std::string("Lisp error");
    CallResult r;
    r.error = std::move(e);
    results->push_back(std::move(r));
    return;  // stop at the first error: later ops did not run
  }
}

// Execute one job and publish the outcome on its slot.
void dispatch_job(emacs_env *env, ServerCore::Impl *impl, RequestJob *job) {
  WireError fatal;
  std::vector<CallResult> results;

  // Lease/close enforcement: a session can die between enqueue (its
  // handler touched it) and this drain pass (the sweep above, or an
  // explicit CloseSession).  Executing the request anyway would run
  // side effects for a client that no longer owns a session; fail it as
  // a structured op error instead.  Release/Close are idempotent no-ops
  // on an absent session and do not need the guard.
  bool session_alive =
      job->type == RequestJob::Type::kReleaseHandles ||
      job->type == RequestJob::Type::kCloseSession ||
      impl->registry->exists(job->session);
  if (!session_alive) {
    fatal.symbol = "unknown-session";
    fatal.message = "session expired or closed while the request was queued";
  }

  if (session_alive) switch (job->type) {
    case RequestJob::Type::kCall: {
      CallResult r;
      run_call(env, impl->registry.get(), job->session, job->call, &r);
      results.push_back(std::move(r));
      break;
    }
    case RequestJob::Type::kWithBuffer: {
      emacs_value helper_result = nullptr;
      if (run_with_buffer(env, impl->registry.get(), job->session,
                          job->buffer_handle, job->calls, &helper_result,
                          &fatal)) {
        decode_helper_result(env, impl->registry.get(), job->session,
                             helper_result, &results, &fatal);
      }
      break;
    }
    case RequestJob::Type::kReleaseHandles:
      impl->registry->free_handles(env, job->session, job->release_ids);
      break;
    case RequestJob::Type::kCloseSession:
      impl->registry->close_session(env, job->session);
      break;
  }

  {  // publish under the slot lock; the handler may be waiting on it
    std::lock_guard<std::mutex> lk(job->slot->m);
    job->slot->results = std::move(results);
    job->slot->fatal = std::move(fatal);
    job->slot->done = true;
    job->slot->cv.notify_all();
  }
}

}  // namespace

size_t ServerCore::drain_once(emacs_env *env) {
  if (!impl_ || !impl_->server_up.load()) return 0;

  // Lease sweep (TODO 5): the drain timer fires continuously while the
  // bridge runs, so this piggybacks free of charge and runs only on the
  // Emacs thread, where freeing the expired sessions' global refs is
  // legal.  Idle sessions thus die at most one tick after their lease.
  if (impl_->lease_seconds > 0)
    impl_->registry->expire_older_than(
        env, std::chrono::seconds(impl_->lease_seconds));

  size_t n = 0;
  for (; n < impl_->max_per_tick; ++n) {
    auto job = impl_->try_pop();
    if (!job) break;

    // The client gave up while this sat in the queue: skip execution.  The
    // handler is gone, so nothing needs a completion signal.
    {
      std::lock_guard<std::mutex> lk(job->slot->m);
      if (job->slot->cancelled) continue;
    }

    try {
      dispatch_job(env, impl_.get(), job.get());
    } catch (const std::exception &e) {
      std::lock_guard<std::mutex> lk(job->slot->m);
      job->slot->results.clear();
      job->slot->fatal.symbol = "internal-error";
      job->slot->fatal.message = e.what();
      job->slot->done = true;
      job->slot->cv.notify_all();
    }
  }
  return n;
}

}  // namespace emacs_rpc_bridge
