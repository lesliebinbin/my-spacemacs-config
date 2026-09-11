---
name: containerized-cuda-build
description: Use this skill when a C++/CUDA project will not configure or compile because the host's CUDA toolkit, compiler, or distro does not match what the project requires. It diagnoses which axis is broken, then builds inside a pinned NVIDIA container image so neither the host nor the project source is modified, and verifies the result on the host GPU.
compatibility: Requires Docker with a reachable daemon, enough disk for a 5-8 GB CUDA devel image plus the build tree, and network access for the pull. A GPU is not needed to build; it is needed only to verify.
allowed-tools: bash view create edit grep glob rg ask_user
---

# Containerized CUDA Build

Use this skill when a build fails on the host for **toolchain-vintage** reasons —
the usual phrasing is "my CUDA is too new", "nvcc rejects this flag", "the
compiler is unsupported", or "this only builds on Ubuntu 22.04".

This skill applies when **all** of the following hold:

- The project must build from source; no prebuilt binaries exist for it.
- The failure is a configure/compile/link failure caused by the toolchain, not a
  bug in the project's source.
- The host toolchain cannot easily be changed — because modifying the host is
  undesirable, because the repo must stay pristine, or because other work
  depends on the current host toolchain.

The payoff is that a container pins the toolkit, the host compiler, and the
distro simultaneously, and is disposable. The host and the source tree are both
left exactly as they were.

## Resources

- Runbook:
  [`../../runbooks/nvidia-container-cuda-build.md`](../../runbooks/nvidia-container-cuda-build.md)
- Image and compiler selection:
  [`references/image-selection.md`](references/image-selection.md)
- Worked case study, with the failure modes that actually occurred:
  [`references/case-study-physx-5.10.md`](references/case-study-physx-5.10.md)

## Workflow

1. **Diagnose the axis before proposing anything.** Read the *first* error from
   the host build and classify it: is `nvcc` rejecting a flag (toolkit), is the
   compiler unsupported (host compiler), or is it a link/ABI failure (CPU libs)?
   These have different fixes, and "CUDA is too new" conflates them.
2. **Confirm the driver is not the problem, and say so explicitly.** Drivers are
   backward compatible — a new driver running old CUDA is the supported case.
   Check `nvidia-smi` against the toolkit's minimum driver and rule it out.
   Do not let a driver downgrade enter the plan.
3. **Check whether the project has a documented supported-toolchain table.** A
   README that names a distro and compiler version turns step 1 from
   guesswork into a lookup. Read it before choosing an image.
4. **Ask before discarding alternatives** — particularly before installing a
   second CUDA toolkit or a second GCC onto the host, and before touching the
   project source. Present the container as the recommended option and state
   the trade-off (it adds a Docker dependency to the build).
5. **Choose the image by distro, then verify the toolchain inside it** with the
   one-line smoke test in the runbook, *before* starting any long build.
6. **Build in a container; verify on the host.** Compilation is CPU-only work
   and needs no GPU. Treat a missing NVIDIA container runtime as irrelevant
   rather than as a blocker.
7. **Write a real consumer program and run it** on the host. A successful
   compile is not evidence of correct linkage against a driver-loaded backend.
8. **Report what was pinned, where the artifacts landed, and how to reproduce
   the build in one command.**

## Required behavior

- **Do not modify the project source to work around a toolchain problem.** If
  the project must stay pristine, that constraint binds here first.
- **Do not modify the host as the first move.** Installing a second CUDA toolkit
  or a second GCC is invasive, frequently incomplete, and is the most common
  wrong turn in this workflow. If the host was going to be changed anyway, the
  container would be unnecessary.
- **Do not treat "install the matching CUDA toolkit" as a fix by itself.** The
  host compiler is a second, independent axis; a project needing CUDA 12.8 and
  GCC 13.3 still fails on a host with GCC 15 after you add CUDA 12.8.
- **Do not conclude the container needs GPU access to build.** It does not.
- **Do not pipe a command through `tail`/`head` and read the exit status.** The
  pipeline reports the pager's status, so a failed command looks successful.
- **Do not conclude a tool cannot do something from a command that never ran.**
  A glob or quoting error in the shell means the output you are reasoning about
  is absent, not empty. Check that the command actually executed.
- **Verify claims about the produced artifacts**, not just that the build
  finished: confirm the expected libraries and headers exist, and check what the
  binaries actually need at runtime with `readelf -d ... | grep NEEDED`.
- **State plainly when a verification step was skipped or was inconclusive.**

## Exit criteria

The skill is complete when all of the following hold:

- The project builds from a clean state using a single documented command that
  names a fully-pinned image.
- The host system is byte-for-byte unchanged — no new system packages, no
  modified toolchain.
- The project source is unchanged, or the changes are limited to files the user
  approved.
- Artifacts are extracted to the agreed install location and are owned by the
  invoking user, not root.
- A consumer program has been compiled **and executed successfully on the
  host GPU**, with its output shown.
- The remaining caveats are stated: what the build depends on, and anything that
  was not verified.
