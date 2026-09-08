;;; emacs_bridge_server.el --- batch Emacs side of the integration harness -*- lexical-binding: t; -*-
;;;
;;; Spawned by tests/integration_test.cpp as `emacs -Q --batch'.  The harness
;;; drives the bridge from a separate process through the local Unix socket;
;;; this script only has to keep Emacs alive and pumping timer events so the
;;; drain timer can execute queued requests (design-001.org: a batch
;;; integration harness must continue pumping Emacs events; no synchronous
;;; waiting for the client).
;;;
;;; Environment:
;;;   ERB_LISP_DIR    directory of lisp/emacs-rpc-bridge.el (added to path)
;;;   ERB_READY_FILE  harness polls for this; we write the socket path to it
;;;   ERB_HALT_FILE   harness creates this file to request graceful shutdown
;;;   ERB_LEASE_SECONDS / ERB_MAX_SESSIONS / ERB_MAX_MESSAGE_BYTES
;;;                   optional start-knob overrides for the TODO 5 limits,
;;;                   quota, and lease scenarios (absent = defcustoms)

(add-to-list 'load-path (getenv "ERB_LISP_DIR"))
(require 'emacs-rpc-bridge)

;; Deterministic "original" current buffer for the restore-after-WithBuffer
;; checks (batch Emacs's initial buffer is not guaranteed).
(let ((main (get-buffer-create "*rpc-main*")))
  (set-buffer main))
(message "server: name-multibyte=%s name-len=%d"
         (multibyte-string-p (buffer-name))
         (length (buffer-name)))

;; Busy-wait SECONDS with no waits at all (no sit-for/sleep-for), so a
;; request running this genuinely blocks the drain: timers cannot fire
;; reentrantly.  The harness uses it to keep a request in flight while a
;; second one sits queued.
(defun erb-busy (seconds)
  (let ((end (+ (float-time) seconds)))
    (while (< (float-time) end))
    t))

;; Fast, single-job ticks (see below): with MAX-PER-TICK 1 a drain call that
;; starts executing one request cannot sweep up later arrivals, which is what
;; makes the shutdown-with-pending scenario deterministic.
(customize-set-variable 'emacs-rpc-bridge-timer-period 0.02)
(defun erb-env-int (name)
  "Environment variable NAME as an integer, or nil when unset."
  (let ((v (getenv name)))
    (and v (not (string-empty-p v)) (string-to-number v))))
(emacs-rpc-bridge-start
 nil nil 1
 (erb-env-int "ERB_LEASE_SECONDS")
 (erb-env-int "ERB_MAX_SESSIONS")
 (erb-env-int "ERB_MAX_MESSAGE_BYTES"))
(unless (emacs-rpc-bridge-running-p)
  (message "server: bridge failed to start: %s"
           (emacs-rpc-bridge-native-last-error))
  (kill-emacs 2))
(with-temp-file (getenv "ERB_READY_FILE")
  (insert emacs-rpc-bridge--socket-file))
(message "server: ready on %s" emacs-rpc-bridge--socket-file)

;; Pump loop: sit-for waits on the command loop, so the 0.1 s drain timer
;; fires and executes requests; check the halt file each wakeup.
(let ((halt (getenv "ERB_HALT_FILE")))
  (while (not (file-exists-p halt))
    (sit-for 0.02)))

(message "server: halt requested; shutting down")
(emacs-rpc-bridge-stop)
(kill-emacs 0)

;;; emacs_bridge_server.el ends here
