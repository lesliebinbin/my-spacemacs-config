;;; emacs-rpc-bridge.el --- Elisp adapter for the emacs-rpc-bridge module -*- lexical-binding: t; -*-

;; Copyright (C) 2026 Zhibin Huang

;; Author: Zhibin Huang
;; Keywords: extensions, rpc
;; Package-Requires: ((emacs "31.1"))

;;; Commentary:

;; Elisp side of the emacs-rpc-bridge native module (see the handoff plan in
;; runbook/emacs-rpc-bridge/design-001.org).  The module registers bare
;; native functions; this file owns the ergonomics:
;;
;;   - defcustoms for the runtime knobs (queue depth, timer period,
;;     per-tick drain bound) recorded in design-001.org "Runtime knobs";
;;   - `emacs-rpc-bridge-start' / `emacs-rpc-bridge-stop', which own the
;;     drain timer that pumps the request queue on Emacs's main thread;
;;   - the Lisp helpers the native dispatch calls.  In particular
;;     `emacs-rpc-bridge--exec-with-buffer' is the TODO 4 buffer-context
;;     helper: it executes a sequence of calls with a buffer bound as
;;     current under save-current-buffer.  Its exact return contract is
;;     decoded by src/dispatch.cpp (see the file comment there) and must
;;     not change without changing the C++ decoder:
;;
;;       (emacs-rpc-bridge--exec-with-buffer BUFFER CALLS) => VECTOR
;;         CALLS: vector of [FUNCTION-SYMBOL ARGUMENTS-VECTOR]
;;         per executed op, one entry:
;;           [ok VALUE]                     op returned VALUE
;;           [error "SYM" "readable msg"]    op signalled; sequence stops
;;           [fatal "SYM" "readable msg"]    could not start the sequence
;;                                          (e.g. dead buffer handle)
;;
;; Loading this file never starts the bridge: call `emacs-rpc-bridge-start'
;; explicitly (no automatic startup, per design-001.org).  Loading also
;; registers `emacs-rpc-bridge-stop' on kill-emacs-hook so the module can
;; free its references while Emacs is still alive.

;;; Code:

(defgroup emacs-rpc-bridge nil
  "Local gRPC bridge into Emacs (native module emacs-rpc-bridge)."
  :group 'extensions
  :prefix "emacs-rpc-bridge-")

;; State vars precede the defcustoms: custom-initialize-reset runs each
;; defcustom's :set function at load time, and the timer-period :set refers
;; to these.
(defvar emacs-rpc-bridge--timer nil
  "The drain timer while the bridge runs, or nil.")
(defvar emacs-rpc-bridge--socket-file nil
  "Unix socket path passed to native-start, kept for restarts.")

(defcustom emacs-rpc-bridge-module-file nil
  "Absolute file name of the compiled libemacs-rpc-bridge module.
Nil means to guess from this file's location: the module lives one
directory up in build/ when loaded from a source checkout."
  :type '(choice (const :tag "Guess from source checkout" nil)
                 (file :must-match t))
  :group 'emacs-rpc-bridge)

(defcustom emacs-rpc-bridge-queue-depth 100
  "Request queue capacity; full queue exerts wait-backpressure on clients.
Fixed at `emacs-rpc-bridge-native-start' time: changing it takes effect
after a stop/start cycle."
  :type 'integer
  :group 'emacs-rpc-bridge)

