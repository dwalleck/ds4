# Native Windows ARM64 port: development plan

## Objective and scope

Build and validate ds4 natively on Windows ARM64 with the RTX Spark N1X,
initially using DeepSeek V4 Flash (`ds4f-q2`). Work in a temporary development
fork until the implementation is reliable; only then trim it and prepare
focused upstream PRs. An upstream submission is not the immediate milestone.

The first production target is single-GPU CUDA inference, followed by SSD
streaming, the server, and the agent. CPU-only builds provide portability and
unit-test coverage, not a requirement to run the full model on the CPU.
Distributed inference, RDMA, and multi-GPU Windows support are deferred. Their
existing Linux/macOS paths must not regress.

This is a plan, not a declaration of Windows support. No native ds4 Windows
build has been demonstrated yet.

## Current checkout and machine

- Checkout: `C:/Users/dwall/repos/ds4`.
- Upstream remote: `origin`, `https://github.com/antirez/ds4.git`.
- Development fork: `fork`, `https://github.com/dwalleck/ds4.git`.
- Working branch: `windows-dev`, initially based on upstream `ee7e840`.
- Initial local commit: `d34c3fd`, adding line-ending attributes. Revisit its
  fixture rules before further changes; do not bulk-renormalize scoring data.
- GPU: NVIDIA RTX Spark N1X, with approximately 96 GiB reserved for GPU memory.
- Physical memory: 128 GiB installed; Windows reports approximately 30.5 GiB
  available to the OS because of the GPU reservation.
- vLLM is stopped; the last inventory showed approximately 95,177 MiB GPU memory
  free. Keep it stopped during large-model testing.
- Pagefile configuration observed: 98,304 MB initial, 131,072 MB maximum.
- Visual Studio Community 2026 18.11.2 is installed, with MSVC 19.51.36260 native
  ARM64 tools and ARM64 runtime libraries.
- Windows SDK 10.0.26100.0 and its ARM64 libraries are installed.
- Clang/clang-cl, nvcc, CMake, and Ninja were not found in the inventory searches.
  This is not proof that no custom installation exists elsewhere.
- WSL is installed but no distribution was present at the last check. CUDA
  passthrough on this particular ARM64/N1X configuration is **not yet verified**.

Stopping vLLM frees its allocations, not the boot-time GPU reservation. There
is no planned reservation change for initial compilation and unit testing.

## Implementation sources and boundaries

### 1. Upstream ds4: authoritative engine and tests

- Repository: https://github.com/antirez/ds4
- Local: `C:/Users/dwall/repos/ds4`
- Guidance: `AGENT.md`, `CONTRIBUTING.md`, `docs/TESTING.md`,
  `docs/DGX_SPARK.md`, `docs/SSD_STREAMING.md`.
- Reuse the existing model loader, CPU reference, CUDA kernels, `sm_121a` build
  configuration, test vectors, and regression tests.
- Avoid importing an older fork's engine wholesale. Preserve existing Linux,
  Metal, ROCm, CUDA, SSD-streaming, and distributed behavior.

### 2. pixmaate/gufo Windows port: primary compatibility implementation

- Repository/branch: https://github.com/pixmaate/gufo/tree/windows-port
- Candidate revision recorded by bubo:
  `2eb0778ac15d9e1c18c5b2f9e5dbbecc19e20ec2`.
- Candidate import: `compat/win32/`, including POSIX shadow headers,
  `posix.cpp`, `spawn.cpp`, and descriptor/socket helpers.
- Intended uses: file descriptors, positional I/O, mapping, timing, locking,
  sockets, process spawning, and Windows error translation.
- License: MIT according to bubo's provenance records. Verify the actual source
  revision and retain the full applicable copyright and permission notices.
- **Decision:** vendor first to establish a working implementation, then trim.
  Do not assume the candidate revision exactly matches bubo's current files.
- Audit before using: thread safety, descriptor ownership, errno behavior,
  mapping alignment, cancellation, short reads, and ARM64 assumptions.

