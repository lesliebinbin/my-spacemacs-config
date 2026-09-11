# Build a CUDA project inside an NVIDIA container image

## Outcome

A project that will not configure or compile on the host — because the host's
CUDA toolkit or C++ compiler is the wrong vintage for it — builds reproducibly
again, **without modifying the project source and without changing anything
about the host system**.

The container supplies a pinned CUDA toolkit, a pinned host compiler, and a
pinned distro in one shot. Build inside it; verify on the host, which is the
only place with a working GPU driver.

## Inputs

Establish these before starting. Do not guess them — every one is verifiable in
a minute.

| Input | How to get it | Example |
|---|---|---|
| Project source dir | User or repo layout | `/mnt/data/build/myproject` |
| Required CUDA version | Project README / release notes | `12.8` |
| Required host compiler | Project README, or the CUDA release notes' supported-compiler table | `GCC 13.3` |
| Host CUDA version | `nvcc --version` | `13.3` (too new) |
| Host compiler version | `g++ --version` | `15.2` (too new) |
| Host driver version | `nvidia-smi` | `610.43.02` |
| Build output dir | Project's build system | `<proj>/bin/linux.x86_64/release` |
| Dependency cache dir | Only if the build fetches packages | `<work>/packman-root` |

## Prerequisites

- Docker is installed and the daemon is reachable.
- Enough disk: CUDA `devel` images are 5–8 GB, and a full build tree can add
  several GB more. Check with `df -h` before pulling.
- Network access for the pull. If the host reaches the internet through a
  proxy, VPN, or TUN device, see the `--network=host` note in step 4.
- **A GPU is not required for any build step, and Docker does not need GPU
  access to compile CUDA code.** `nvcc` is a CPU program. GPU access is only
  needed to *run* the result. Do not let a missing NVIDIA container runtime
  block the build — see step 7.

## Step 0. Establish which axis is actually broken

This is the step that saves the most time. "CUDA is too new" is usually two or
three independent problems wearing one coat, and they have different fixes.

| Axis | What it decides | Fixable on the host? |
|---|---|---|
| **Driver** | What compiled binaries can **run** | No — and usually you don't need to. Drivers are backward compatible: a new driver runs old CUDA binaries. |
| **Toolkit** (`nvcc`) | What can **compile** | Only by installing a second toolkit — invasive. |
| **Host compiler** (`g++`) | Whether the toolkit **accepts** the host | Only by installing a second GCC — invasive, and easy to get wrong. |
| **CPU libs / headers** | ABI compatibility | Depends on the project. |

Three consequences worth internalising:

1. **Never "fix" this by downgrading the driver.** A new driver running old CUDA
   is the normal, supported case. Verify it's new enough and move on.
2. **Installing a matching CUDA toolkit on the host often does not fix it**,
   because the *second* axis — the host compiler — is still too new. A project
   needing CUDA 12.8 and GCC 13.3 will not build on a host with GCC 15 just
   because you added CUDA 12.8. **This is the most common wrong turn.**
3. **The container fixes every axis at once**, because the image pins the
   toolkit *and* the distro *and* therefore the compiler.

Confirm the driver is not the problem before continuing:

```bash
nvidia-smi                      # driver version, top-right
nvcc --version                  # toolkit the host would compile with
```

A CUDA 12.x toolkit needs a driver of roughly 525 or newer; CUDA 13.x needs
roughly 580 or newer. Check the driver table in the CUDA release notes for the
exact floor rather than trusting these numbers. A driver far ahead of the
toolkit is good, not a problem.

Then read the project's **first** configure or compile error and classify it.
Getting this wrong sends you down the wrong path for an hour.

## Step 1. Choose the image by its *distro*, not its CUDA number

The NVIDIA image tag is:

```text
nvidia/cuda:<cuda-version>-<flavor>-<distro>
```

- **`cuda-version`** — the toolkit you need. Pin it fully (`12.8.2`), not
  partially (`12.8`), so the tag cannot move under you.
- **`flavor`** — must be **`devel`**. `base` and `runtime` do **not** contain
  `nvcc`, and the build will fail confusingly without it.
