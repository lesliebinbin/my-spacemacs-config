// server.cpp — network-facing endpoint (design-001.org TODO 3 "Implement
// dispatch and object lifetime").
//
// Skeleton only: no implementation yet, and this file is NOT part of the
// CMake target until it contains code.  It will host the local gRPC server:
//
//   external client -> Unix-domain socket -> bounded request queue
//
// Constraints recorded from design-001.org (do not regress these):
//   - Unix-domain socket in a private per-user runtime directory, owner-only
//     permissions; never a remotely reachable TCP address.
//   - Network workers decode messages and wait for completions.  They must
//     NEVER call the Emacs API, dereference Lisp handles, or retain
//     emacs_env* for later use.  Only the Emacs-thread drain (dispatch.cpp)
//     touches Lisp.
//   - Bound both queue length and work drained per tick; do not hold queue
//     locks while calling Lisp.
//   - Stop accepting requests before shutdown; do not block the Emacs thread
//     joining a worker that is waiting for Emacs dispatch (see dispatch.cpp).
//
// TODO(TODO 3): gRPC service implementation for the emacs.rpc.v1 contract in
// proto/emacs_rpc.proto; generated bindings require an installed protobuf +
// gRPC toolchain (ask the user before installing; no silent downloads).
