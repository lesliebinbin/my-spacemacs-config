;; -*- mode: emacs-lisp; lexical-binding: t -*-

(defun dotspacemacs/user-config ()
  "Configuration for user code:
This function is called at the very end of Spacemacs startup, after layer
configuration.
Put your configuration code here, except for variables that should be set
before packages are loaded."
  (custom/spacemacs-load-user-custom-via-org
   "user-config/native-compile.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/shell-path.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/terminal-clipboard.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/yas-snippets.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/vscode.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/logo-animation.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/mpvi.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/atomic-chrome.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/apple-music.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/tree-sitter.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/copilot.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/xwidget.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/agent-shell-clipboard.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/meson.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/clangd.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/general-gc.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/custom-xwidget.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/jupyter-eval.org")
  (custom/spacemacs-load-user-custom-via-org
   "user-config/remote-tramp.org"))
