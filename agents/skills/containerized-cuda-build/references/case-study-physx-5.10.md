# Case study: building NVIDIA PhysX 5.10 with GPU support

Worked example, 2026-09-10. Included because the diagnosis is the reusable part,
and because four of the five failures below were **not** what they appeared to
be at first glance.

## The situation

| | |
|---|---|
| Project | NVIDIA PhysX 5.10, GPU-accelerated |
| Target GPU | RTX 5060 (Blackwell, `sm_120`) |
| Host | Ubuntu 26.04, CUDA 13.3, GCC 15.2, driver 610.43.02 |
| Project requires | CUDA 12.8, GCC 13.3 |
| Constraints | Repo must stay exactly as cloned. Install to `~/local`. |

Two hard constraints made this a container problem rather than a config problem:
the project's build died under CUDA 13.3, and the repo could not be edited to
work around it.

## The diagnosis that mattered

"CUDA is too new" turned out to be **two independent blockers**:

1. The host's CUDA 13.3 is a major version past what PhysX 5.10 supports. CUDA
   13 removed `compute_70` and other older targets that the build requests.
2. Even with a matching CUDA 12.8 installed, the host's **GCC 15.2 is newer than
   CUDA 12.8 accepts**. A distinct axis, with a distinct fix.

The host driver (610.43.02) was never a problem, and was never touched. It is
newer than any CUDA 12.8 requirement, which is the normal and supported
direction.

The decisive observation: **the project's own `README_LINUX.md` named GCC 13.3.0
for Ubuntu 24.04.** That turned "find a compatible toolchain" into a lookup —
and `nvidia/cuda:12.8.2-devel-ubuntu24.04` ships exactly CUDA 12.8.93 with GCC
13.3.0. One image satisfied both blockers at once. That is the whole trick: the
tag's distro suffix is what pins the compiler.

## Routes that were tried and rejected

**Installing CUDA 12.8 on the host via the NVIDIA runfile.** Rejected, and the
reason generalizes: the runfile's `cuda-installer` needs `libxml2.so.2`, but
Ubuntu 26.04 ships only soname 16. A shim was extracted from an Ubuntu 24.04
deb and did work — but the route was abandoned because **it would not have fixed
blocker 2**. CUDA 12.8 on a GCC 15 host still fails. This is the archetypal
wrong turn the skill warns about.

**Patching the project source.** Ruled out by the pristine-repo constraint.

**Prebuilt binaries.** Checked, then ruled out — native PhysX 5.x ships
source-only, and the one prebuilt alternative (`ovphysx`) is a different C API.

## The build

```dockerfile
FROM nvidia/cuda:12.8.2-devel-ubuntu24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake python3 curl make git unzip file ca-certificates \
        libgl1-mesa-dev libglu1-mesa-dev libx11-dev libxext-dev \
        libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev \
        freeglut3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
```

```bash
docker build -t physx-build:cu128 .

# generate
docker run --rm --network=host --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -e PM_PACKAGES_ROOT=/packman -e PM_CUDA_PATH=/usr/local/cuda \
  -v "$BUILD/physx:/src" -v "$BUILD/packman-root:/packman" -w /src \
  physx-build:cu128 bash -c './generate_projects.sh linux-gcc'

# build — note the cache mount is repeated, which is the point of the trap below
docker run --rm --network=host --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -e PM_PACKAGES_ROOT=/packman \
  -v "$BUILD/physx:/src" -v "$BUILD/packman-root:/packman" \
  -w /src/compiler/linux-gcc-release physx-build:cu128 bash -c 'make -j12'
```

Result: 19 SDK libraries, 68 sample programs, a 347 MB `libPhysXGpu_64.so`
containing `sm_120` SASS for 61 objects, zero errors. Verified on the host with
9/9 checks passing against the real RTX 5060.

## Failure modes that actually occurred

These are the ones worth carrying forward. Only the first is PhysX-specific in
form; all five generalize.

### 1. The dependency cache must be mounted at the same path for generate *and* build

The generate step baked `-I/packman/chk/rapidjson/1.1.0-.../include` — an
absolute path — into the generated makefiles. The generate run mounted the cache
at `/packman`; the first build run did not. The build succeeded to **90%** and
then died on `rapidjson/document.h: No such file or directory`.

The late failure is what makes this trap expensive: nearly everything works, so
the mount is the last thing you suspect. Fix: mount the cache, at the identical
absolute path, with the matching environment variable, on **both** runs.

### 2. GUI/X11 development packages are configure-time dependencies

The build enables graphical samples, and `SnippetRender.cmake` calls
`FIND_PACKAGE(OpenGL)` then `FIND_PACKAGE(GLUT)`. CMake stops at the first, so
fixing OpenGL revealed the GLUT failure as a *second* full configure cycle.
Install the whole list up front.

### 3. The build wrote absolute paths, so a host configure must never be reused

The source copy excluded `compiler/linux-gcc-*` specifically to avoid inheriting
a build directory configured against the host's CUDA 13.3. Mixing toolchains in
one build directory is not a recoverable state.

### 4. A library loaded with `dlopen` does not appear in `ldd`

PhysX loads `libPhysXGpu_64.so` at runtime. The test binary linked perfectly and
would have failed at runtime with a null context manager had the library not been
findable. `readelf -d` on it showed `NEEDED` entries for only `libstdc++`,
`libm`, and `libc` — **no `libcudart`** — which also established that the CUDA
toolkit is a compile-time dependency only, and that end users need just a driver.

### 5. Source-level traps that masquerade as toolchain failures

Both of these initially looked like build problems and were nothing of the kind:

- **`PxFlags` converts to `bool` *and* to `PxU8`/`PxU16`/`PxU32`**, so
  `(flags & Enum) != 0` is an ambiguous-overload error. A boolean context is
  fine.
- **`PxSimulationStatistics::nbShapes` is an array** indexed by geometry type,
  not a count, so printing it as an integer prints a pointer. The same struct
  describes only the *last* step, so a settled pile reports all zeros and totals
  must be accumulated across steps.

Neither is a container or toolchain issue. Recognising that quickly is the point.

## What was verified, and how

A hand-written C++ consumer program — not the project's own samples — linked the
static libraries, created a CUDA context, and simulated a box falling onto a
plane:

- Device reported as `NVIDIA GeForce RTX 5060`, as expected.
- The box fell, came to rest at exactly `y = 0.5000` (its half-extent), with no
  lateral drift.
- 9/9 checks passed, run **on the host**, since the Docker daemon had no NVIDIA
  container runtime registered and therefore could not see the GPU at all.

That last point is the case study's most transferable lesson: **the container
built the code, and the host ran it.** Compilation never needed the GPU, and a
container runtime that could not expose the GPU was never a blocker.

## Outcome

- Full GPU-enabled PhysX 5.10 built from an unmodified source tree.
- Host system unchanged — no new packages, no toolkit changes.
- Installed to `~/local`: 69 headers, 10 libraries, plus runtime data.
- A CMake demo project at `~/codings/physx-demo` with `release`,
  `relwithdebinfo`, and `debug` presets, all building and passing from clean.

## What would have gone faster

- Running the toolchain smoke test (step 2 of the runbook) before anything else,
  rather than inferring image compatibility from documentation.
- Reading the project's supported-compiler table as a *lookup* immediately,
  instead of treating the CUDA version and the compiler version as one problem.
- Installing the GUI/X11 packages during the first image build.
- Checking the mount-path rule before the first long build, not after the first
  late failure.