- **`distro`** — this is the lever that picks your compiler. `ubuntu24.04`
  ships GCC 13.x; a newer Ubuntu ships a newer GCC. Choosing the distro is how
  you pin the host compiler, and it is the part people miss.

So the selection procedure is: find the distro whose compiler matches what the
project supports, then take that distro's tag for the toolkit version you need.

The distro choice is the whole trick. If the project declares a supported
compiler for a specific distro, use that distro.

## Step 2. Smoke-test the toolchain before touching the project

Ten seconds here de-risks everything downstream. Never start a two-hour build
on an unverified toolchain.

```bash
docker run --rm nvidia/cuda:12.8.2-devel-ubuntu24.04 \
  bash -c 'nvcc --version; gcc --version | head -1'
```

Compare against the versions the project requires. If they match, the rest of
this runbook is mechanical. If they do not, go back to step 1 — do not proceed
and hope.

## Step 3. Bake an image with the project's build dependencies

Write a `Dockerfile` next to the build directory — not inside the source tree,
which must stay clean:

```dockerfile
FROM nvidia/cuda:12.8.2-devel-ubuntu24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake python3 curl make git unzip file ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
```

```bash
docker build -t myproject-build:cu128 .
```

Add the project's own build dependencies to the `apt-get` list. **Include GUI
and X11 development packages up front if the build has any graphical samples or
tools.** CMake's `FIND_PACKAGE(OpenGL)` / `FIND_PACKAGE(GLUT)` run during
*configure*, so a missing one costs you a full configure cycle per package —
and CMake reports them one at a time, so you discover the second only after
fixing the first:

```dockerfile
        libgl1-mesa-dev libglu1-mesa-dev libx11-dev libxext-dev \
        libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev freeglut3-dev
```

Check whether the build declares such samples before you find out the slow way.

## Step 4. Run the build with an explicit mount and user contract

The flags below are not decoration; each fixes a specific failure.

```bash
docker run --rm \
  --network=host \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -v /absolute/host/path/to/source:/src \
  -v /absolute/host/path/to/cache:/cache \
  -w /src \
  myproject-build:cu128 \
  bash -c '<build command>'
```

| Flag | Why |
|---|---|
| `--user "$(id -u):$(id -g)"` | Without it the build writes **root-owned** files into your bind mount and you cannot delete them without `sudo`. |
| `-e HOME=/tmp` | Containers run as your uid have no writable home; tools that cache into `$HOME` fail. |
| `--network=host` | Needed when dependency fetchers must reach the internet through a host proxy, VPN, or TUN device. Docker's default bridge bypasses it. |
| `-v ...:/absolute/path` | See the mount-path rule below. |

### The mount-path rule

**If a build system writes absolute dependency paths into generated files, the
dependency cache must be mounted at the same absolute path for *both* the
generate step and the *build* step.**

This is the single nastiest trap in this workflow, because it fails *late*. The
generate step succeeds, most of the build succeeds, and then it dies near the
end with a header not found — at which point you suspect the project, not the
mount.

The mechanism: a dependency manager (Conan, vcpkg, packman, a CMake
`file(GENERATE)`, anything that emits `-I/abs/path/...`) resolves its cache to
an absolute path at configure time and bakes it into the generated makefiles.
Mount the cache somewhere else on the build run and every baked path dangles.

Concretely: pick one mount point, use it for both runs, and pass the same
environment variable pointing at it each time. If you mounted the cache while
generating, **you must mount it while building too** — with both the mount and
the environment variable, not just one.

## Step 5. Configure, then build

Run the project's own generation and build commands. Nothing about them changes
— the whole point of this runbook is that the project is built exactly as its
authors intended, just on a pinned toolchain.

Do these as separate `docker run` invocations so a configure failure and a build
failure are distinguishable, and so the build step carries the cache mount from
step 4.

Watch for configuration caches. A build directory produced on the **host** by a
different toolkit must not be reused. Delete it, or work in a copy of the source
tree so the pristine checkout stays pristine.

## Step 6. Extract the artifacts

The build wrote into the bind-mounted directory, so the output is **already on
the host**, owned by your uid. Copy it out normally:

