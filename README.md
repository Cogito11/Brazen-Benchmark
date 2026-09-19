# Brazen

A cross-platform C++ benchmarking app with a desktop UI (Dear ImGui).
Pick a benchmark from the Home tab, customize it in a configuration
modal (which tests, mode, duration, and category-specific settings),
and run it -- CPU, RAM, and GPU each get their own guided flow, or hit
"Run Everything" for a one-click full sweep.

## Layout

- **Left sidebar -- "Your Machine"**: a compact hardware summary (CPU,
  RAM, GPU) with a "View more" link to a combined details modal; your
  composite CPU score (with a cat-tier rank, click the "?" for how it's
  calculated); and a status section that appears while something's
  running, with a Cancel All button.
- **Right side -- tabs**: **Home** (hardware recap + the four "Run ___
  Benchmark" buttons), **Results** (the results table; auto-selected
  when you start a run), and **Log** (a scrolling run log). Both Results
  and Log have one-click "Copy All" buttons to the system clipboard.

## Per-category configuration modals

Each "Run X Benchmark" button opens a modal scoped to that category
before anything starts:

- **CPU**: checkboxes for which of the 5 CPU tests to include, a
  Single-Core / Multi-Core / Both mode selector, and a duration slider.
- **RAM**: buffer size (8/16/32/64/128 MB -- bigger sizes better defeat
  large L3 caches on high-end CPUs), an auto/manual thread count, mode,
  and duration.
- **GPU**: workload checkboxes (ALU, texture, and fill rate), render
  resolution (256 up to 2048), and duration per selected workload. No
  mode selector -- GPU work has no single/multi-core equivalent (see
  "Why GPU testing is architecturally different" below).