**Important policy conflict:** this compatibility layer contains C++. ds4's
`AGENT.md` says not to introduce C++. Keep ds4's own sources in C. Before
importing the C++ implementation, explicitly resolve the temporary vendored
exception with the user; otherwise adapt the needed implementation into C.
A working C-callable interface alone does not satisfy the no-C++ rule.

### 3. bubo: integration reference and downstream fixes

- Repository: https://github.com/haloforge-labs/bubo
- Local: `C:/Users/dwall/repos/bubo`
- Inspect: `compat/win32/`, `CMakeLists.txt`, `docs/WINDOWS.md`,
  `tools/windows/`, and `src/core/mapped_prefetch.hpp`.
- Useful patterns: shadow-header include ordering, avoiding CRT alias
  collisions, runtime DLL checks, build diagnostics, and mapped-file prefetch.
- Compare its compatibility layer with the pinned pixmaate revision; record
  any downstream fixes selected rather than silently mixing sources.
- bubo targets Windows x64/AMD HIP, not Windows ARM64/CUDA. Its pthread header
  is a stub, so it does not solve ds4's threading requirements.
- bubo is a gufo derivative with an adapted ds4 model package, not a full
  Windows port of ds4. That package excludes standalone SSD streaming and
  several other ds4 paths. Do not treat it as proof those paths work.

### 4. vllm-for-rtx-spark: Windows ARM64 CUDA toolchain reference

- Repository: https://github.com/devjeonghwan/vllm-for-rtx-spark
- Local source: `C:/Users/dwall/repos/vllm-for-rtx-spark`
- Installed distribution: `C:/Users/dwall/vllm-rtx-spark` (a separate tree).
- Inspect build scripts, development documentation, native-build/runtime
  patches, and runtime checks.
- Useful patterns: identifying the supported Windows ARM64 CUDA toolchain,
  host compiler setup, architecture checks, DLL resolution, QPC timing, and
  Windows commit-limit diagnostics.
- Its working binaries prove the runtime worked here, not that the CUDA
  development toolkit is installed. Do not copy Linux or x64 library paths
  into an ARM64 build without verifying the actual libraries.
- Apache-2.0: preserve applicable license/notice obligations for copied code.

### 5. DS4 GB10/GX10 fork and Athena: secondary references only

- Local: `C:/Users/dwall/repos/DS4-GB10-GX10-DSpark-CUDA`
- Upstream: https://github.com/xangel82/DS4-GB10-GX10-DSpark-CUDA
- Useful for historical CUDA optimization and DSpark implementation context.
  It is not a Windows portability layer and is no longer actively updated.
- `Athena-Engine` and `Athena-Logos` are also cloned locally. Athena Engine's
  public repository describes binary distribution under its own license.
  Do not assume its engine source is available or reusable under the old
  fork's MIT license. No Athena implementation import is currently planned.

## Concrete work sequence

### Phase 0 — Preserve a clean starting point

- [x] Record the current branch, commit, status, and remote layout.
- [x] Inspect fixture EOL behavior against Git blobs; correct the initial
      attributes commit if needed without changing reference payload bytes.
- [x] Keep build products, downloaded models, and large logs outside Git.
- [x] Record imported revisions and local modifications in a provenance file.
- [ ] Push `windows-dev` to the fork when ready; do not push to upstream.

Periodically integrate upstream changes at known-good checkpoints. Avoid
mandatory daily rebases through broken work. Preserve milestone commits/tags
and use coordinated force-with-lease only if rewriting an already-pushed branch.

### Phase 1 — Prove the toolchain before porting the engine

- [x] Initialize the ARM64 Visual Studio developer environment in a child
      process, without changing machine-wide PATH.
- [x] Build and run a tiny C program using the installed native MSVC tools.
- [x] Inspect its PE machine type: ARM64 is `0xAA64`; reject accidental x64.
- [x] Identify supported Clang and CUDA toolkit packages from the RTX Spark
      build documentation, including compiler/toolkit version constraints.
