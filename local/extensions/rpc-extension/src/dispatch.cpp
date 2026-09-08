// dispatch.cpp — Emacs-thread execution of queued requests (design-001.org
// TODO 3 "Implement dispatch and object lifetime").
//
// Skeleton only: no implementation yet, and this file is NOT part of the
// CMake target until it contains code.
//
// Architecture (design-001.org "Architecture and execution model"):
//
//   ... queue -> Elisp timer calls native drain function -> live emacs_env
//   -> Lisp function.  Results travel back through a completion queue to the
//   waiting RPC worker.
//
// The drain runs on Emacs's main thread with a fresh valid environment.  The
// bridge registers explicit start / drain / stop native functions; an Elisp
// adapter creates and cancels the timer (lisp adapter comes later).
//
// Constraints recorded from design-001.org (do not regress these):
//   - Start with polling; an event wakeup mechanism may follow later.
//   - Requests have queued/running/completed/cancelled states.  Cancellation
//     before execution can prevent execution; a client deadline after
//     execution begins does not roll back effects or guarantee interruption.
//     Never retry side-effecting calls automatically.
//   - A long-running Lisp call can still block Emacs: RPC does not make
//     arbitrary Lisp execution parallel or preemptible.
//   - Call non_local_exit_check after potentially exiting API operations;
//     extract and clear an exit before further normal API calls.  Catch C++
//     exceptions at C callback boundaries.
//   - Process queued session expiry/cleanup on the Emacs thread.  Before
//     shutdown: stop accepting requests, cancel pending work, stop the
//     timer, release references while Emacs is alive.
//
// TODO(TODO 3): queue + drain implementation, start/drain/stop native
// functions registered from module.cpp.