(defcustom emacs-rpc-bridge-timer-period 0.1
  "Seconds between drain ticks while the bridge runs.
Each tick calls the native drain, which executes at most
`emacs-rpc-bridge-max-drain-per-tick' queued requests.  Live: setting this
while the bridge runs cancels and recreates the timer."
  :type 'number
  :set (lambda (sym val)
         (set-default sym val)
         (when (timerp emacs-rpc-bridge--timer)
           (emacs-rpc-bridge-stop)
           (emacs-rpc-bridge-start emacs-rpc-bridge--socket-file)))
  :group 'emacs-rpc-bridge)

(defcustom emacs-rpc-bridge-max-drain-per-tick 20
  "Upper bound of requests executed per drain tick.
Bounds the longest single stall of the editor to one tick's slice;
remaining requests wait for the next tick.  Fixed at native-start time:
changing it takes effect after a stop/start cycle."
  :type 'integer
  :group 'emacs-rpc-bridge)

(defcustom emacs-rpc-bridge-lease-seconds 300
  "Inactivity lease in seconds: sessions idle this long are closed and
their handles freed.  A session's activity is refreshed by every request;
disconnected clients therefore cannot leak handles indefinitely.  0
disables expiry.  Fixed at native-start time: changing it takes effect
after a stop/start cycle."
  :type 'integer
  :group 'emacs-rpc-bridge)

(defcustom emacs-rpc-bridge-max-sessions 32
  "Concurrent-session quota; OpenSession fails with RESOURCE_EXHAUSTED
when full.  0 allows unlimited sessions.  Fixed at native-start time:
changing it takes effect after a stop/start cycle."
  :type 'integer
  :group 'emacs-rpc-bridge)

(defcustom emacs-rpc-bridge-max-message-bytes 0
  "Inbound message-size cap in bytes (0 = the gRPC default of 4 MiB).
Oversized requests are rejected by the server transport with
RESOURCE_EXHAUSTED before they reach the queue.  Fixed at native-start
time: changing it takes effect after a stop/start cycle."
  :type 'integer
  :group 'emacs-rpc-bridge)

;; ---- module loading ---------------------------------------------------------

(defvar emacs-rpc-bridge--source-dir nil
  "Directory this file was loaded from; captured at load time because
`load-file-name' is nil again as soon as loading finishes.")

(defun emacs-rpc-bridge--module-file ()
  "Resolve the native module file name."
  (or emacs-rpc-bridge-module-file
      (and emacs-rpc-bridge--source-dir
           (expand-file-name "../build/libemacs-rpc-bridge.so"
                             emacs-rpc-bridge--source-dir))))

(defun emacs-rpc-bridge--load-module ()
  "Load the native module unless it is already present."
  (unless (fboundp 'emacs-rpc-bridge-native-start)
    (let ((file (emacs-rpc-bridge--module-file)))
      (unless (and file (file-exists-p file))
        (user-error "emacs-rpc-bridge module not found (set \
`emacs-rpc-bridge-module-file'); try building with `mise build'"))
      (module-load file)))
  (unless (fboundp 'emacs-rpc-bridge-native-start)
    (error "emacs-rpc-bridge module loaded but native functions missing")))

;; ---- start / stop -----------------------------------------------------------

(defun emacs-rpc-bridge--socket-path ()
  "Per-user Unix socket path, inside an owner-only runtime directory.
Prefers XDG_RUNTIME_DIR (already private); otherwise a private
subdirectory of the temporary directory, chmod 700."
  (let ((dir (getenv "XDG_RUNTIME_DIR")))
    (if (and dir (file-directory-p dir))
        (expand-file-name (format "emacs-rpc-bridge-%s.sock" (user-uid)) dir)
      (let ((private (expand-file-name "emacs-rpc-bridge"
                                       (file-temporary-directory))))
        (make-directory private t)
        (set-file-modes private #o700)
        (expand-file-name (format "emacs-rpc-bridge-%s.sock" (user-uid))
                          private)))))

;;;###autoload
(defun emacs-rpc-bridge-start (&optional socket-file queue-depth max-per-tick
                                        lease-seconds max-sessions
                                        max-message-bytes)
  "Start the bridge on Unix socket SOCKET-FILE (per-user default).
Optional QUEUE-DEPTH, MAX-PER-TICK, LEASE-SECONDS, MAX-SESSIONS and
MAX-MESSAGE-BYTES override the defcustoms for this run (see each
defcustom for semantics; nil means the defcustom value).  Starts the
drain timer; returns the socket path once the server is bound and
running."
  (interactive)
  (emacs-rpc-bridge--load-module)
  (when (emacs-rpc-bridge-running-p)
    (user-error "emacs-rpc-bridge is already running"))
  (let ((sock (or socket-file (emacs-rpc-bridge--socket-path))))
    (when (file-exists-p sock) (delete-file sock))
    (apply #'emacs-rpc-bridge-native-start
           (list sock
                 (or queue-depth emacs-rpc-bridge-queue-depth)
                 (or max-per-tick emacs-rpc-bridge-max-drain-per-tick)
                 (or lease-seconds emacs-rpc-bridge-lease-seconds)
                 (or max-sessions emacs-rpc-bridge-max-sessions)
                 (or max-message-bytes emacs-rpc-bridge-max-message-bytes)))
    ;; native-start returns before the gRPC server has bound; poll briefly.
    (let ((i 0) err)
      (while (and (< i 100)
                  (not (emacs-rpc-bridge-native-running-p)))
        (setq i (1+ i))
        (sleep-for 0.01)
        (setq err (emacs-rpc-bridge-native-last-error))
        (when err
          (setq i 100)))
      (unless (emacs-rpc-bridge-native-running-p)
        (user-error "emacs-rpc-bridge failed to start: %s"
                    (or err "unknown reason"))))
    (setq emacs-rpc-bridge--socket-file sock)
    (setq emacs-rpc-bridge--timer
          (run-at-time emacs-rpc-bridge-timer-period
                       emacs-rpc-bridge-timer-period
                       #'emacs-rpc-bridge--tick))
    sock))

;;;###autoload
(defun emacs-rpc-bridge-stop ()
  "Stop the bridge: cancel the drain timer, stop the server, delete the
socket, and release all session references inside the native module."
  (interactive)
  (when (timerp emacs-rpc-bridge--timer)
    (cancel-timer emacs-rpc-bridge--timer)
    (setq emacs-rpc-bridge--timer nil))
  (when (fboundp 'emacs-rpc-bridge-native-stop)
    (emacs-rpc-bridge-native-stop))
  (when (and emacs-rpc-bridge--socket-file
             (file-exists-p emacs-rpc-bridge--socket-file))
    (delete-file emacs-rpc-bridge--socket-file))
  (setq emacs-rpc-bridge--socket-file nil)
  t)

;;;###autoload
(defun emacs-rpc-bridge-running-p ()
  "Non-nil while the bridge server is bound and draining."
  (and (fboundp 'emacs-rpc-bridge-native-running-p)
       (emacs-rpc-bridge-native-running-p)))

;; ---- drain timer ------------------------------------------------------------

(defun emacs-rpc-bridge--tick ()
  "Drain tick: execute queued requests (see design-001.org).
Errors are reported to *Messages*; the timer itself must survive."
  (condition-case err
      (emacs-rpc-bridge-native-drain)
    (error
     (message "emacs-rpc-bridge drain error: %s"
              (error-message-string err)))))

;; ---- helpers invoked by the native dispatch --------------------------------

(defun emacs-rpc-bridge--call-safely (fn &rest args)
  "Call FN with ARGS, returning (ok . VALUE) or (error SYM MSG).
Never signals: caller checks the ok/error shape."
  (condition-case err
      (cons 'ok (apply fn args))
    (error
     (list 'error (symbol-name (car err))
           (or (error-message-string err) (prin1-to-string err))))))

(defun emacs-rpc-bridge--exec-with-buffer (buffer calls)
  "Run CALLS with BUFFER bound as current; see Commentary for the contract.
Each CALLS element is [FUNCTION-SYMBOL ARGUMENTS-VECTOR]; the arguments
vector is converted to a list and applied.  The sequence stops at the
first failing op; earlier
effects are kept (contextual grouping, not a transaction).  A failure to
establish the buffer context at all (e.g. a killed buffer) yields a fatal
entry and no per-op results."
  (condition-case fatal
      (save-current-buffer
        ;; Signals (wrong-type-argument, killed-buffer ...) are caught by
        ;; the outer condition-case as a fatal entry.
        (set-buffer buffer)
        (let ((entries nil)
              (n (length calls))
              (i 0)
              (stopped nil))
          (while (and (< i n) (not stopped))
            (let* ((op (aref calls i))
                   ;; apply spreads only lists; the args travel as a vector.
                   (entry
                    (condition-case err
                        (vector 'ok
                                (apply (aref op 0)
                                       (append (aref op 1) nil)))
                      (error
                       (setq stopped t)
                       (vector 'error (symbol-name (car err))
                               (or (error-message-string err)
                                   (prin1-to-string err)))))))
              (push entry entries)
              (setq i (1+ i))))
          (vconcat (nreverse entries))))
    (error
     (vector (vector 'fatal (symbol-name (car fatal))
                     (or (error-message-string fatal)
                         (prin1-to-string fatal)))))))

(add-hook 'kill-emacs-hook #'emacs-rpc-bridge-stop)

;; Set only when really loaded from a file (not eval-buffer).
(when load-file-name
  (setq emacs-rpc-bridge--source-dir (file-name-directory load-file-name)))

(provide 'emacs-rpc-bridge)
;;; emacs-rpc-bridge.el ends here