- [x] Obtain approval before installing missing tools or changing system config.
- [x] Build the same C smoke test with clang-cl targeting Windows ARM64.
- [x] Compile and run a minimal CUDA kernel, check device discovery, kernel
      output, and error handling using the intended `sm_121a` configuration.
- [x] Verify ARM64 import libraries and runtime DLLs; distinguish host tool
      architecture from the architecture of produced binaries.

**Exit:** native ARM64 C and CUDA smoke tests pass, with reproducible commands.
CUDA setup can proceed alongside the CPU compatibility work if necessary.

### Phase 2 — Vendor and test the compatibility layer

- [x] Resolve the temporary C++ vendor exception described above.
- [x] Verify and import pixmaate sources with licenses and a pinned revision.
- [x] Compare bubo's downstream fixes and select only justified additions.
- [x] Audit shadow headers for compatibility with both C and CUDA translation
      units, the CRT, Winsock, and ARM64.
- [x] Supply the pthread subset ds4 actually uses; evaluate maintained ARM64
      options versus a small Win32-backed shim. Do not assume pthreadVC3 works.
- [x] Add model-independent tests for:
      - concurrent positional reads without shared-file-position races;
      - short reads, EOF, large offsets, write failures, and error propagation;
      - read-only mappings, anonymous allocations, alignment, and unmapping;
      - mutexes, condition variables, once initialization, join, and shutdown;
      - monotonic timing and timeout behavior;
      - file locking and descriptor ownership.
- [x] Make unsupported semantics fail explicitly instead of silently succeeding.

Do not assume `VirtualUnlock` guarantees Linux-style cache eviction, or that
zeroing memory implements every aspect of `MADV_DONTNEED`. Prefetch and direct
I/O flags are experiments until measured. Unbuffered I/O needs correct buffer,
offset, and length alignment; OVERLAPPED I/O needs correct completion handling.

**Exit:** isolated compatibility tests pass repeatedly on ARM64 Windows.

### Phase 3 — Minimal Windows CPU build

- [x] Add a Windows-specific build entry point without replacing the existing
      Unix Makefile. Choose the smallest maintainable integration after the
      toolchain tests, rather than committing to a CMake rewrite.
- [x] Compile ds4's C sources as C, initially with `DS4_NO_GPU`.
- [x] Resolve POSIX includes and platform assumptions incrementally.
- [x] Keep optional distributed/RDMA facilities unavailable on Windows initially,
      with explicit diagnostics; retain their existing non-Windows behavior.
- [x] Run startup/help checks and suitable model-independent tests.
- [x] Exercise tokenizer, prompt rendering, GGUF parsing, and KV serialization
      wherever these can be isolated from full-model execution.
- [ ] Check that the existing Linux CPU build still compiles, using a suitable
      Linux environment when available.

**Exit:** a reproducible Windows CPU build and a documented passing test subset.
Full 81 GiB CPU inference is not an exit requirement.

### Phase 4 — Model acquisition and native CUDA inference

- [x] Use the upstream `ds4f-q2` target, currently documented as approximately
      81 GiB. Resolve the exact current filename/revision before downloading.
- [x] Download to `C:/models`; verify size and any published integrity metadata.
      Record model identity so numerical comparisons use identical weights.
- [x] Adapt CUDA build/link steps to verified ARM64 toolkit paths and libraries.
- [x] Port file/path and platform dependencies in CUDA helper code without
      changing numerical kernels unless necessary.
- [x] Load the model with conservative context/buffer settings; monitor both
      GPU allocations and Windows system commit.
- [x] Run short deterministic generations and the relevant upstream numerical
      and official-continuation tests.
- [x] Compare with a known-good build on matching model/settings. Use existing
      numerical tolerances, not an unsupported promise of byte-identical
      output across compilers or CPU/GPU backends. The checked-in 0731 Q2 Metal
      golden reference passed; no fresh Metal GPU run has been performed.

