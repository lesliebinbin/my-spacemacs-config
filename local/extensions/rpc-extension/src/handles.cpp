// handles.cpp — session-scoped object handle table (design-001.org TODO 3
// "Implement dispatch and object lifetime").
//
// Skeleton only: no implementation yet, and this file is NOT part of the
// CMake target until it contains code.
//
// Purpose: map session-scoped opaque handle ids (proto/emacs_rpc.proto
// Handle) to persistent Emacs references so external clients can name
// buffers, windows, frames, and other Lisp objects across calls.
//
// Constraints recorded from design-001.org (do not regress these):
//   - Keep persistent objects with make_global_ref and release them through
//     free_global_ref ONLY in a valid Emacs context (i.e. on the Emacs
//     thread, inside a callback).  Temporary local values must not outlive
//     their callback.
//   - Reject wrong-session, expired, and released handles.
//   - Keeping a buffer alive does not prevent kill-buffer; operations must
//     handle dead buffers correctly.
//   - Enforce quotas and inactivity leases so disconnected clients do not
//     leak references indefinitely.  Cleanup runs on the Emacs thread.
//   - Never serialize an address or emacs_value; the id is opaque.  A
//     handle's "kind" is informational, not trusted type validation.
//
// TODO(TODO 3): handle table implementation; release semantics wired into
// ReleaseHandles / CloseSession / session expiry.
