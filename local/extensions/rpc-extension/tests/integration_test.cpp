// integration_test.cpp — end-to-end harness (design-001.org TODO 3
// acceptance): spawns its own batch Emacs running tests/emacs_bridge_server.el,
// connects through the local Unix socket with the generated client, and drives
// every object-lifetime and dispatch scenario from a separate process.
//
// Thread rules of the design apply to the client too: each scenario is a
// normal gRPC request with a deadline; the Emacs side keeps pumping timer
// events (its script does no synchronous waiting for us).  Every RPC carries a
// deadline and the whole run has a watchdog alarm, so a deadlock surfaces as
// a timed-out FAIL instead of a hung test.
//
// Run standalone (exit 0 = all checks passed) or via ctest.

#include <emacs_rpc.grpc.pb.h>
#include <emacs_rpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
namespace v1 = emacs::rpc::v1;
using std::chrono::steady_clock;
using std::chrono::system_clock;

const char *kSourceDir = RPC_SRC_DIR;
constexpr auto kRpcDeadline = std::chrono::seconds(5);
constexpr auto kReadyTimeout = std::chrono::seconds(10);

int g_checks = 0;
int g_failures = 0;

void report(bool ok, const std::string &name) {
  ++g_checks;
  if (ok) {
    std::printf("PASS: %s\n", name.c_str());
  } else {
    ++g_failures;
    std::printf("FAIL: %s\n", name.c_str());
  }
}

// ---- value builders ---------------------------------------------------------

v1::Value vint(int64_t v) { v1::Value x; x.set_integer(v); return x; }
v1::Value vreal(double v) { v1::Value x; x.set_real(v); return x; }
v1::Value vtext(const std::string &s) { v1::Value x; x.set_text(s); return x; }
v1::Value vsym(const std::string &s) { v1::Value x; x.set_symbol_name(s); return x; }
v1::Value vnil() { v1::Value x; x.set_nil_value(true); return x; }
v1::Value vtrue() { v1::Value x; x.set_true_value(true); return x; }
v1::Value vhandle(uint64_t id) { v1::Value x; x.mutable_handle()->set_id(id); return x; }

// ---- RPC helpers ------------------------------------------------------------

class Client {
 public:
  explicit Client(const std::string &socket)
      : channel_(grpc::CreateChannel("unix:" + socket,
                                     grpc::InsecureChannelCredentials())),
        stub_(v1::EmacsRpc::NewStub(channel_)) {}

  grpc::Status open(int32_t version, std::string *session_out,
                    std::string *emacs_version_out) {
    v1::OpenSessionRequest req;
    req.set_protocol_version(version);
    v1::OpenSessionResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(deadline());
    grpc::Status st = stub()->OpenSession(&ctx, req, &resp);
    if (st.ok()) {
      *session_out = resp.session_id();
      *emacs_version_out = resp.emacs_version();
    }
    return st;
  }

  struct CallResult {
    grpc::Status status;
    v1::CallResponse resp;
  };

  CallResult call(const std::string &session, uint64_t corr,
                  const std::string &fn,
                  std::initializer_list<v1::Value> args) {
    v1::CallRequest req;
    req.set_session_id(session);
    req.set_correlation_id(corr);
    req.mutable_op()->set_function_name(fn);
    for (const v1::Value &a : args) *req.mutable_op()->add_arguments() = a;
    CallResult out;
    grpc::ClientContext ctx;
    ctx.set_deadline(deadline());
    out.status = stub()->Call(&ctx, req, &out.resp);
    return out;
  }

  struct WithBufferResult {
    grpc::Status status;
    v1::WithBufferResponse resp;
  };

  WithBufferResult with_buffer(const std::string &session, uint64_t corr,
                               uint64_t buffer_handle,
                               std::initializer_list<v1::CallOp> calls) {
    v1::WithBufferRequest req;
    req.set_session_id(session);
    req.set_correlation_id(corr);
    req.mutable_buffer()->set_id(buffer_handle);
    for (const v1::CallOp &c : calls) *req.add_calls() = c;
    WithBufferResult out;
    grpc::ClientContext ctx;
    ctx.set_deadline(deadline());
    out.status = stub()->WithBuffer(&ctx, req, &out.resp);
    return out;
  }

