# Choosing an NVIDIA CUDA container image

## Tag anatomy

```text
nvidia/cuda:<cuda-version>-<flavor>-<distro>
             12.8.2        devel    ubuntu24.04
```

Each field is a separate decision, and only the last one is commonly overlooked.

## Field 1 — CUDA version

Pin it **fully**. `12.8.2` is a fixed image; `12.8` is a moving tag that gets
rebuilt as point releases land, so a build that works today can break later with
no change on your side. The project's own documentation usually names an exact
version — use that one.

## Field 2 — flavor

| Flavor | Contains | Use for |
|---|---|---|
| `base` | Essentially nothing CUDA; a base OS plus minimal metadata | Not useful for building. |
| `runtime` | CUDA **runtime** libraries only | Running prebuilt CUDA binaries. |
| `devel` | Full toolkit: `nvcc`, headers, static and shared libraries, `cuda-gdb` | **Building.** Always this one. |

Choosing `base` or `runtime` produces a confusing failure: `cmake` reports
`No CMAKE_CUDA_COMPILER could be found`, pointing at a compiler-availability
problem rather than at the image choice. The image is also several GB smaller
than expected, which is the tell.

## Field 3 — distro: this is how you pin the compiler

The distro determines the GCC that ships with it, and a CUDA toolkit only
accepts host compilers up to a version NVIDIA supports. So when a project says
"CUDA 12.8 with GCC 13", the distro is what satisfies the second half of that
requirement.

Rough shape of the relationship — always verify rather than trust this:

| Ubuntu | Ships GCC (approx.) |
|---|---|
| 20.04 | 9.x |
| 22.04 | 11.x |
| 24.04 | 13.x |

Older Ubuntu bases pair with older CUDA versions because that is what was
current at the time. In practice you rarely choose freely: the project's
supported-toolchain table picks the distro for you, and you take whichever
image tag carries both that distro and the CUDA version you need.

## Verified data points

Only rows actually checked on this machine. Do not extrapolate from a single
row — verify each new image with the recipe below.

| Image tag | CUDA | GCC | Verified |
|---|---|---|---|
| `nvidia/cuda:12.8.2-devel-ubuntu24.04` | 12.8.93 | 13.3.0 | yes |

## Verification recipe

Run this before starting any long build. It costs seconds and prevents an
hour-long mistake:

```bash
docker run --rm nvidia/cuda:12.8.2-devel-ubuntu24.04 \
  bash -c 'nvcc --version; echo ---; gcc --version | head -1; echo ---; g++ --version | head -1'
```

Read it against the project's requirements:

- `nvcc` missing entirely → wrong flavor. You took `base` or `runtime`.
- `nvcc` present but the compiler reports "unsupported GNU version" later at
  build time → the distro ships a GCC the toolkit rejects. **This fails at
  compile time, not at pull time**, which is why the smoke test matters.
- Both correct → proceed.

## Authoritative source

NVIDIA publishes a supported-compiler matrix per CUDA release in the CUDA
Installation Guide, and the container tags are listed on the NVIDIA Container
Registry. Check those rather than relying on the table above, which reflects one
machine's observations on one date.

## Keeping the host driver out of the picture

A container image bundles a toolkit, **not a driver**. The driver always comes
from the host. This is a feature: it means the container cannot fight your
driver, and a host driver that is newer than the toolkit is the normal,
supported configuration. Check the required minimum driver for your toolkit in
the CUDA release notes and confirm `nvidia-smi` clears it, then stop thinking
about the driver.
