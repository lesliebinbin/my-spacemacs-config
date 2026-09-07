# Quelpa Recipes Reference in Spacemacs

This document describes how to configure and manage source packages in Spacemacs
using Quelpa recipes under `dotspacemacs-additional-packages`.

## Recipe Syntax in Spacemacs

In `.spacemacs.d/emacs-config/layers.el`:

```elisp
(defun dotspacemacs/layers ()
  (setq-default
   ;; ...
   dotspacemacs-additional-packages
   `(
     ;; Simple package name (from configured ELPA/MELPA archives):
     package-name

     ;; Source recipe:
     (package-name :location (recipe :fetcher github-ssh
                                     :repo "owner/repo"
                                     :commit "full-or-short-sha"
                                     :files ("*")))
     ;; ...
     )))
```

## Common Recipe Properties

| Property | Value | Description |
|---|---|---|
| `:fetcher` | `github-ssh`, `github`, `gitlab`, `git` | Fetcher backend. `github-ssh` uses `git@github.com:...` (recommended for private/authenticated pushes), while `github` uses HTTPS. |
| `:repo` | `"username/repository"` | GitHub or GitLab repository slug. |
| `:commit` | `"8647d78183..."` | Pinned commit SHA or tag. When set, Quelpa clones without depth limitation and checks out the exact ref. |
| `:branch` | `"main"` or `"master"` | Branch to follow if not using the repository's default branch. |
| `:files` | `("*")` or specific file list | Files to bundle into the built package. Defaults to `("*.el" "lisp/*.el" ...)`. For simple flat packages, `("*")` ensures docs and all `.el` files are included. |
| `:version` | string or function | Overrides version string generation if needed. |

## Pinned Commit vs Default Branch

### 1. Pinned commit (Testing phase)
Use `:commit` during patch development and initial verification to guarantee that Spacemacs builds the exact vetted commit:

```elisp
(origami :location (recipe :fetcher github-ssh
                           :repo "lesliebinbin/origami.el"
                           :commit "8647d781834aa4e6cb918e46ac26c1684651803b"
                           :files ("*")))
```

### 2. Default branch tracking (Normal operation)
Once verified across machines, remove `:commit` so Quelpa pulls new commits pushed to the fork's default branch:

```elisp
(origami :location (recipe :fetcher github-ssh
                           :repo "lesliebinbin/origami.el"
                           :files ("*")))
```

## Troubleshooting & Cache Invalidation

1. **Package not updating / Old version still loaded**:
   Spacemacs skips installing a package if `package-installed-p` returns non-nil. If an older version from ELPA exists:
   ```bash
   rm -rf ~/.emacs.d/elpa/<emacs-version>/develop/<package-name>-*
   ```
2. **Quelpa git cache dirty**:
   Quelpa maintains cached clones under `~/.emacs.d/.cache/quelpa/build/<package-name>/` or `~/.emacs.d/quelpa/build/<package-name>/`. If git state is corrupted or out of sync:
   ```bash
   rm -rf ~/.emacs.d/.cache/quelpa/build/<package-name>
   ```
3. **Checking active library path**:
   Verify which file Emacs actually loads:
   ```bash
   emacs --batch -l ~/.emacs.d/init.el --eval '(message "%s" (locate-library "<package-name>"))'
   ```