  grpc::Status release_handles(const std::string &session,
                               std::initializer_list<uint64_t> ids) {
    v1::ReleaseHandlesRequest req;
    req.set_session_id(session);
    for (uint64_t id : ids) req.add_handles()->set_id(id);
    v1::ReleaseHandlesResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(deadline());
    return stub()->ReleaseHandles(&ctx, req, &resp);
  }

  grpc::Status close_session(const std::string &session) {
    v1::CloseSessionRequest req;
    req.set_session_id(session);
    v1::CloseSessionResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(deadline());
    return stub()->CloseSession(&ctx, req, &resp);
  }

 private:
  static std::chrono::time_point<system_clock> deadline() {
    return system_clock::now() + kRpcDeadline;
  }

  std::shared_ptr<grpc::Channel> channel_;
  // Generated sync stubs are safe for concurrent calls from multiple threads,
  // so the shutdown-with-pending test can share this one.
  std::unique_ptr<v1::EmacsRpc::Stub> stub_;

 public:
  v1::EmacsRpc::Stub *stub() { return stub_.get(); }
};

v1::CallOp op(const std::string &fn, std::initializer_list<v1::Value> args) {
  v1::CallOp o;
  o.set_function_name(fn);
  for (const v1::Value &a : args) *o.add_arguments() = a;
  return o;
}

// Payload of a text/unibyte value as raw bytes (either string kind carries
// UTF-8 bytes for our purposes); "" when the value is not a string kind.
std::string bytes_of(const v1::Value &v) {
  switch (v.kind_case()) {
    case v1::Value::kText: return v.text();
    case v1::Value::kUnibyte: return v.unibyte();
    default: return "";
  }
}

