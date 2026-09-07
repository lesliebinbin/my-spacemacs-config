# Fork and patch an unmaintained Emacs package

## Outcome

Fix compatibility bugs or deprecations in an unmaintained Emacs package by:

- Reproducing the failure cleanly in Emacs batch mode.
- Maintaining an owned fork on GitHub and cloning it locally into `~/codings/<package-name>`.
- Using `eldev` to validate and compile the package without warnings/errors.
- Pointing Spacemacs `dotspacemacs-additional-packages` to the fork via a Quelpa recipe pinned to a specific `:commit` (and later transitioning to `:branch` or default branch after verification).
- Removing temporary runtime workarounds and verifying full functionality.

## Inputs

Choose these values before starting:

| Input | Example | Description |
|---|---|---|
| Package name | `origami` | The Elisp package symbol |
| Upstream repository | `https://github.com/gregsexton/origami.el` | The original canonical upstream URL |
| Fork repository | `lesliebinbin/origami.el` | User-owned GitHub repository name |
| Local workspace | `/home/lesliebinbin/codings/origami.el` | Local git clone destination |
| Git protocol | `github-ssh` or `github` | Quelpa fetcher protocol |
| Commit SHA | `8647d78183...` | The initial pinned commit SHA for testing |

## Prerequisites

- `git` and SSH access configured for GitHub (e.g. `ssh -T git@github.com`).
- `gh` CLI logged in, or create the fork via GitHub web UI.
- Emacs installed (e.g. GNU Emacs 29+ or 31+).
- `eldev` Elisp build tool installed (`~/.local/bin/eldev`).

To verify or install `eldev`:

```bash
which eldev || curl -fsSL https://raw.githubusercontent.com/doublep/eldev/master/webinstall/eldev | sh
eldev --version
```

## 1. Reproduce and diagnose the issue

Before making changes, isolate the failure in batch mode with `--batch -Q` or via `toggle-debug-on-error`.

Examples of common deprecations in modern Emacs (e.g. Emacs 31):
- Dynamic face attributes evaluated at load time: `defface` using `(face-attribute 'highlight :background)` bakes `unspecified` into `:box` specs, causing posframe child frames to error with `"Invalid face box"`.
- Deprecated libraries: `(require 'cl)` must be replaced with `(require 'cl-lib)` and functions updated (e.g. `cl-destructuring-bind`, `cl-remove-if`).
- Quoted face inheritance: `:inherit 'face` should be `:inherit face`.
- Obsolete aliases: `define-global-minor-mode` -> `define-globalized-minor-mode`.

Verify minimal batch reproduction:

```bash
emacs --batch -Q --eval '(defface test-face (quote ((t (:box (:line-width 1 :color unspecified))))) "doc")'
```

## 2. Fork and clone the repository

1. Fork the upstream repository to your GitHub account (via GitHub web UI or `gh repo fork <upstream-url>`).
2. Clone the fork locally into your coding workspace:

```bash
git clone git@github.com:<your-user>/<package-repo>.git ~/codings/<package-name>
cd ~/codings/<package-name>
git remote add upstream <upstream-url>
```

## 3. Apply surgical patches

1. Fix the root cause (e.g., replace load-time backquoted face definitions with static light/dark/fallback specs):

```elisp
(defface <package>-face
  '((((class color) (min-colors 88) (background light))
     (:background "#cccccc" :box (:line-width 1 :color "#999999")))
    (((class color) (min-colors 88) (background dark))
     (:background "#333333" :box (:line-width 1 :color "#555555")))
    (t (:background "grey" :box (:line-width 1 :color "grey"))))
  "Face documentation.")
```

2. Fix latent deprecations (`cl` -> `cl-lib`, unquote `:inherit`).
3. Ensure required helper libraries (e.g. `s`, `dash`) are explicitly required in files calling their functions.

## 4. Compile and validate with `eldev`

Run `eldev` pointing to your existing Spacemacs ELPA dependencies:

```bash
cd ~/codings/<package-name>
eldev --external=~/.emacs.d/elpa/<emacs-version>/develop compile
```

Ensure compilation exits with code 0 and no fatal warnings or obsolete syntax. Clean temporary test artifacts:

```bash
rm -rf .eldev *.elc
```

## 5. Commit and push to your fork

1. Stage only the modified source files.
2. Commit with a clear explanation of each fix.
3. Push to `origin`:

```bash
git add <files>
git commit -m "Fix <package> face spec and migrate cl to cl-lib"
git push origin master
```

4. Note the resulting commit SHA:

```bash
git rev-parse HEAD
```

## 6. Configure Spacemacs via Quelpa recipe

1. Edit `.spacemacs.d/emacs-config/layers.el`.
2. Under `dotspacemacs-additional-packages`, add a recipe specifying your fork and pinning the commit initially:

```elisp
   dotspacemacs-additional-packages
   `(
     ;; ...
     (<package-name> :location (recipe :fetcher github-ssh
                                      :repo "<your-user>/<package-repo>"
                                      :commit "<commit-sha>"
                                      :files ("*")))
     ;; ...
     )
```

3. Remove any temporary runtime monkey-patches or `custom-set-faces` workarounds from `.spacemacs.d/emacs-config/user-config.el`.

## 7. Force reinstall and test

Because Spacemacs won't reinstall an already installed package if a directory with that package name exists in `elpa/`:

1. Remove the old package from ELPA:

```bash
rm -rf ~/.emacs.d/elpa/<emacs-version>/develop/<package-name>-*
```

2. Trigger package installation and check location:

```bash
emacs --batch -l ~/.emacs.d/init.el --eval '(message "Package location: %s" (locate-library "<package-name>"))'
```

3. Verify that the loaded library points to the new Quelpa timestamped directory in `~/.emacs.d/elpa/<emacs-version>/develop/<package-name>-<timestamp>/`.

## 8. Validate interactively and transition to branch tracking

1. Open GUI Emacs, enable debug on error (`M-x toggle-debug-on-error`), and exercise the feature that previously crashed.
2. Test in terminal Emacs (`emacs -nw`).
3. Once satisfied, edit `emacs-config/layers.el` to omit `:commit` if you want it to track the default branch going forward:

```elisp
     (<package-name> :location (recipe :fetcher github-ssh :repo "<your-user>/<package-repo>" :files ("*")))
```
