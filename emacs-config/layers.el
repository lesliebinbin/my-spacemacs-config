;; -*- mode: emacs-lisp; lexical-binding: t -*-

(defun dotspacemacs/layers ()
  "Layer configuration:
This function should only modify configuration layer settings."
  (setq-default
   ;; Base distribution to use. This is a layer contained in the directory
   ;; `+distribution'. For now available distributions are `spacemacs-base'
   ;; or `spacemacs'. (default 'spacemacs) dotspacemacs-distribution 'spacemacs
   ;; Lazy installation of layers (i.e. layers are installed only when a file
   ;; with a supported type is opened). Possible values are `all', `unused'
   ;; and `nil'. `unused' will lazy install only unused layers (i.e. layers
   ;; not listed in variable `dotspacemacs-configuration-layers'), `all' will
   ;; lazy install any layer that support lazy installation even the layers
   ;; listed in `dotspacemacs-configuration-layers'. `nil' disable the lazy
   ;; installation feature and you have to explicitly list a layer in the
   ;; variable `dotspacemacs-configuration-layers' to install it.
   ;; (default 'unused) dotspacemacs-enable-lazy-installation 'unused
   ;; If non-nil then Spacemacs will ask for confirmation before installing
   ;; a layer lazily. (default t) dotspacemacs-ask-for-lazy-installation t
   ;; List of additional paths where to look for configuration layers.
   ;; Paths must have a trailing slash (i.e. "~/.mycontribs/") dotspacemacs-configuration-layer-path '()
   ;; List of configuration layers to load.

   dotspacemacs-configuration-layers

   `(
     ;; ----------------------------------------------------------------
     ;; Example of useful layers you may want to use right away.
     ;; Uncomment some layer names and press `SPC f e R' (Vim style) or
     ;; `M-m f e R' (Emacs style) to install them.
     ;; ----------------------------------------------------------------
     (auto-completion
      :variables
      auto-completion-enable-help-tooltip 'manual
      auto-completion-return-key-behavior 'complete
      auto-completion-tab-key-behavior 'cycle
      auto-completion-enable-snippets-in-popup t
      auto-completion-use-company-box nil
      :disabled-for git)
     docker
     cmake
     sql
     protobuf
     emacs-lisp
     dap
     json
     yaml
     toml
     command-log
     restclient
     llm-client
     git
     helm
     markdown
     multiple-cursors
     (org
      :variables
      org-enable-babel-support t
      org-confirm-babel-evaluate nil
      org-src-preserve-indentation t
      org-edit-src-content-indentation 0
      org-src-fontify-natively t)

     (shell
      :variables
      shell-default-height 30
      shell-default-position 'bottom
      shell-default-shell 'vterm)
     syntax-checking
     version-control
     semantic
     (better-defaults
      :variables
      better-defaults-move-to-beginning-of-code-first t
      better-defaults-move-to-end-of-code-first
      nil)
     (treemacs
      :variables
      treemacs-use-all-the-icons-theme t
      treemacs-width 30)


     csv
     toml
     (python
      :variables
      python-backend 'lsp
      python-lsp-server 'pyright
      python-formatter 'black)
     javascript
     typescript
     vue
     prettier
     c-c++
     meson
     clojure
     ,@(unless (custom/apple-intel-p)
         '(parinfer))
     (github-copilot
      :variables
      copilot-chat-backend 'curl)
     ,@(when (eq system-type 'darwin)
         '(
           (osx :variables
                osx-command-as 'super
                osx-option-as 'meta
                osx-control-as 'control
                osx-function-as nil
                osx-right-command-as 'left
                osx-right-option-as 'left
                osx-right-control-as 'left
                osx-swap-option-and-command nil)))

     ,@(unless (or t (custom/apple-intel-p))
         '(
           (eaf :variables
                eaf-python-command (getenv "EAF_PYTHON_PATH")
                eaf-enable-debug (custom/wsl-p))))

     ,@(when-let* ((conda-home (getenv "CONDA_PREFIX")))
         `(
           (conda
            :variables
            conda-anaconda-home ,conda-home
            :config
            (conda-env-initialize-interactive-shells)
            (conda-env-initialize-eshell))))



     (ibuffer :variables ibuffer-group-buffers-by 'projects)
     tabs
     kubernetes
     quickurl
     pandoc
     imenu-list
     nginx
     agent-shell)











   ;; List of additional packages that will be installed without being wrapped
   ;; in a layer (generally the packages are installed only and should still be
   ;; loaded using load/require/use-package in the user-config section below in
   ;; this file). If you need some configuration for these packages, then
   ;; consider creating a layer. You can also put the configuration in
   ;; `dotspacemacs/user-config'. To use a local version of a package, use the
   ;; `:location' property: '(your-package :location "~/path/to/your-package/")
   ;; Also include the dependencies as they will not be resolved automatically.
   dotspacemacs-additional-packages
   `(
     (jupyter-eval :location (recipe :fetcher github-ssh :repo "lesliebinbin/emacs-jupyter-eval" :files ("*")))
     (origami :location (recipe :fetcher github-ssh :repo "lesliebinbin/origami.el" :files ("*")))
     simple-httpd
     websocket
     exec-path-from-shell
     clipetty
     gcmh
     mpvi
     format-all
     ;; Check if function exists AND if it returns t
     ,@(when (and (fboundp 'treesit-available-p)
                  (treesit-available-p) )
         '(treesit-auto
           (treesit-fold :location (recipe :fetcher github :repo "emacs-tree-sitter/treesit-fold"))))

     timu-macos-theme
     (atomic-chrome :location (recipe :fetcher github :repo "KarimAziev/atomic-chrome"))
     (applescript-mode :location (recipe :fetcher github :repo "lesliebinbin/applescript-mode"))
     (buffer-path-utils :location ,(expand-file-name "buffer-path-utils" user-packages-directory))
     (compile-multi :location (recipe :fetcher github :repo "mohkale/compile-multi"))
     (dwim-shell-command :location (recipe :fetcher github :repo "xenodium/dwim-shell-command")))





   ;; A list of packages that cannot be updated.
   dotspacemacs-frozen-packages
   '()
   ;; A list of packages that will not be installed and loaded.
   dotspacemacs-excluded-packages
   '(
     company)

   ;; Defines the behaviour of Spacemacs when installing packages.
   ;; Possible values are `used-only', `used-but-keep-unused' and `all'.
   ;; `used-only' installs only explicitly used packages and deletes any unused
   ;; packages as well as their unused dependencies. `used-but-keep-unused'
   ;; installs only the used packages but won't delete unused ones. `all'
   ;; installs *all* packages supported by Spacemacs and never uninstalls them.
   ;; (default is `used-only')
   dotspacemacs-install-packages
   'used-only))