**Exit:** verified native CUDA inference and recorded correctness results.

### Phase 5 — SSD streaming

- [x] Port hot positional-read and file-reopen paths, including Linux-specific
      `/proc/self/fd` assumptions where they occur.
- [x] Validate staging-buffer lifetime, alignment, partial reads, prefetch,
      cancellation, and overlap with GPU work.
- [ ] Compare cached random reads and appropriately aligned unbuffered reads.
      Treat bubo's `O_CONCURRENT_RANDOM` as a candidate, not a proven winner here.
- [ ] Test explicit low expert-cache budgets so streaming is exercised even if
      much of the chosen model fits in reserved GPU memory.
- [ ] Measure memory, disk traffic, prefill, and decode across context sizes.
- [ ] Check output correctness against the non-streaming path.

**Exit:** correct, bounded-memory streaming with measured performance. Set
performance targets from actual baselines, not an arbitrary WSL-relative limit.

### Phase 6 — Server, CLI, and agent

- [x] Port socket/IPC behavior, including socketpair and any Unix socket use.
- [x] Validate Winsock initialization, socket handle widths, polling, errors,
      shutdown, and cancellation.
- [x] Port process spawning, quoting, environment handling, inherited handles,
      child cleanup, and exit-status reporting.
- [x] Add Windows console handling for the interactive CLI.
- [ ] Test instance locks, KV persistence, restart/resume, API streaming, tool
      calls, and intentional client disconnects.
- [ ] Port and run the applicable server/agent tests; document exclusions.

**Exit:** usable local CLI/server/agent with reliable process cleanup.

### Phase 7 — Stabilize, trim, and prepare upstream changes

- [ ] Run repeated inference, streaming, and server soak tests.
- [x] Add reproducible build instructions and a small environment diagnostic.
- [ ] Remove unused compatibility code and experimental flags only after tests
      protect the behavior being retained.
- [ ] Resolve the no-C++ requirement for the eventual upstream implementation.
- [x] Preserve all applicable third-party notices; trimming does not remove
      attribution obligations for retained derived code.
- [ ] Obtain Linux/macOS backend checks and before/after benchmarks as required
      by `CONTRIBUTING.md`; clearly label anything not tested.
- [ ] Ask before running distributed tests or changing shared GPU services.
- [ ] Discuss scope with upstream, then extract reviewable PRs from the working
      fork. Keep platform support and performance experiments separable.

## Linux baseline and WSL policy

A Linux reference environment is useful, but WSL CUDA is not a prerequisite
for native Windows development. Verify Windows ARM64/N1X GPU passthrough and
appropriate Linux toolkit support before relying on it. Native Windows CUDA
working does not establish that WSL CUDA works.

Any suitable ARM64 Linux distribution can provide CPU build checks. Ubuntu
is a convenience choice, not a ds4 requirement. A remote Linux CUDA system
can supply correctness references if local WSL GPU access is unavailable.
Performance comparisons across different hardware must be labeled accordingly.
For WSL disk benchmarks use its native filesystem, not `/mnt/c`, to avoid
confounding results with cross-filesystem access overhead. Do not duplicate
large models into WSL until that test route is justified.

## Evidence to retain at each milestone

- Source and dependency revisions, compiler/SDK/toolkit versions, build command.
- Model identity, context/cache settings, driver and memory configuration.
- Tests run, pass/fail results, known exclusions, and reproduction commands.
- Relevant benchmark CSVs and resource measurements for hot-path changes.
- A known-good commit or tag before integrating upstream changes.

Native milestone: ARM64 CPU frontends, compatibility regressions, CUDA
synthetic numerical tests, official continuations, and the frozen Metal golden
comparison pass. Commands, versions, resource measurements, approval decisions,
and the fixture-byte audit are recorded in [WINDOWS.md](WINDOWS.md).
Remaining live lifecycle, benchmark, soak, and upstream gates stay unchecked.