Each category remembers its own settings independently between opens
(a quick CPU check and a long RAM soak test can coexist without one
overwriting the other's duration), and starting a run auto-switches you
to the Results tab.

## Included tests

| Test | Category | What it stresses | Unit |
|---|---|---|---|
| Integer Math | CPU | Mixed add/mul/xor/mod integer ALU throughput | Mops/s |
| Floating Point | CPU | sin/cos/sqrt numerical integration | Mops/s |
| Prime Sieve | CPU | Sieve of Eratosthenes (memory + integer logic) | sieves/s |
| Hashing | CPU | FNV-style buffer hashing (sequential memory read) | MB/s |
| Sorting | CPU | `std::sort` over randomized integer arrays | Melems/s |
| RAM Bandwidth | RAM | Sequential copy over a configurable, >cache-size buffer | GB/s |
| GPU ALU | GPU | Per-pixel iterative floating-point shader workload | GFLOPS |
| GPU Texture | GPU | Repeated filtered texture sampling | GB/s |
| GPU Fill Rate | GPU | Fullscreen raster/fill throughput | Gpixels/s |

## Building

### Dependencies

- CMake >= 3.16
- A C++17 compiler
- OpenGL (3.3 core support required for the GPU test; the app still runs
  fine without it, the GPU test just reports itself unavailable, both in
  the sidebar and with a specific reason in its Home-tab button tooltip)
- GLFW3 (the build uses your system's GLFW if `find_package(glfw3)`
  succeeds; otherwise CMake fetches and builds it from source
  automatically via `FetchContent`)

On Ubuntu/Debian, the quickest way to get everything:

```bash
sudo apt-get update
sudo apt-get install -y cmake libglfw3-dev libgl1-mesa-dev xorg-dev build-essential
```

On macOS (with Homebrew): `brew install cmake glfw`
On Windows: install CMake, use vcpkg or let `FetchContent` build GLFW for you.

Dear ImGui itself is vendored under `third_party/imgui` (pinned to
v1.90.9) so there's no separate install step for it.

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
```

This produces two executables:

- **`Brazen`** -- the full GUI application.
- **`BrazenCoreTest`** -- a small headless console runner that registers
  and runs every CPU/RAM test once (useful for CI, or any environment
  without a display/GPU, since it has zero GUI/GL dependencies). GPU
  testing is intentionally not part of this binary, since it requires a
  real GL context. Disable the whole target with
  `-DBRAZEN_BUILD_CORE_TEST=OFF` if you don't want it.

Run the GUI with `./Brazen` (or `build/Brazen.exe` on Windows).

### Setting the version string

The local/development default is kept in
[`cmake/BrazenVersion.cmake`](cmake/BrazenVersion.cmake). The version shown
on the App Info screen is baked in at build time and can be overridden with
`-DBRAZEN_VERSION`:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DBRAZEN_VERSION=1.2.3
```

If omitted, CMake uses `BRAZEN_DEFAULT_VERSION` from the config file. CI
overrides it automatically: stable releases use the pushed `vX.Y.Z` tag
without the leading `v`, while rolling builds use an identifier such as
`rolling-42`. This keeps local defaults easy to update without changing the
stable-versus-rolling release behavior.

## Continuous integration and releases

- **`.github/workflows/rolling.yml`** builds all three platforms on
  every push to `main` and republishes them as a single "Rolling
  Release" pre-release on the repo's Releases page, so there is always
  a downloadable build of the latest code.
- **`.github/workflows/release.yml`** builds and publishes a permanent,
  version-tagged release whenever a tag matching `v*.*.*` (for example
  `v1.0.0`) is pushed. The version baked into the binary comes from the
  tag name with the leading `v` stripped.
- **`.github/workflows/build.yml`** holds the actual build steps both
  of the above call. It is not triggered directly.

To cut a release: update anything that needs updating, then

```bash
git tag v1.0.0
git push origin v1.0.0
```


## Architecture

```
src/
  core/                     UI-independent CPU/RAM benchmarking engine
    IBenchmarkTest.h        Test interface every CPU/RAM benchmark implements
    TestRegistry.h          Self-registration + lookup of all CPU/RAM tests
    BenchmarkRunner.h        Runs one test single- or multi-threaded (with
                             per-thread core-affinity pinning) for a fixed
                             duration and aggregates a score
    BenchmarkManager.h       Background worker + job queue; bridges the
                             runner to the UI thread safely. Each queued
                             job carries its own duration and (optional)
                             pre-built test instance -- see "Per-job state"
                             below for why that matters.
    ThreadAffinity.h         Cross-platform "pin this thread to core N"
    HardwareInfo.h           Cross-platform CPU/RAM model & capacity query
    ScoreCalculator.h        Composite CPU score + cat-tier ranking
    RegisterTests.h          Wires up every built-in CPU/RAM test
    tests/
      IntegerMathTest.h, FloatMathTest.h, PrimeSieveTest.h,
      HashingTest.h, SortingTest.h    Real CPU test implementations
      RamBandwidthTest.h              RAM test, buffer size configurable
                                       via constructor (see below)
  gpu/                      GPU testing -- deliberately separate, see below
    GLLoader.h / .cpp       Minimal OpenGL 3.3 core function loader
    GpuComputeTest.h / .cpp  Shader/FBO/texture setup plus ALU, texture,
                 and fill-rate workloads; resolution is
                 changeable via SetResolution()
    GpuTestRunner.h / .cpp   Calibration + timed-run state machine, driven
                             from the main thread's render loop; also
                             exposes GPU vendor/renderer strings
  ui/
    BrazenApp.h / .cpp      All ImGui layout/state: sidebar, tabs, and
                             every modal (hardware details, score info,
                             the three benchmark configuration modals,
                             clear-results confirmation)
  main.cpp                  GLFW + OpenGL3 bootstrap and the render loop
  core_test_main.cpp        Headless console entry point (CPU/RAM only)
```

### Per-job state (duration, thread count, custom test instance)

`BenchmarkManager::QueuedRun` carries its own `durationSeconds` and an
optional pre-built test instance (`shared_ptr<IBenchmarkTest>`), captured
at the moment a job is enqueued -- not read from a shared field at the
moment it starts running. This matters because CPU, RAM, and GPU each
have independent duration settings: if duration lived in one shared
mutable field on `BenchmarkManager`, enqueueing a RAM run with a 15s
duration while a CPU run (queued with an 8s duration) was still waiting
its turn would retroactively change the CPU jobs' duration too. The
optional custom test instance is what lets the RAM modal's buffer-size
setting actually take effect: `BenchmarkManager::Enqueue()` can take a
ready-made `RamBandwidthTest` instance built with the chosen buffer size,
bypassing `TestRegistry`'s always-default-constructed factory for that
one job.

### Why GPU testing is architecturally different

Every CPU/RAM test implements `IBenchmarkTest`, and `BenchmarkRunner`
runs it by cloning it once per worker thread and letting each clone run
independently -- that's what "multi-core" means for those tests, and
it's a completely valid, well-defined thing to do because plain CPU work
has no shared, single-owner resource that multiple threads would
contend over incorrectly.

An OpenGL context breaks that assumption: a GL context can only be
*current* on one thread at a time, and issuing draw calls to it from
multiple threads without very careful (and mostly resource-loading
oriented) context-sharing setup is unsafe. So GPU testing intentionally
does **not** go through `IBenchmarkTest`/`BenchmarkRunner` at all --
`GpuTestRunner` is driven once per frame from `main.cpp`, on the same
thread that owns the GL context and runs the render loop. A side effect:
because each work chunk waits for GPU completion, using timer queries when
available and `glFinish()` as a fallback, the UI renders at a reduced frame
rate while a GPU run is active. That's expected, not a bug.

`GpuTestRunner::PollAndAdvance()` still produces a `BenchmarkResult` --
the same struct CPU/RAM tests produce -- so GPU runs show up in the same
results table (tagged with `RunMode::Gpu`), just fed in directly via
`BrazenApp::AddExternalResult()` from `main.cpp` instead of through
`BenchmarkManager`. It's also why the GPU configuration modal has no
mode selector, and why the CPU-focused composite score never includes
GPU results.

### Core affinity

CPU/RAM single-core runs pin to the *highest-numbered* logical core
(core 0 typically fields more OS/interrupt traffic, adding noise to a
single-thread measurement); multi-core runs pin thread *i* to core *i*
(or to a user-chosen subset, if the RAM modal's manual thread count is
used). If the OS rejects a pin request, the result is still recorded but
flagged (a "Pinned: No" column with a tooltip) rather than silently
assumed to be clean. GPU results show "N/A" there instead, since core
affinity isn't a meaningful concept for GPU work.

### Adding a new CPU or RAM test

1. Create a class implementing `brazen::IBenchmarkTest` (see any file
   under `src/core/tests/` as a template). At minimum implement
   `GetName()`, `GetCategory()`, `GetUnit()`, `RunWorkChunk()`, and
   `Clone()`.
2. Register it in `RegisterBuiltInTests()` (`src/core/RegisterTests.h`):
   ```cpp
   reg.Register([] { return std::make_unique<MyNewTest>(); });
   ```
3. It automatically appears as a checkbox in the CPU (or RAM) benchmark
   modal and participates in "Run Everything".

### Extending the GPU test

`GpuComputeTest` currently runs one fragment-shader workload
(`src/gpu/GpuComputeTest.cpp` has the GLSL source inline). To add a
second GPU test (e.g. a memory-bandwidth-style texture read/write test,
or a fill-rate test), the cleanest path is to give `GpuTestRunner` a
small enum/selector for "which GPU test is active" and a second
`GpuComputeTest`-like class, then extend the GPU configuration modal
with a test-selector -- the calibration/timing/result-reporting
machinery in `GpuTestRunner` doesn't need to change.
