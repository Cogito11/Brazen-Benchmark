# Brazen Benchmark

A lightweight, cross-platform (Windows, Linux, macOS) synthetic benchmark
with a desktop UI built on Dear ImGui. It measures **CPU**, **RAM**,
**GPU** and **disk** throughput with self-contained workloads, keeps a
history of your runs for the session, and reports a composite CPU score
with a (deliberately playful) cat-themed rank.

Brazen is a *relative* benchmark: it is most useful for comparing a
machine with itself over time, or with another machine running the same
version of the same tests. It is not a calibrated, industry-standard
score.

## Contents

- [Using the app](#using-the-app)
- [Included tests](#included-tests)
- [The disk test in detail](#the-disk-test-in-detail)
- [Scoring](#scoring)
- [Reliability and safety](#reliability-and-safety)
- [Tests](#tests)
- [Building](#building)
- [Continuous integration and releases](#continuous-integration-and-releases)
- [Architecture](#architecture)
- [Extending Brazen](#extending-brazen)
- [License](#license)

## Using the app

The window has a navigation sidebar on the left and the selected view on
the right. The sidebar's views are:

| View | What it is |
|---|---|
| **Benchmarks** | Four tabs (CPU, GPU, RAM, Disk), each with its own settings and a *Run Benchmark* button. |
| **Results** | Composite score, component averages and the history of every run this session. |
| **Log & Console** | A live log of everything the app does, plus a command box. |
| **System Info** | Detailed hardware and OS information, with a Refresh button. |
| **App Info** | Version, about text and the rank table. |
| **Settings** | UI scale, a shortcut to the console, and the clear-results confirmation toggle. |

While anything is running, a status bar under the title shows what is
running and a **Cancel All** button. Starting a run from a button
switches to the Results view.

### Benchmark tabs

Every tab remembers its settings independently, so a quick CPU check and
a long RAM run don't overwrite each other.

- **CPU**: choose which of the five CPU tests to include, a Single-Core /
  Multi-Core / Both mode, and a duration per test (1-30 s, default 8 s).
- **RAM**: buffer size (8 / 16 / 32 / 64 / 128 MB; larger buffers better
  defeat big L3 caches), an automatic or manual thread count, mode, and
  duration (1-30 s). The tab shows how much memory the run will allocate
  and how much is free; a run that wouldn't fit is refused (see
  [Reliability and safety](#reliability-and-safety)).
- **GPU**: workload checkboxes (ALU, texture, fill rate), render
  resolution (256 up to 2048 square) and duration per workload (1-30 s).
  There is no mode selector, because GPU work has no single/multi-core
  equivalent (see [why GPU testing is different](#why-gpu-testing-is-architecturally-different)).
  All detected GPUs are listed, but only the one driving the window can
  be benchmarked.
- **Disk**: target drive, test size (512 MB / 1 / 2 / 4 GB, default 1 GB)
  and Write / Read checkboxes. There is **no duration**: the test moves a
  fixed amount of data and runs until it is finished. Mode
  (Single-Core / Multi-Core / Both) lives under *Advanced options*,
  defaulting to Single-Core. See [the disk test in detail](#the-disk-test-in-detail).

If the GPU test cannot run (no OpenGL 3.3 core context), the app still
works and the GPU tab and log say why.

### Results

- A table of every run this session with date, test, mode, score and
  status (Done, Cancelled or Failed). Click a row for its detail view
  (threads, elapsed time, whether threads were pinned to cores, and any
  notes or failure reason). Select rows to delete them, or clear
  everything (with an optional confirmation).
- **Copy All** puts the results table on the clipboard.
- **Composite Score** cards for Single-Core and Multi-Core, and a
  **Component Averages** table for RAM, Storage and the three GPU
  workloads. See [Scoring](#scoring).
- History is kept in memory only: nothing is written to disk between
  launches.

### Log & Console

The log records everything of note the app does, each line timestamped
and tagged with a level and a category:

- **Startup**: the windowing library, OpenGL version and renderer, display
  scale, registered tests, and detected hardware: CPU (cores, threads,
  cache, instruction sets), RAM, OS, system/board/BIOS, every GPU, and
  every drive with its free space, where its test files would go, and
  which one the disk test will use.
- **Runs**: each job being queued, started, finished, cancelled or failed
  (with the reason), the settings it used, GPU calibration, the
  disk test's I/O mode and phase changes, and warnings such as threads
  the OS refused to pin.
- **Actions**: drive selection, refreshes, deleted and cleared results.

Info / Warning / Error lines can be filtered, the whole log can be copied
with **Copy All**, and the view follows new output only while you are at
the bottom, so scrolling up to read isn't interrupted.

The box at the bottom is a command console. There is nothing separate to
launch, and on Windows no extra console window opens. `Up` / `Down`
recalls earlier commands.

| Command | What it does |
|---|---|
| `help` | List the commands |
| `status` | What is running or queued, with progress |
| `run cpu\|ram\|gpu\|disk\|all` | Start a benchmark using the settings currently on its tab. `all` runs the GPU step after the others finish so they don't disturb each other |
| `cancel` | Stop everything running or queued |
| `results` | List recorded results |
| `sysinfo` | CPU, RAM, OS and system details |
| `drives` / `drive <n>` | List drives / choose the drive for the disk test |
| `refresh` | Re-scan GPUs and drives |
| `version` | Show the version |
| `clear log` / `clear results` | Clear the log / all recorded results |
| `quit` | Close Brazen |

If Brazen is started from an existing terminal, log lines are also echoed
there (handy for development). Double-clicking the executable opens only
the app window.

## Included tests

| Test | Category | What it stresses | Unit |
|---|---|---|---|
| Integer Math | CPU | Mixed add/mul/xor/mod integer ALU throughput | Mops/s |
| Floating Point | CPU | sin/cos/sqrt numerical integration | Mops/s |
| Prime Sieve | CPU | Sieve of Eratosthenes (memory + integer logic) | sieves/s |
| Hashing | CPU | FNV-style buffer hashing (sequential memory read) | MB/s |
| Sorting | CPU | `std::sort` over randomized integer arrays | Melems/s |
| RAM Bandwidth | RAM | Sequential copy over a configurable, larger-than-cache buffer | GB/s |
| GPU ALU | GPU | Per-pixel iterative floating-point shader workload | GFLOPS |
| GPU Texture | GPU | Repeated filtered texture sampling | GB/s |
| GPU Fill Rate | GPU | Fullscreen raster/fill throughput | Gpixels/s |
| Disk Write | Disk | Sequential writes, flushed to the physical device | MB/s |
| Disk Read | Disk | Sequential reads of a previously written file | MB/s |

## The disk test in detail

The disk test is deliberately simple and predictable.

- **Fixed work, not a timer.** Write moves `test size` bytes to a new
  temporary file in 4 MB blocks; Read first prepares a file of that size
  and then reads it back once. The test ends when the data has been
  moved, so a fast NVMe drive finishes in seconds and a slow USB stick
  takes longer. The status bar shows the phase, a progress bar, the
  percentage and elapsed time. A 5-minute safety limit stops a stalled
  device and reports what completed.
- **Read preparation is not timed.** It is shown as "Preparing test file"
  and cancellable.
- **Durable writes.** The flush to the physical device is inside the
  timed section, so the score is write speed to the drive, not how fast
  the OS accepted data into RAM.
- **OS cache bypassed.** I/O uses `FILE_FLAG_NO_BUFFERING` on Windows,
  `O_DIRECT` on Linux and `F_NOCACHE` on macOS, so Read measures the
  device rather than RAM. If a filesystem refuses (some network shares,
  FUSE mounts, older tmpfs), the test falls back to buffered I/O, evicts
  cached pages where the platform allows it, and notes on the result that
  numbers may be inflated.
- **Incompressible data**, so drives that compress or special-case zeros
  can't flatter the result.
- **Errors are results.** A full, read-only, missing or failing drive
  marks the result *Failed* with the reason (shown in the log, Results
  table and detail view) instead of silently scoring 0. Failed and
  cancelled results never feed any score.
- **Cleanup.** Temporary files (`brazen_disktest_*.tmp`) are deleted when
  a run finishes, is cancelled or fails. If an earlier run crashed or lost
  power and left files behind, the next disk run removes those older than
  15 minutes (never a more recent file, in case another Brazen is working
  in the same folder). Free space is checked when the run is queued and
  again when the test starts, always leaving a 512 MB reserve, and a
  single file is capped at 1 TB.
- **Multi-Core** (Advanced): one temporary file per worker, up to 4
  workers. It measures concurrent I/O behavior, not a drive's simple
  single-stream speed, so don't compare it directly with Single-Core
  results.

This is a sequential-throughput test. It is not a queue-depth-sweeping or
random-I/O tool like fio or CrystalDiskMark.

## Scoring

- **Composite score (CPU only).** Each completed CPU test's result is
  divided by a reference baseline and multiplied by 1000; the composite is
  the average across the completed CPU tests, separately for Single-Core
  and Multi-Core runs. The latest completed result per test and mode is
  used, so repeating a run doesn't skew it.
- **Rank.** The composite maps to a cat-themed rank from *Alley Cat* up to
  *Cheetah* (see **View Rank Table** in Results or App Info). Multi-Core
  uses a wider scale because aggregate throughput grows with core count.
  Baselines are illustrative estimates anchored around an Intel Core
  i9-13900H, not calibrated against reference hardware, so treat the rank
  as a fun relative label.
- **Component averages.** RAM and Storage (Disk Read and Write) are
  averaged directly since they share a unit. The three GPU workloads use
  different units and are shown separately. GPU, RAM and Disk results
  never affect the composite CPU score.
- Cancelled and failed runs are excluded from all scores.

## Reliability and safety

Benchmarks push the machine hard, so the engine is built to fail cleanly
and to measure what it claims to measure. These guarantees are enforced by
the self-test suite (see [Tests](#tests)).

**The app doesn't go down with a test.**
- An exception from any test (out of memory above all) becomes a *Failed*
  result with the reason, in the log, Results table and detail view, never
  a crash. The same applies if a worker thread can't be started.
- A run that finishes without doing any work is reported as failed rather
  than as a plausible-looking zero score.
- The RAM test is refused up front if its buffers (two per thread) wouldn't
  fit in about 60 % of the *currently free* memory (50 % of installed RAM if
  free memory can't be measured). On Linux free memory honors container
  (cgroup) limits. This matters because exceeding free memory doesn't
  produce a clean error: Linux over-commits and then the OOM killer ends
  the process; Windows pages until the machine crawls.
- The disk test re-checks free space when it starts, caps its file size,
  and cleans up leftovers (see above).
- The GPU test ramps up from tiny draws instead of starting with a large
  one, abandons the run if a single draw approaches the driver watchdog
  limit (Windows resets a GPU busy for about 2 s), and treats any OpenGL
  error as a failure, since a failed draw "completes" instantly and would
  otherwise be scored as an absurdly fast GPU.

**Measurements are valid.**
- Every worker thread finishes `Setup()` before any starts timing, so all
  threads are measured over the same window. Otherwise early threads run
  alone for a while and inflate aggregate numbers such as RAM bandwidth.
- Threads are pinned only to cores the process may actually use
  (containers, `taskset`, `start /affinity`), and a "use every core" run
  starts one thread per *allowed* core. On Windows with more than 64
  logical processors, cores are addressed through processor groups.
- A run is marked *cancelled* only if a worker actually saw the cancel
  request, not merely because Cancel was clicked just after it finished.
- Nonsensical durations (NaN, negative, huge) are clamped.
- The GPU test cross-checks the driver's timer query against wall-clock
  time. Some drivers (software rasterizers, for instance) report a small
  fraction of the real time, which inflated scores by roughly 65x in
  testing; when they disagree, wall-clock timing is used and the log says
  so. The fill-rate workload issues many full-target draws per timed chunk
  instead of timing a single microsecond-long draw.
- Each CPU/RAM test exposes a read-only verification hook so the tests can
  check that the arithmetic is right (exactly 148,933 primes below two
  million, hashes matching an independent FNV-1a implementation, sorted
  output, identical state between identical instances, copies leaving both
  buffers equal).

**Known limits.** Hashing reports MiB/s but labels it MB/s, while RAM and
disk report decimal MB/s; this is kept because the score baseline was
measured that way and changing it would shift every Hashing score. macOS
has no hard thread pinning (only an affinity tag) and no available-memory
reading, so it falls back to a share of installed RAM.

## Tests

The engine has a dependency-free regression suite
([`tests/SelfTest.cpp`](tests/SelfTest.cpp), no GUI or OpenGL needed):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target BrazenSelfTest
ctest --test-dir build --output-on-failure   # about 5 seconds
build/BrazenSelfTest --list                  # or run named tests directly
```

It covers the runner (exceptions, start gating, cancellation, durations),
the manager (job bookkeeping, shutdown, concurrent enqueue/cancel), the
registry, thread affinity, each CPU/RAM test's correctness, and the disk
test (round trip, bad paths, low space, cancellation, leftover cleanup).
Tests that need something the machine can't provide, such as several GB of
free disk, are reported as skipped, not failed.

To run it under the sanitizers (GCC/Clang):

```bash
cmake -S . -B build-asan -DBRAZEN_SANITIZE=address,undefined && cmake --build build-asan --target BrazenSelfTest
cmake -S . -B build-tsan -DBRAZEN_SANITIZE=thread            && cmake --build build-tsan --target BrazenSelfTest
ctest --test-dir build-asan --output-on-failure
ctest --test-dir build-tsan --output-on-failure
```

GPU behavior needs an OpenGL context and isn't part of this suite; it is
exercised by running the app (or any GL 3.3 context, including a software
renderer under Xvfb).

## Building

### Dependencies

- CMake >= 3.16
- A C++17 compiler
- OpenGL 3.3 core support for the GPU test. The app still runs without
  it; the GPU test just reports itself unavailable, with the reason.
- GLFW 3: the build uses your system's GLFW if `find_package(glfw3)`
  succeeds, otherwise CMake fetches and builds it with `FetchContent`.

Dear ImGui is vendored under `third_party/imgui` (pinned to v1.90.9), so
there is no separate install step for it.

| Platform | Setup |
|---|---|
| Ubuntu/Debian | `sudo apt-get install -y cmake libglfw3-dev libgl1-mesa-dev xorg-dev build-essential` |
| macOS | `brew install cmake glfw` |
| Windows | Install CMake and a C++17 toolchain (MSVC or MinGW-w64); GLFW is fetched automatically, or use vcpkg. |

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
```

This produces three executables:

- **`Brazen`**: the GUI application (`build/Brazen.exe` on Windows). On
  Windows it is linked as a GUI-subsystem program, so launching it does
  not open a console window.
- **`BrazenSelfTest`**: the regression suite described under
  [Tests](#tests); disable with `-DBRAZEN_BUILD_TESTS=OFF`.
- **`BrazenCoreTest`**: a small headless console runner that executes
  every CPU/RAM test once. It has no GUI or OpenGL dependency, so it is
  useful for CI and for machines without a display or GPU. GPU and disk
  testing are not part of it. Disable it with
  `-DBRAZEN_BUILD_CORE_TEST=OFF`.

### Version string

The version shown on App Info (and by the `version` command) is baked in
at build time. The local default lives in
[`cmake/BrazenVersion.cmake`](cmake/BrazenVersion.cmake) and can be
overridden:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DBRAZEN_VERSION=1.2.3
```

CI overrides it automatically: stable releases use the pushed `vX.Y.Z`
tag without the leading `v`, rolling builds use an identifier such as
`rolling-42`.

## Continuous integration and releases

- **`.github/workflows/rolling.yml`** builds all three platforms on every
  push to `main` and republishes them as a single "Rolling Release"
  pre-release, so there is always a download of the latest code.
- **`.github/workflows/release.yml`** builds and publishes a permanent,
  versioned release when a tag matching `v*.*.*` is pushed.
- **`.github/workflows/build.yml`** holds the build steps both call. It
  builds Linux (x86_64), Windows (x86_64) and macOS (arm64), runs a
  headless core-logic smoke test, launches the GUI briefly as a smoke test
  (Linux under Xvfb, Windows with software OpenGL), and packages the
  artifacts (`Brazen-linux-x86_64.tar.gz`, `Brazen-windows-x86_64.zip`,
  `Brazen-macos-arm64.tar.gz`). It is not triggered directly.

- **`.github/workflows/tests.yml`** runs the self-test suite on every push
  and pull request: normally on Linux, Windows and macOS, and on Linux
  under AddressSanitizer + UndefinedBehaviorSanitizer and under
  ThreadSanitizer.

To cut a release:

```bash
git tag v1.0.0
git push origin v1.0.0
```

## Architecture

```
src/
  core/                     UI-independent engine (no ImGui/OpenGL)
    IBenchmarkTest.h        Interface every CPU/RAM/disk test implements,
                            including the optional "fixed-work" hooks
    TestRegistry.h          Self-registration + lookup of default-constructible tests
    BenchmarkRunner.h       Runs one test single- or multi-threaded (with
                            per-thread core pinning) and aggregates a score
    BenchmarkManager.h      Background worker + job queue; bridges the
                            runner to the UI thread and reports progress
    Log.h                   Thread-safe application logger (see below)
    ThreadAffinity.h        Allowed-core detection + cross-platform thread pinning
    HardwareInfo.h          CPU/RAM/OS/GPU/drive detection
    ScoreCalculator.h       Composite CPU score + cat-tier ranking
    RegisterTests.h         Registers the built-in CPU/RAM tests
    tests/
      IntegerMathTest.h, FloatMathTest.h, PrimeSieveTest.h,
      HashingTest.h, SortingTest.h    CPU tests
      RamBandwidthTest.h              RAM test (buffer size via constructor)
      DiskIoTest.h                    Disk write/read test + cross-platform
                                      direct-I/O file wrapper
  gpu/                      GPU testing, deliberately separate (see below)
    GLLoader.h / .cpp       Minimal OpenGL 3.3 core function loader
    GpuComputeTest.h / .cpp Shader/FBO/texture setup and the ALU, texture
                            and fill-rate workloads
    GpuTestRunner.h / .cpp  Calibration + timed-run state machine driven
                            from the render loop
  ui/
    BrazenApp.h / .cpp      All ImGui layout and state: sidebar, views,
                            benchmark tabs, log, console and its commands
  main.cpp                  GLFW + OpenGL bootstrap and the render loop
  core_test_main.cpp        Headless console entry point (CPU/RAM only)
tests/
  SelfTest.h                Tiny test framework (TEST_CASE / CHECK / SKIP_UNLESS)
  SelfTest.cpp              The regression suite (BrazenSelfTest)
```

### Logging

Everything goes through `brazen::Logger` (`LogInfo` / `LogWarn` /
`LogError`, printf-style, with a category tag). It is thread-safe and
header-only, so the worker thread, the render thread and code that runs
before the UI exists can all log; entries queue until `BrazenApp` drains
them into the Log view each frame. Entries are also echoed to `stderr`,
which is visible when the app is launched from a terminal.

### Fixed-work tests

Most tests are time-boxed: the runner calls `RunWorkChunk()` until the
requested duration elapses. A test whose natural unit is "do this much
work" (the disk test) instead overrides:

- `RunsToCompletion()`: return `true`; the duration passed to the runner
  becomes only a safety time limit.
- `IsComplete()`: per instance, true when that worker is done or has
  failed. Defaults to `false`, so time-boxed tests behave exactly as
  before.
- `GetProgress()`: a fraction and phase name, read from the UI thread on
  the queued prototype (so the state must be shared between clones and
  thread-safe).
- `HasFailed()` / `StatusNote()`: a failure reason, or an informational
  note, surfaced on `BenchmarkResult::failed` / `notes`.
- `SetCancelFlag()`: lets a slow `Setup()` (such as preparing a multi-GB
  read file) notice Cancel.

### Per-job state

`BenchmarkManager::QueuedRun` carries its own duration, thread count and
an optional pre-built test instance, captured when the job is enqueued
rather than read from shared state when it starts. CPU, RAM, GPU and disk
settings are independent, so a RAM run queued with 15 s must not
retroactively change CPU jobs already waiting with 8 s. The custom
instance is also what lets the RAM buffer size and the disk target drive
and size take effect: those tests aren't default-constructible, so they
bypass `TestRegistry`'s factory for that job. The manager also tracks a
pending-job count (queued plus running) so "start the next thing once
everything has drained" has no gap between a job leaving the queue and
being marked busy.

### Why GPU testing is architecturally different

CPU/RAM tests implement `IBenchmarkTest`, and `BenchmarkRunner` clones
them once per worker thread. That is well defined because plain CPU work
has no single-owner resource for threads to contend over.

An OpenGL context can be current on only one thread at a time, and
issuing draw calls from several threads is unsafe. So GPU testing does
**not** go through `IBenchmarkTest`/`BenchmarkRunner`: `GpuTestRunner` is
driven once per frame from `main.cpp`, on the thread that owns the GL
context. Because each work chunk waits for the GPU (timer queries where
available, `glFinish()` otherwise), the UI renders at a reduced frame rate
during a GPU run. That is expected.

`GpuTestRunner` still produces a `BenchmarkResult` (tagged `RunMode::Gpu`)
that is fed to `BrazenApp::AddExternalResult()`, so GPU runs share the
results table. This is also why the GPU tab has no mode selector, and why
GPU results never feed the CPU composite.

### Core affinity

`AllowedCores()` lists the logical CPUs the process may use (Linux
`sched_getaffinity`, the Windows process affinity mask or processor
groups). Single-core runs pin to the highest-numbered allowed core (core 0
usually fields more OS and interrupt traffic); multi-core runs pin thread
*i* to the *i*-th allowed core (or a chosen subset when a manual RAM thread
count is used) and default to one thread per allowed core. If the OS
rejects a pin request, the result is still recorded, flagged "Pinned: No"
and logged as a warning. GPU results show "N/A".

## Extending Brazen

### Adding a CPU test

1. Create a class implementing `brazen::IBenchmarkTest` (see any file in
   `src/core/tests/`). At minimum implement `GetName()`, `GetCategory()`,
   `GetUnit()`, `RunWorkChunk()` and `Clone()`.
2. Register it in `RegisterBuiltInTests()` (`src/core/RegisterTests.h`):
   ```cpp
   reg.Register([] { return std::make_unique<MyNewTest>(); });
   ```
3. It then appears as a checkbox on the CPU tab and is included in
   `run cpu`.

Tests that need constructor arguments (like RAM's buffer size or the disk
target) aren't registered; construct them in the matching
`Start*Benchmark()` in `BrazenApp.cpp` and pass them to
`BenchmarkManager::Enqueue()`. If a test runs until its work is done
rather than for a duration, implement the
[fixed-work hooks](#fixed-work-tests).

When adding a test, also add a case to `tests/SelfTest.cpp` that checks its
result is *correct* (not just that it runs), and give the class a small
read-only accessor for whatever state that check needs.

### Logging from new code

Include `core/Log.h` and call `LogInfo("Category", "message %d", x)`.
Use `LogWarn` for things the user should notice and `LogError` for
failures; they show up in the Log view automatically.

### Extending the GPU test

`GpuComputeTest` holds the GLSL sources inline (`src/gpu/GpuComputeTest.cpp`).
To add a workload, add it there, then extend `GpuTestRunner`'s workload
selection and the GPU tab's checkboxes. The calibration, timing and
result-reporting machinery in `GpuTestRunner` doesn't need to change.

## License

Apache License 2.0. See [LICENSE](LICENSE). Dear ImGui and GLFW are
third-party components under their own licenses.