// Read a whole file; empty string on error.
std::string read_file(const std::string &path) {
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// ---- Emacs subprocess -------------------------------------------------------

pid_t g_child = -1;
std::string g_log_path;

void on_alarm(int) {
  std::printf("FAIL: harness watchdog fired; killing Emacs child\n");
  if (g_child > 0) kill(g_child, SIGKILL);
  _exit(1);
}

// Spawn `emacs -Q --batch' with the server script; returns once the bridge
// reports ready (socket path in the ready file) or the child dies.
std::string spawn_emacs() {
  g_log_path = "/tmp/erb-it-" + std::to_string(getpid()) + ".log";
  std::string ready = "/tmp/erb-it-" + std::to_string(getpid()) + ".ready";
  std::string halt = "/tmp/erb-it-" + std::to_string(getpid()) + ".halt";
  ::unlink(ready.c_str());
  ::unlink(halt.c_str());

  pid_t pid = fork();
  if (pid == 0) {
    int fd = open(g_log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
    setenv("ERB_LISP_DIR", (std::string(kSourceDir) + "/lisp").c_str(), 1);
    setenv("ERB_READY_FILE", ready.c_str(), 1);
    setenv("ERB_HALT_FILE", halt.c_str(), 1);
    const char *emacs = getenv("ERB_EMACS");
    execlp(emacs ? emacs : "emacs", emacs ? emacs : "emacs", "-Q", "--batch",
           "-l", (std::string(kSourceDir) + "/tests/emacs_bridge_server.el").c_str(),
           static_cast<char *>(nullptr));
    _exit(127);
  }
  g_child = pid;

  auto start = steady_clock::now();
  while (steady_clock::now() - start < kReadyTimeout) {
    int st = 0;
    if (waitpid(pid, &st, WNOHANG) == pid) {
      std::printf("FAIL: Emacs child exited early (status %d); log:\n%s\n",
                  st, read_file(g_log_path).c_str());
      exit(1);
    }
    if (access(ready.c_str(), F_OK) == 0) {
      std::string sock = read_file(ready);
      // Trim trailing newline written by with-temp-file.
      while (!sock.empty() && (sock.back() == '\n' || sock.back() == '\r'))
        sock.pop_back();
      return sock;
    }
    usleep(20'000);
  }
  std::printf("FAIL: Emacs never became ready; log:\n%s\n",
              read_file(g_log_path).c_str());
  kill(pid, SIGKILL);
  waitpid(pid, nullptr, 0);
  exit(1);
}

// Ask the server to shut down gracefully and wait for it to exit 0.
void halt_emacs() {
  std::string halt = "/tmp/erb-it-" + std::to_string(getpid()) + ".halt";
  std::ofstream(halt).close();
  auto start = steady_clock::now();
  int st = 0;
  while (steady_clock::now() - start < kReadyTimeout) {
    if (waitpid(g_child, &st, WNOHANG) == g_child) return;
    usleep(20'000);
  }
  std::printf("FAIL: Emacs did not exit after halt; log:\n%s\n",
              read_file(g_log_path).c_str());
  kill(g_child, SIGKILL);
  waitpid(g_child, nullptr, 0);
  exit(1);
}

// ---- scenario building blocks -----------------------------------------------

uint64_t handle_of(const v1::CallResponse &resp) {
  return resp.value().handle().id();
}

}  // namespace

int main() {
  std::signal(SIGALRM, on_alarm);
  alarm(90);  // whole-run watchdog

  std::string socket = spawn_emacs();
  report(true, "spawned batch Emacs; bridge ready on " + socket);

  Client c(socket);

  // ---- sessions -------------------------------------------------------------
  std::string sA, sB;
  std::string emacs_ver;
  grpc::Status st = c.open(1, &sA, &emacs_ver);
  report(st.ok(), "OpenSession v1 accepted");
  if (!st.ok()) std::printf("  detail: %s\n", st.error_message().c_str());
  report(!sA.empty(), "session token non-empty");
  report(emacs_ver.find("GNU Emacs") != std::string::npos,
         "emacs_version reports GNU Emacs");

  st = c.open(0, &sA, &emacs_ver);
  report(st.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
         "OpenSession v0 rejected (INVALID_ARGUMENT)");

  st = c.open(1, &sA, &emacs_ver);
  report(st.ok(), "re-open after rejection");
  st = c.open(1, &sB, &emacs_ver);
  report(st.ok(), "second independent session");

  // ---- Call: scalars --------------------------------------------------------
  auto r = c.call(sA, 1, "+", {vint(3), vint(7)});
  bool plus_ok = r.status.ok() &&
                 r.resp.value().kind_case() == v1::Value::kInteger &&
                 r.resp.value().integer() == 10;
  report(plus_ok, "Call(+ 3 7) == integer 10");
  if (!plus_ok) {
    std::printf("  debug: status=%s code=%d has_error=%d sym='%s' msg='%s'\n",
                r.status.ok() ? "OK" : r.status.error_message().c_str(),
                static_cast<int>(r.status.error_code()),
                r.resp.has_error() ? 1 : 0,
                r.resp.has_error() ? r.resp.error().symbol().c_str() : "",
                r.resp.has_error() ? r.resp.error().message().c_str() : "");
  }

  r = c.call(sA, 2, "+", {vreal(1.5), vreal(2.25)});
  report(r.status.ok() && r.resp.value().kind_case() == v1::Value::kReal &&
             r.resp.value().real() == 3.75,
         "Call(+ 1.5 2.25) == real 3.75");

  r = c.call(sA, 3, "concat", {vtext("αβ"), vtext("γδ")});
  report(r.status.ok() && r.resp.value().kind_case() == v1::Value::kText &&
             r.resp.value().text() == "αβγδ",
         "Call(concat) round-trips multibyte text");

  r = c.call(sA, 4, "not", {vnil()});
  report(r.status.ok() && r.resp.value().kind_case() == v1::Value::kTrueValue,
         "Call(not nil) == t");
  r = c.call(sA, 5, "null", {vtrue()});
  report(r.status.ok() && r.resp.value().kind_case() == v1::Value::kNilValue,
         "Call(null t) == nil");

  r = c.call(sA, 6, "intern", {vtext("abc")});
  report(r.status.ok() && r.resp.value().kind_case() == v1::Value::kSymbolName &&
             r.resp.value().symbol_name() == "abc",
         "Call(intern \"abc\") == symbol abc");
  r = c.call(sA, 7, "symbol-name", {vsym("abc")});
  report(r.status.ok() && r.resp.value().kind_case() == v1::Value::kText &&
             r.resp.value().text() == "abc",
         "symbol argument round-trips into symbol-name");

  // ---- Call: handle arguments ------------------------------------------------
  r = c.call(sA, 8, "list", {vint(1), vtext("two")});
  report(r.status.ok() && r.resp.value().kind_case() == v1::Value::kHandle,
         "compound result (list) becomes a handle");
  uint64_t hL = handle_of(r.resp);
  r = c.call(sA, 9, "car", {vhandle(hL)});
  report(r.status.ok() && r.resp.value().kind_case() == v1::Value::kInteger &&
             r.resp.value().integer() == 1,
         "Call(car <list handle>) == 1");
  r = c.call(sA, 10, "nth", {vint(1), vhandle(hL)});
  report(r.status.ok() && r.resp.value().kind_case() == v1::Value::kText &&
             r.resp.value().text() == "two",
         "Call(nth 1 <list handle>) == \"two\"");

  // ---- buffer handles + WithBuffer -------------------------------------------
  const std::string unicode = "héllo 🌍 日本語";
  r = c.call(sA, 11, "get-buffer-create", {vtext("*it-ünïcode*")});
  report(r.status.ok() && r.resp.value().kind_case() == v1::Value::kHandle,
         "get-buffer-create returns a buffer handle");
  uint64_t hB = handle_of(r.resp);

  r = c.call(sA, 12, "buffer-name", {vhandle(hB)});
  report(r.status.ok() && r.resp.value().kind_case() == v1::Value::kText &&
             r.resp.value().text() == "*it-ünïcode*",
         "buffer handle usable as a plain Call argument");

  auto wb = c.with_buffer(sA, 13, hB,
                          {op("insert", {vtext(unicode)}),
                           op("buffer-string", {})});
  report(wb.status.ok() && wb.resp.results_size() == 2 &&
             !wb.resp.results(0).has_error() &&
             wb.resp.results(1).value().kind_case() == v1::Value::kText &&
             wb.resp.results(1).value().text() == unicode,
         "WithBuffer: insert + buffer-string reads back identical Unicode");

  r = c.call(sA, 14, "buffer-name", {});
  // *rpc-main* is created from an ASCII-only script literal, so Emacs may
  // legitimately report its name as unibyte; accept either string kind.
  bool main_restored = r.status.ok() && bytes_of(r.resp.value()) == "*rpc-main*";
  report(main_restored, "current buffer restored after WithBuffer");

  // ---- GC retention ----------------------------------------------------------
  r = c.call(sA, 15, "garbage-collect", {});
  report(r.status.ok(), "explicit garbage-collect succeeds");
  wb = c.with_buffer(sA, 16, hB, {op("buffer-string", {})});
  report(wb.status.ok() && wb.resp.results_size() == 1 &&
             wb.resp.results(0).value().kind_case() == v1::Value::kText &&
             wb.resp.results(0).value().text() == unicode,
         "buffer handle survives explicit GC");

  // ---- error stops sequence, effects kept, context restored ------------------
  wb = c.with_buffer(sA, 17, hB,
                     {op("insert", {vtext("more")}),
                      op("car", {vint(5)}),  // wrong-type-argument listp 5
                      op("buffer-string", {})});
  report(wb.status.ok() && wb.resp.results_size() == 2 &&
             !wb.resp.results(0).has_error() &&
             wb.resp.results(1).has_error() &&
             wb.resp.results(1).error().symbol() == "wrong-type-argument",
         "signalled op stops the sequence with a structured error");
  r = c.call(sA, 18, "buffer-name", {});
  report(r.status.ok() && bytes_of(r.resp.value()) == "*rpc-main*",
         "current buffer restored after in-sequence error");
  wb = c.with_buffer(sA, 19, hB, {op("buffer-string", {})});
  report(wb.status.ok() && wb.resp.results(0).value().kind_case() == v1::Value::kText &&
             wb.resp.results(0).value().text() == unicode + "more",
         "earlier edits kept after error (no rollback)");

  // ---- error recovery --------------------------------------------------------
  r = c.call(sA, 20, "+", {vint(40), vint(2)});
  report(r.status.ok() && r.resp.value().integer() == 42,
         "Emacs usable after an error: Call(+ 40 2) == 42");

  // ---- release then reuse fails ----------------------------------------------
  st = c.release_handles(sA, {hL});
  report(st.ok(), "ReleaseHandles succeeds for a list handle");
  r = c.call(sA, 21, "car", {vhandle(hL)});
  report(r.status.ok() && r.resp.has_error() &&
             r.resp.error().message().find("released handle") !=
                 std::string::npos,
         "released list handle reuse is a per-op error");
  st = c.release_handles(sA, {hL});
  report(st.ok(), "re-releasing an unknown handle is a no-op");

  st = c.release_handles(sA, {hB});
  report(st.ok(), "ReleaseHandles succeeds for a buffer handle");
  wb = c.with_buffer(sA, 22, hB, {op("buffer-name", {})});
  report(wb.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION &&
             wb.status.error_message().find("unknown buffer handle") !=
                 std::string::npos,
         "WithBuffer on a released buffer handle rejected");

  // ---- killed buffer ----------------------------------------------------------
  r = c.call(sA, 23, "get-buffer-create", {vtext("*rpc-doomed*")});
  uint64_t hD = r.status.ok() ? handle_of(r.resp) : 0;
  report(hD != 0, "get-buffer-create for the doomed buffer");
  r = c.call(sA, 24, "kill-buffer", {vhandle(hD)});
  report(r.status.ok() && r.resp.value().kind_case() == v1::Value::kTrueValue,
         "Call(kill-buffer <handle>) succeeds (global ref does not prevent it)");
  wb = c.with_buffer(sA, 25, hD, {op("buffer-name", {})});
  report(wb.status.ok() && wb.resp.results_size() == 0 &&
             wb.resp.has_error() &&
             wb.resp.error().message().find("Selecting deleted buffer") !=
                 std::string::npos,
         "WithBuffer on a killed buffer is a fatal error, not a crash");

  // ---- session isolation ------------------------------------------------------
  r = c.call(sA, 26, "get-buffer-create", {vtext("*rpc-iso*")});
  uint64_t hB2 = r.status.ok() ? handle_of(r.resp) : 0;
  report(hB2 != 0, "session A creates another buffer handle");
  wb = c.with_buffer(sB, 27, hB2, {op("buffer-name", {})});
  report(wb.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION &&
             wb.status.error_message().find("unknown buffer handle") !=
                 std::string::npos,
         "session B cannot use session A's buffer handle");
  r = c.call(sB, 28, "car", {vhandle(hB2)});
  report(r.status.ok() && r.resp.has_error() &&
             r.resp.error().message().find("handle") != std::string::npos,
         "session B cannot use session A's handle as a Call argument");

  // ---- limits (no Emacs involvement) ------------------------------------------
  v1::WithBufferRequest big;
  big.set_session_id(sA);
  big.mutable_buffer()->set_id(hB2);
  for (int i = 0; i < 1001; ++i) big.add_calls()->set_function_name("+");
  v1::WithBufferResponse bigresp;
  grpc::ClientContext bigctx;
  bigctx.set_deadline(system_clock::now() + kRpcDeadline);
  grpc::Status bigst = c.stub()->WithBuffer(&bigctx, big, &bigresp);
  report(bigst.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
         "WithBuffer with >1000 calls rejected before dispatch");

  // ---- close session ----------------------------------------------------------
  st = c.close_session(sB);
  report(st.ok(), "CloseSession succeeds");
  r = c.call(sB, 29, "+", {vint(1), vint(1)});
  report(r.status.error_code() == grpc::StatusCode::FAILED_PRECONDITION &&
             r.status.error_message().find("unknown session") !=
                 std::string::npos,
         "calls on a closed session rejected");
  r = c.call(sA, 30, "+", {vint(1), vint(1)});
  report(r.status.ok() && r.resp.value().integer() == 2,
         "session A unaffected by closing session B");

  // ---- shutdown with pending requests -----------------------------------------
  // The server runs with MAX-PER-TICK 1, so once the slow request is being
  // executed the drain cannot sweep up later arrivals; erb-busy blocks Emacs's
  // thread with no waits, so no timer can fire reentrantly.  Timeline:
  //   t+0.00  slow request (erb-busy 1.0) is dispatched on the next tick
  //   t+0.20  second request enqueues; Emacs's thread is inside erb-busy, so
  //           its handler waits on the completion slot
  //   t+0.40  halt file written; invisible to Emacs until erb-busy returns
  //   t+1.0x  erb-busy returns; the pump loop sees the halt and stops: the
  //           queued request is cancelled by begin_stop/cancel_queued and its
  //           handler must give up — no hang, no deadlock.
  {
    std::string sC;
    report(c.open(1, &sC, &emacs_ver).ok(), "session C for shutdown test");
    auto start_wall = steady_clock::now();
    grpc::Status slow_st, pending_st;
    v1::CallResponse slow_resp, pending_resp;
    std::thread slow([&] {
      v1::CallRequest req;
      req.set_session_id(sC);
      req.set_correlation_id(31);
      req.mutable_op()->set_function_name("erb-busy");
      req.mutable_op()->add_arguments()->set_real(1.0);
      v1::CallResponse resp;
      grpc::ClientContext ctx;
      ctx.set_deadline(system_clock::now() + kRpcDeadline);
      slow_st = c.stub()->Call(&ctx, req, &resp);
      slow_resp = resp;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::thread pending([&] {
      v1::CallRequest req;
      req.set_session_id(sC);
      req.set_correlation_id(32);
      req.mutable_op()->set_function_name("+");
      req.mutable_op()->add_arguments()->set_integer(1);
      req.mutable_op()->add_arguments()->set_integer(1);
      v1::CallResponse resp;
      grpc::ClientContext ctx;
      ctx.set_deadline(system_clock::now() + kRpcDeadline);
      pending_st = c.stub()->Call(&ctx, req, &resp);
      pending_resp = resp;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Emacs's thread is inside erb-busy(1.0); stop while the second request
    // is queued and its handler is waiting.
    halt_emacs();
    slow.join();
    pending.join();
    report(slow_st.ok() && slow_resp.value().kind_case() == v1::Value::kTrueValue,
           "running call completed before shutdown");
    if (!(slow_st.ok() && slow_resp.value().kind_case() == v1::Value::kTrueValue))
      std::printf("  debug: slow status=%s code=%d msg='%s'\n",
                  slow_st.ok() ? "OK" : slow_st.error_message().c_str(),
                  static_cast<int>(slow_st.error_code()),
                  slow_st.ok() && slow_resp.has_error()
                      ? slow_resp.error().message().c_str()
                      : "");
    bool pending_clean =
        (pending_st.ok() && pending_resp.value().kind_case() ==
                                v1::Value::kInteger &&
         pending_resp.value().integer() == 2) ||
        pending_st.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ||
        pending_st.error_code() == grpc::StatusCode::CANCELLED;
    report(pending_clean,
           "queued request terminated by shutdown without hanging");
    if (!pending_clean)
      std::printf("  debug: pending status=%s code=%d msg='%s'\n",
                  pending_st.ok() ? "OK" : pending_st.error_message().c_str(),
                  static_cast<int>(pending_st.error_code()),
                  pending_st.ok() && pending_resp.has_error()
                      ? pending_resp.error().message().c_str()
                      : "");
    report(slow_st.ok() && pending_clean,
           "no deadlock across stop while requests in flight");
    report(steady_clock::now() - start_wall < std::chrono::seconds(10),
           "shutdown finished within the time bound");
    g_child = -1;  // already reaped by halt_emacs
  }

  std::printf("== %d checks, %d failure(s) ==\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
