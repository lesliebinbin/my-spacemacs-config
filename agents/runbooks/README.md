# Runbooks

Runbooks are deterministic, agent-independent procedures. Each runbook should
state:

1. Scope and completion criteria.
2. Required tools and environmental assumptions.
3. Inputs that must be chosen before execution.
4. Ordered, non-destructive steps.
5. Expected artifacts and observable results.
6. Verification and focused troubleshooting.

Do not place automatic activation rules here; those belong in a skill.

## Available runbooks

- [`fork-patch-emacs-package.md`](fork-patch-emacs-package.md): fork, patch,
  compile-test with `eldev`, and wire unmaintained Emacs packages into
  Spacemacs via Quelpa recipes.
- [`scikit-build-cuda-extension.md`](scikit-build-cuda-extension.md): bootstrap
  a library-first Python package with CMake, CUDA, and separate pybind11
  training and inference modules.