```bash
cp -a <source>/install/<preset>/<name>/. ~/local/
```

Do not use `docker cp`; the whole point of the bind mount is that it is
unnecessary.

Then confirm the copy is complete — headers, shared libraries, static
libraries, and any runtime data directories:

```bash
find ~/local -maxdepth 3 -name '*.h' | wc -l
ls -la ~/local/bin/linux.x86_64/release/
```

## Step 7. Verify on the host, not in the container

**Compilation never needs a GPU; execution always does.** Keep those separate.
Unless the NVIDIA Container Toolkit is registered with the Docker daemon,
`docker run --gpus all` fails even when `nvidia-smi` works fine on the host. Do
not burn time fighting it — build in the container, test on the host, and make
that split explicit in the plan.

Write a real consumer program and run it on the host. "It compiled" is not
evidence that it works; linkage against a driver-loaded backend can only be
proven by running.

Two runtime facts that bite here:

- **A GPU backend loaded via `dlopen` does not appear in `ldd` output.** The
  binary can link cleanly and still fail at runtime with a null context. Either
  place the shared object beside the binary or put its directory on the
  binaries' `rpath`. An `rpath` (`BUILD_RPATH`/`INSTALL_RPATH` in CMake) is
  preferable to requiring `LD_LIBRARY_PATH` — verify by running under
  `env -u LD_LIBRARY_PATH`.
- **A toolkit is a compile-time dependency only** if the shipped library's
  `NEEDED` entries show no CUDA runtime. Check with `readelf -d <lib> | grep
  NEEDED` before assuming the CUDA toolkit must ship to end users.

## Troubleshooting

### The build fails near the end with a header not found

Almost certainly the mount-path trap in step 4. Check whether the compilation
command contains an `-I` path that does not exist inside the build container.
For `make`-based builds, re-run a single failing target with `make VERBOSE=1` to
see the actual command line.

### CMake reports `Could NOT find OpenGL`, then `Could NOT find GLUT`

Both are configure-time. Install the full GUI/X11 list from step 3 in one go
and reconfigure; fixing them one at a time doubles the cycle count.

### `docker run --gpus all` fails

Expected if the NVIDIA Container Toolkit is not registered with the daemon.
Irrelevant for building. Move verification to the host.

### The pull stalls or dies with `connection reset by peer`

Common behind a proxy, VPN, or TUN device. Re-run the pull — layers already
fetched are cached and it resumes. **Do not diagnose progress by piping the
pull through `tail` or `head`:** the pipeline's exit status is the pager's, not
`docker`'s, and the output is buffered until exit, so a progressing pull looks
frozen and a finished one looks successful even if it was not. Check the real
exit code, and use `ss -tin` byte counters if you need to watch throughput.

### The build dies with an ambiguous overload or a struct field that reads as garbage

This is a **source-level** problem, not a toolchain one, and it is not what this
runbook is for. A C++ flag-like type that converts both to `bool` and to integer
types makes `(flags & Enum) != 0` ambiguous — test it in a boolean context.
An array field printed as an integer prints a pointer. Fix the calling code;
do not start editing the project to work around it.

### Everything succeeded but a check fails only under a pipe

`cmd | tail` and `cmd | head` report the **pager's** exit code. A green pipeline
is not evidence the command succeeded. Capture `$?` directly, or use
`${PIPESTATUS[0]}`.

## Recovery

- **Wrong image chosen** (missing `nvcc`, wrong compiler): the pull is cached;
  rebuilding the image on a different base costs only the `apt-get` layer.
- **Build directory poisoned by a host configure**: delete it and re-configure
  inside the container. Never mix toolchains in one build directory.
- **Root-owned files in the mount** from a run without `--user`: fix with
  `sudo chown -R "$(id -u):$(id -g)" <dir>`.
- **Disk exhausted mid-pull or mid-build**: `docker system prune` removes
  dangling images; the project source and the build cache are untouched.
- **Host system accidentally modified**: revert it. The premise of this runbook
  is that the host should never need changing; if it was changed, that is a bug
  in the approach, not an accepted cost.
