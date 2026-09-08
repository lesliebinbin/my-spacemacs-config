// codegen_smoke.cpp — milestone 2 acceptance: the emacs.rpc.v1 bindings
// generated from proto/emacs_rpc.proto compile, link, and round-trip.
//
// Deliberately no Emacs and no bridge logic: this validates the protocol
// contract + toolchain only (design-001.org TODO 2).  Returns nonzero and
// prints what failed on any mismatch.

#include <emacs_rpc.pb.h>
#include <emacs_rpc.grpc.pb.h>

#include <cstdint>
#include <iostream>
#include <string>

namespace v1 = emacs::rpc::v1;

namespace {

bool check(bool ok, const char *what) {
  if (!ok) std::cerr << "FAIL: " << what << "\n";
  return ok;
}

// Serialize a message and parse it back into a fresh one of the same type.
template <typename T>
bool round_trips(const T &msg, T *parsed, const char *what) {
  std::string wire;
  if (!msg.SerializeToString(&wire)) return check(false, what);
  if (!parsed->ParseFromString(wire)) return check(false, what);
  return check(parsed->SerializeAsString() == wire, what);
}

}  // namespace

int main() {
  bool ok = true;

  // Session lifecycle message.
  v1::OpenSessionRequest open;
  open.set_protocol_version(1);
  v1::OpenSessionRequest open2;
  ok &= round_trips(open, &open2, "OpenSessionRequest round-trip");
  ok &= check(open2.protocol_version() == 1, "OpenSessionRequest version");

  // Typed values: nil, true, integer, text.
  v1::CallRequest call;
  call.set_session_id("s-1");
  call.set_correlation_id(42);
  call.mutable_op()->set_function_name("+");
  v1::Value *arg = call.mutable_op()->add_arguments();
  arg->set_integer(7);
  arg = call.mutable_op()->add_arguments();
  arg->set_integer(35);
  v1::CallRequest call2;
  ok &= round_trips(call, &call2, "CallRequest round-trip");
  ok &= check(call2.op().arguments_size() == 2 &&
                  call2.op().arguments(1).integer() == 35,
              "CallRequest integer args");
  // Out-of-range protection comes from sint64 semantics at the bridge; the
  // wire layer must reject nothing silently — presence checks suffice here.
  ok &= check(call2.correlation_id() == 42, "CallRequest correlation id");

  // Errors are first-class values distinct from transport failures.
  v1::CallResponse err;
  err.set_correlation_id(7);
  err.mutable_error()->set_symbol("wrong-type-argument");
  err.mutable_error()->set_message("readable fallback");
  v1::CallResponse err2;
  ok &= round_trips(err, &err2, "CallResponse error round-trip");
  ok &= check(err2.error().symbol() == "wrong-type-argument",
              "CallResponse error symbol");

  // WithBuffer carries a bounded sequence of ops.
  v1::WithBufferRequest wb;
  wb.set_session_id("s-1");
  wb.mutable_buffer()->set_id(3);
  wb.mutable_buffer()->set_kind("buffer");
  v1::CallOp *op1 = wb.add_calls();
  op1->set_function_name("insert");
  op1->add_arguments()->set_text("hello");
  v1::WithBufferRequest wb2;
  ok &= round_trips(wb, &wb2, "WithBufferRequest round-trip");
  ok &= check(wb2.buffer().kind() == "buffer" && wb2.calls_size() == 1 &&
                  wb2.calls(0).function_name() == "insert",
              "WithBufferRequest fields");

  v1::WithBufferResponse wbresp;
  v1::CallResponse *r0 = wbresp.add_results();
  r0->mutable_value()->set_integer(5);
  v1::WithBufferResponse wbresp2;
  ok &= round_trips(wbresp, &wbresp2, "WithBufferResponse results");
  ok &= check(wbresp2.results_size() == 1 &&
                  wbresp2.results(0).value().integer() == 5,
              "WithBufferResponse results value");

  // The generated service stub type exists (compile-time check; no channel
  // is opened here — transport tests come with the bridge milestones).
  v1::EmacsRpc::Stub *unused_stub = nullptr;
  (void)unused_stub;

  if (ok) {
    std::cout << "codegen smoke OK: emacs.rpc.v1 bindings compile, link, "
                 "and round-trip\n";
    return 0;
  }
  return 1;
}
