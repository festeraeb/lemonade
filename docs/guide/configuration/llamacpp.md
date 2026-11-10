# llama.cpp Backend Options

Lemonade uses [llama.cpp](https://github.com/ggerganov/llama.cpp) as its primary LLM inference backend, supporting multiple hardware acceleration options. This document explains the available backends and how to choose between them.

## Available Backends

### CPU
> **macOS:** Lemonade uses the Metal-enabled llama.cpp build as the unified
> runtime. The same build can run CPU-only when GPU offload is disabled, so a
> separate `llamacpp:cpu` backend is not required on macOS.

- **Platform**: Windows, Linux
- **Hardware**: x86_64 processors (Windows and Linux); ARM64/aarch64 processors (Linux)
- **Use Case**: CPU-only execution and universal fallback on Windows and Linux
- **Performance**: Slowest option, suitable for small models or testing
- **Installation**: Automatically available via upstream llama.cpp releases

### Vulkan
- **Platform**: Windows, Linux
- **Hardware**: AMD GPUs (iGPU and dGPU), NVIDIA GPUs, Intel GPUs, Qualcomm Adreno and other Vulkan-capable GPUs on ARM64 Linux
- **Use Case**: Cross-vendor GPU acceleration
- **Performance**: Good performance across all GPU vendors
- **Installation**: Automatically available via upstream llama.cpp releases
- **Notes**: Recommended for most GPU users; on ARM64 Linux (e.g., Qualcomm X Elite), Vulkan is the default backend

### ROCm
- **Platform**: Windows, Linux
- **Hardware**: AMD Radeon RX 6000/7000 series (RDNA2/RDNA3/RDNA4), AMD Ryzen AI iGPUs (Strix Point/Halo)
- **Use Case**: AMD GPU-optimized inference
- **Performance**: Optimized for AMD hardware, may outperform Vulkan on supported GPUs
- **Channel Options**:
  - **Stable** (default): Custom builds with latest optimizations from lemonade-sdk
  - **Nightly**: Bleeding-edge builds from lemonade-sdk/llamacpp-rocm (experimental)
- **Installation**: Varies by channel (see below)

### CUDA
- **Platform**: Windows, Linux
- **Hardware**: NVIDIA GPUs with Compute Capability 7.5+ (Turing, Ampere, Ada, Hopper, Blackwell)
- **Use Case**: NVIDIA GPU-optimized inference
- **Performance**: Optimized for NVIDIA hardware, typically outperforms Vulkan on supported GPUs
- **Source**: Per-architecture builds from [lemonade-sdk/llama.cpp](https://github.com/lemonade-sdk/llama.cpp)
- **Binaries**: Compute-capability-specific builds (sm_75, sm_80, sm_86, sm_89, sm_90, sm_100, sm_120)
- **Runtime**: Bundled CUDA runtime libraries (no system-wide CUDA toolkit installation required)
- **Driver**: The NVIDIA driver must be installed. The binaries are built with CUDA 12.9. Lemonade checks only for the CUDA 12 driver floor (528.33+ on Windows, 525.60.13+ on Linux), relying on CUDA minor-version compatibility, which has limitations (for example, no PTX JIT on older drivers). A driver at least as new as the CUDA 12.9 toolkit's (576.02+ on Windows, 575.51.03+ on Linux) is recommended. If no NVIDIA driver is detected, the CUDA backend is reported as unsupported. If the driver is older than that, the backend is reported as `action_required` with a link to NVIDIA's driver downloads. Either way, `auto` backend selection falls through to Vulkan.
- **Notes**: On Windows, .7z extraction requires the bsdtar bundled with Windows 11 22H2+. On Linux, the build is shipped as .tar.xz and extracts with the system `tar`.

### Metal
- **Platform**: macOS only
- **Hardware**: Apple Silicon (M1/M2/M3/M4) and Intel Macs with Metal support
- **Use Case**: Accelerated llama.cpp inference on macOS
- **Performance**: Optimized for Apple Silicon
- **Installation**: Automatically available via upstream llama.cpp releases
- **Notes**: The Metal-enabled build can also run CPU-only when GPU offload is disabled

### System
- **Platform**: Linux only
- **Hardware**: Depends on system-installed llama-server binary
- **Use Case**: Advanced users with custom llama.cpp builds
- **Performance**: Depends on build configuration
- **Installation**: Requires manual installation of `llama-server` in system PATH
- **Enable**: Set `"llamacpp": { "backend": "system" }` in `config.json` to enable the system backend, or use `lemonade config set llamacpp.backend=system`
- **HIP plugin on non-standard paths**: When an AMD GPU is present, the system backend needs the GGML HIP plugin (`libggml-hip.so`). Lemonade looks for it in the standard system library paths. If your distribution or package manager installs it elsewhere (e.g. NixOS, a custom prefix, or a manual build), set `LEMONADE_GGML_HIP_PATH` to the full path of the plugin so the backend is reported as available:

  ```bash
  export LEMONADE_GGML_HIP_PATH=/opt/rocm/lib/libggml-hip.so
  ```

  The filename must look like `libggml-hip*.so*` (versioned sonames such as `libggml-hip.so.0` are accepted). This Linux-only variable is used solely to detect plugin availability; it is not forwarded to the GGML loader, so it does not change where llama.cpp actually loads the plugin from.

### nathanw
- **Platform**: Linux only
- **Hardware**: AMD Strix Halo (gfx1151)
- **Use Case**: The Strix Halo Vulkan performance fork of llama.cpp, running ordinary GGUF models
- **Tier and format**: guest, container
- **Installation**: `lemonade backends install llamacpp:nathanw` pulls the `vulkan-radv-performance` build of [kyuz0/amd-strix-halo-toolboxes](https://hub.docker.com/r/kyuz0/amd-strix-halo-toolboxes), pinned by digest. Podman or Docker is required; see [Container Backends](container-backends.md)
- **Enable**: `lemonade config set llamacpp.backend=nathanw`, or `--llamacpp nathanw` for one load

### halobox
- **Platform**: Linux only
- **Hardware**: AMD Strix Halo (gfx1151)
- **Use Case**: [halo-box/strix-llama.cpp](https://github.com/halo-box/strix-llama.cpp), the Strix Halo community fork of llama.cpp, on ROCm with ordinary GGUF models
- **Tier and format**: guest, container
- **Installation**: `lemonade backends install llamacpp:halobox` pulls the ROCm `strix-llama` build of [kyuz0/amd-strix-halo-toolboxes](https://hub.docker.com/r/kyuz0/amd-strix-halo-toolboxes), pinned by digest. Podman or Docker is required; see [Container Backends](container-backends.md)
- **Enable**: `lemonade config set llamacpp.backend=halobox`, or `--llamacpp halobox` for one load

## ROCm Channel Configuration

The ROCm backend supports three channels to balance stability, performance, and access to latest features:

### Stable Channel (Default)
```json
{
  "rocm_channel": "stable"
}
```
- **Source**: Custom builds from [lemonade-sdk/llama.cpp](https://github.com/lemonade-sdk/llama.cpp)
- **Binaries**: Common builds for supported architectures
- **Updates**: Frequent updates with latest optimizations and fixes
- **Platform**: Windows and Linux
- **Runtime**: Requires runtime for both Windows and Linux to be installed separately.
- **Best For**: Users who want the latest performance optimizations

### Nightly Channel
```json
{
  "rocm_channel": "nightly"
}
```
- **Source**: Nightly builds from [lemonade-sdk/llamacpp-rocm](https://github.com/lemonade-sdk/llamacpp-rocm)
- **Binaries**: Architecture-specific builds (gfx1150, gfx1151, gfx103X, gfx110X, gfx120X)
- **Updates**: Nightly builds with experimental features and latest upstream changes
- **Platform**: Windows and Linux
- **Runtime**: Bundled runtime on Linux, TheRock ROCm dependencies
- **Best For**: Developers and testers who want bleeding-edge features and are comfortable with potential instability

### Changing Channels

To switch between channels, update your `config.json`:

```json
{
  "rocm_channel": "stable"
}
```

Or use the Lemonade CLI:
```bash
# Switch to stable channel
lemonade config set rocm_channel=stable

# Switch to stable channel (default)
lemonade config set rocm_channel=stable

# Switch to nightly channel (experimental)
lemonade config set rocm_channel=nightly
```

After changing channels, you'll need to reinstall the ROCm backend:
```bash
lemonade backends install llamacpp:rocm
```

### Reusing a System-Installed ROCm (Windows and Linux)

On the stable channel Lemonade normally downloads its own ROCm runtime. By default it installs the ROCm runtime from AMD's pip wheels into a lemonade-managed virtual environment (under `therock-wheels/<arch>-<version>/` in the cache), and falls back to the TheRock tarball (under `therock/<arch>-<version>/`) when Python/venv/pip is unavailable or the wheel install can't serve your GPU. See [`rocm_install_method`](#choosing-the-rocm-install-method) to force one path.

If you already have ROCm installed system-wide, Lemonade reuses it instead of downloading a second copy when it can find a matching version. It locates the install root in this order, using the first directory that contains the HIP runtime (`bin\amdhip64.dll` or `bin\amdhip64_<version>.dll` on Windows, `lib{,64}/libamdhip64.so` on Linux):

1. The `ROCM_PATH` environment variable
2. `rocm-sdk path --root`, when `rocm-sdk` is on your `PATH` (e.g. a ROCm installed from the TheRock pip wheels)
3. The platform default: `HIP_PATH` (set by the AMD HIP SDK installer) on Windows, `/opt/rocm` on Linux

`ROCM_PATH` and `rocm-sdk` work on both platforms. When the runtime is found via one of them, a `major.minor` version match is accepted (and a runtime with no version file is accepted as-is), so a patch-level difference won't trigger a second download. To force Lemonade to use a specific ROCm, set `ROCM_PATH` before starting the server:

```bash
# Linux
export ROCM_PATH=/path/to/rocm
```

```powershell
# Windows (PowerShell)
$env:ROCM_PATH = "C:\path\to\rocm"
```

### Choosing the ROCm Install Method

`rocm_install_method` controls how Lemonade installs its bundled ROCm runtime:

| Value | Behavior |
|---|---|
| `auto` *(default)* | Install from pip wheels; fall back to the TheRock tarball when Python/pip is unavailable or the wheels can't serve the GPU. |
| `wheel` | Use pip wheels only; fail rather than fall back to the tarball. |
| `tarball` | Use the TheRock tarball only; never invokes Python or pip. Use this for Python-averse, air-gapped, or pip-restricted environments. |

```bash
lemonade config set rocm_install_method=tarball
```

It can also be set with the `LEMONADE_ROCM_INSTALL_METHOD` environment variable.

### Pinning to a Specific Version Tag

You can pin `llamacpp.rocm_bin` to a specific release tag instead of using `"builtin"` or `"latest"`. **Each channel downloads from a different GitHub repository, so you must set the correct channel before setting a specific tag.**

| Channel | Repository | Tag format |
|---|---|---|
| `stable` *(default)* | [lemonade-sdk/llama.cpp](https://github.com/lemonade-sdk/llama.cpp) | Lemonade-specific build tags |
| `nightly` | [lemonade-sdk/llamacpp-rocm](https://github.com/lemonade-sdk/llamacpp-rocm) | Nightly tags, e.g. `b1260` |

> **Always set `rocm_channel` to the correct channel before setting `rocm_bin` to a specific tag.** If the tag does not exist in the current channel's repository, the download will fail with HTTP 404.

Example — pin to a specific nightly build:
```bash
# 1. Switch to the nightly channel first
lemonade config set rocm_channel=nightly

# 2. Then pin to the desired nightly tag
lemonade config set llamacpp.rocm_bin=b1260
```

## Passing llama-server Arguments

`llamacpp_args` (`llamacpp.args` in `config.json`, `--llamacpp-args` on `lemonade load`) is appended to the llama-server command line. Arguments you set there take precedence over Lemonade's defaults wherever a default exists:

- Lemonade does not pass `-ngl` / `--gpu-layers`, so llama-server's own offload choice applies and `-ngl 20` (or `--gpu-layers 20`) in `llamacpp_args` is passed through as written. This is how to let a model that does not fit in memory spill to system RAM.
- On a CPU backend Lemonade adds `--no-mmproj-offload` for models with a projector. A `--no-mmproj-offload` or `--mmproj-offload` of your own replaces it. On a GPU backend nothing is added, so `--no-mmproj-offload` keeps the projector out of VRAM.
- RoPE / YaRN recipe options (below) yield to the same flag in `llamacpp_args`.

A small set of flags is still managed by Lemonade because it must know their values: `-m`, `--ctx-size`, `--port`, `--device`, `--jinja`, `--mmproj`, `--model-draft`, `--embeddings` and `--reranking` (with their aliases). Passing one of these is rejected with an error that lists them.

## Extending Context with RoPE / YaRN Scaling

A model can be run past its trained context by scaling its rotary position embeddings. Lemonade exposes llama-server's RoPE and YaRN flags as recipe options. Each one is passed to llama-server only when it is set; leave one unset and llama-server uses the value stored in the model.

| Recipe option | CLI flag (`lemonade load`) | llama-server flag |
|---|---|---|
| `llamacpp_rope_scaling` (`none`, `linear`, `yarn`) | `--llamacpp-rope-scaling` | `--rope-scaling` |
| `llamacpp_rope_scale` | `--llamacpp-rope-scale` | `--rope-scale` |
| `llamacpp_rope_freq_base` | `--llamacpp-rope-freq-base` | `--rope-freq-base` |
| `llamacpp_rope_freq_scale` | `--llamacpp-rope-freq-scale` | `--rope-freq-scale` |
| `llamacpp_yarn_orig_ctx` | `--llamacpp-yarn-orig-ctx` | `--yarn-orig-ctx` |
| `llamacpp_yarn_ext_factor` | `--llamacpp-yarn-ext-factor` | `--yarn-ext-factor` |
| `llamacpp_yarn_attn_factor` | `--llamacpp-yarn-attn-factor` | `--yarn-attn-factor` |
| `llamacpp_yarn_beta_slow` | `--llamacpp-yarn-beta-slow` | `--yarn-beta-slow` |
| `llamacpp_yarn_beta_fast` | `--llamacpp-yarn-beta-fast` | `--yarn-beta-fast` |

For example, to run a model with a native 262144-token window at four times that length:

```bash
lemonade load Qwen3-0.6B-GGUF --ctx-size 1048576 \
  --llamacpp-rope-scaling yarn --llamacpp-rope-scale 4 --llamacpp-yarn-orig-ctx 262144
```

The same keys work in `recipe_options.json`, in a model's options, and in the `/api/v1/load` body. If you also spell one of these flags in `llamacpp_args`, the value in `llamacpp_args` is used and the recipe option for that flag is ignored.

## Choosing the Right Backend

### Decision Tree

1. **Do you have an NVIDIA GPU (Turing or newer)?**
   - Try **CUDA** first for best performance
   - Fall back to **Vulkan** if you encounter issues

2. **Do you have an AMD GPU?**
   - **For Radeon RX 6000/7000 or Ryzen AI iGPU**:
     - Try **ROCm** first for best performance
     - Fall back to **Vulkan** if you encounter issues
   - **For older AMD GPUs (RX 5000 and earlier)**:
     - Use **Vulkan** (ROCm not supported)

3. **Do you have an Intel GPU or older NVIDIA GPU?**
   - Use **Vulkan**

4. **Are you using macOS?**
   - Use **Metal**
   - The Metal-enabled build can also run CPU-only when GPU offload is disabled; no separate CPU backend selection is required

5. **No GPU or unsupported GPU?**
   - Use **CPU**

### ROCm Channel Selection

- **Use Stable** if you:
  - Prefer stability over latest features
  - Want upstream llama.cpp compatibility
  - Are deploying in production

- **Use Nightly** if you:
  - Want bleeding-edge experimental features
  - Are testing unreleased llama.cpp functionality
  - Are comfortable with potential bugs and instability
  - Are a developer contributing to lemonade or llama.cpp

## Platform Specifics

### Linux
- All backends supported (CPU, Vulkan, ROCm, CUDA, System)
- CPU and Vulkan backends support both x86_64 and ARM64 (aarch64) systems; on ARM64, Vulkan is the default
- ROCm requires compatible AMD GPU (see above)
- CUDA requires compatible NVIDIA GPU (see above)
- System backend requires manual llama-server installation

### Windows
- Supported: CPU, Vulkan, ROCm, CUDA
- ROCm requires compatible AMD GPU
- CUDA requires compatible NVIDIA GPU and Windows 11 22H2+ (for bundled bsdtar that extracts .7z assets)
- No system backend support

### macOS
- Supported: Metal
- The Metal-enabled llama.cpp build provides both accelerated and CPU-only execution
- A separate `llamacpp:cpu` selection is not required
