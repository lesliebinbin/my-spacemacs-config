---
name: emacs-package-fork-patch
description: Use this skill when fixing bugs, deprecations, or compatibility issues in unmaintained or archived Emacs packages by maintaining a personal GitHub fork, validating builds with eldev, and integrating the fork into Spacemacs via Quelpa recipes.
compatibility: Requires git, GitHub SSH access, GNU Emacs 28+, and eldev.
allowed-tools: bash edit view create grep glob ask_user
---

# Emacs Package Fork & Patch

Use this skill when an Emacs package used by Spacemacs is broken or unmaintained
upstream (e.g., dead repository, unmerged PRs, or syntax incompatibilities with
new Emacs versions), and needs a maintainable fix at the source rather than
fragile runtime monkey-patching.

## Resources

- Runbook:
  [`../../runbooks/fork-patch-emacs-package.md`](../../runbooks/fork-patch-emacs-package.md)
- Reference:
  [`references/quelpa-recipes.md`](references/quelpa-recipes.md)

## Workflow

1. **Diagnose and reproduce**:
   - Isolate the error using `emacs --batch -Q` or `toggle-debug-on-error`.
   - Identify whether upstream is abandoned or unmaintained.
2. **Ensure tooling**:
   - Verify that `eldev` is installed (`which eldev`). If missing, bootstrap via
     `curl -fsSL https://raw.githubusercontent.com/doublep/eldev/master/webinstall/eldev | sh`.
3. **Fork and clone**:
   - Fork upstream to user's GitHub account and clone locally into `~/codings/<package-name>`.
   - Add upstream remote: `git remote add upstream <upstream-url>`.
4. **Patch and validate with eldev**:
   - Apply surgical fixes (e.g. replace dynamic load-time face attributes with static specs,
     migrate `cl` -> `cl-lib`, unquote `:inherit`).
   - Byte-compile using `eldev --external=~/.emacs.d/elpa/<emacs-version>/develop compile`.
   - Clean compilation artifacts (`rm -rf .eldev *.elc`).
5. **Commit and push**:
   - Commit cleanly with detailed explanation.
   - Push to user's GitHub fork (`git push origin master`).
   - Record commit SHA via `git rev-parse HEAD`.
6. **Integrate into Spacemacs**:
   - Add recipe in `.spacemacs.d/emacs-config/layers.el` under `dotspacemacs-additional-packages`:
     ```elisp
     (<pkg> :location (recipe :fetcher github-ssh :repo "owner/<pkg>" :commit "<sha>" :files ("*")))
     ```
   - Delete any temporary runtime workaround from `emacs-config/user-config.el`.
7. **Reinstall and test**:
   - Purge old package directory from `~/.emacs.d/elpa/<emacs-version>/develop/<pkg>-*`.
   - Trigger Quelpa install and verify library path with `locate-library`.
8. **User validation & finalize**:
   - Ask user to confirm GUI and terminal behavior.
   - Upon confirmation, optionally remove `:commit` from recipe to follow default branch.

## Required behavior

- Do not use runtime `custom-set-faces` or function advise as a permanent solution when a source fork is appropriate.
- Always verify compilation with `eldev` before pushing to the fork.
- Always pin `:commit` initially when integrating into `layers.el` to ensure reproducible testing before switching to branch tracking.
- When switching an existing ELPA package to a Quelpa recipe, always clean the old directory in `elpa/<version>/develop/<pkg>-*` so Spacemacs forces an installation.

## Exit criteria

The fork and patch is complete when:
- The fork compiles cleanly under `eldev` without errors or obsolete warnings.
- The package is fetched and installed from the fork into Spacemacs ELPA via Quelpa.
- Any temporary runtime workaround is removed.
- The failure cannot be reproduced in GUI and terminal Emacs sessions.
