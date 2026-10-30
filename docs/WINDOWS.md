# Native Windows ARM64 development

Experimental native Windows ARM64 support is developed on `windows-dev`.
All five CPU/CUDA frontends build. Native inference, bounded SSD streaming,
CLI/server/agent lifecycle checks, and the eval TUI have passed the checks below.
The initial validated model is DeepSeek V4 Flash. Remaining upstream gates and
unverified backends are recorded in [WINDOWS_PORT_PLAN.md](WINDOWS_PORT_PLAN.md).

Full-model inference is validated for the pinned Flash model below. Native
Qwen/Engram disk-tail loading and Qwen CUDA kernels are covered separately;
these checks alone do not establish full-model inference for another model.
Distributed, RDMA, and multi-GPU Windows execution remain explicitly unavailable.

## Toolchain

The native C and CUDA smoke checks have passed on RTX Spark N1X with driver
617.69. Both MSVC and clang-cl produced ARM64 PE machine `0xAA64` executables.
The CUDA check compiled `sm_121a`, discovered compute capability 12.1, checked
all kernel result values, and verified invalid-device error propagation.

Validated development components:

- Visual Studio Community 2026, MSVC tools directory `14.51.36231`, native
  `HostARM64/ARM64` compiler and linker.
- Windows SDK `10.0.26100.0`, ARM64 UCRT and Windows libraries.
- LLVM `23.1.4`, official `aarch64-pc-windows-msvc` portable distribution.
- CUDA SDK meta-package `13.4.3`: NVCC/CRT/runtime/NVVM `13.4.92`,
  cuBLAS `13.8.0.4`, CCCL `13.3.4.3.1`.

No elevated installation, machine-wide PATH change, driver update, reboot,
pagefile change, or GPU reservation change was needed. The Python build entry
initializes `vcvarsall.bat arm64` in a child process and verifies executable
architecture after linking. Existing Unix Makefile targets are unchanged.

The CPU, CUDA, and compatibility targets embed
`tools/windows/ds4.manifest`, selecting the process UTF-8 active code page.
Without it, the UCRT narrowed Unicode command-line arguments through system
ACP 1252 before `main`, turning an existing lambda-named directory into `?`.
The manifest preserves UTF-8 argv/environment without changing the machine
locale. Native spawned-child regressions include BMP and supplementary Unicode.

Install the official portable LLVM archive under `%LOCALAPPDATA%/ds4-tools`,
or pass its compiler path through `--clang`. The validated archive is:

<https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/clang%2Bllvm-23.1.4-aarch64-pc-windows-msvc.tar.zst>

SHA-256: `6a1f1a4eaa18c6b83d0c61c85813664143643fa7df3c11f49750773d560a2164`.
Windows' bundled tar rejected its large Zstandard decoding window; extraction
with Python's `compression.zstd` and `window_log_max=31` succeeded.

Acquire only the needed CUDA components into a user-owned directory:

```powershell
python -m pip install --only-binary=:all: --target "$env:LOCALAPPDATA/ds4-tools/cuda-13.4.92" "cuda-toolkit[nvcc,cudart,crt,nvvm,cublas,cccl]==13.4.3"
python tools/windows/build.py smoke
python tools/windows/build.py cuda-smoke
```

`--vs` selects a Visual Studio installation; otherwise `vswhere` locates it.
`--cuda` selects the toolkit root containing `bin/nvcc.exe`, `include`, and
`lib/arm64`; the default is the installed package's `nvidia/cu13` directory.
Build products remain under ignored `build/windows-arm64`. Do not add binary
outputs or model weights to Git. Keep vLLM stopped during large-model checks.

The CUDA engine uses `-gencode arch=compute_121a,code=sm_121a`, matching
`make cuda-spark`, rather than `-arch=sm_121a`'s additional generic PTX target:
the latter cannot assemble the engine's architecture-specific block-scale MMA.
CCCL requires the MSVC host flag `/Zc:preprocessor`. The builder generates the
missing ARM64 cuBLAS import library from the shipped DLL's exports and places
the three required NVIDIA runtime DLLs beside the executables. Executables
reserve an 8 MiB stack, matching the usual Linux stack allowance; existing CUDA
tests exceed Windows' default 1 MiB. Reservation is not eager commitment.

## Verified engine checks

All five `DS4_NO_GPU` frontends build and pass `--help`. The CPU CLI also parsed
the full GGUF and rendered/tokenized a chat prompt with `--dump-tokens`, without
attempting full-model CPU inference. Reproduce the model-independent checks:

```powershell
python tools/windows/build.py compat
python tools/windows/build.py cpu --program ds4 --program ds4-server --program ds4-bench --program ds4-eval --program ds4-agent --test ds4_test --test ds4_agent_test --test test_session_state --test test_windows_unsupported
build/windows-arm64/cpu/ds4_test.exe --server
build/windows-arm64/cpu/ds4_agent_test.exe
build/windows-arm64/cpu/test_session_state.exe
build/windows-arm64/cpu/test_windows_unsupported.exe
```

The clean build and all these tests passed. Compatibility coverage includes
UTF-8 paths, direct memory streams, patterns, positional I/O, descriptor
inheritance, sockets, process cleanup, pthread publication/join/detach, and
resolver-first Winsock initialization. Poll deadlines use the monotonic
performance counter, not the coarse system tick count that could expire early.
Console tests passed Unicode editing, repeated surrogate pairs, resize,
viewport-relative ANSI output, redirected cooked Unicode input, private history
ACLs, and mode/code-page restoration.

Frontend startup establishes UTF-8/VT output for the whole process, not only
while linenoise is reading a prompt. This fixes garbled Unicode diagnostics and
literal ANSI escapes on a CP437/VT-disabled console. Normal exit flushes buffered
output before restoring console settings; handled cancellation keeps the live
console configured. Redirected byte streams remain unchanged.

The compatibility `sigaction` adapter preserves process-wide handlers across
repeated delivery; raw UCRT `signal` handlers are otherwise one-shot. Regression
tests cover repeated SIGINT, ignore/restore transitions, and shared abort aliases.

Native memory streams operate directly on caller buffers; session serialization
does not introduce disk-backed snapshot copies. Distributed, tensor-parallel,
RDMA, and multi-GPU configurations fail explicitly before loading a model.

Windows cannot partially unmap a file view. The model loader now replaces
Qwen/Engram's initial view with a read-only resident prefix, rebasing its borrowed
metadata before releasing the original view. N-gram/Engram payloads stay on disk;
the general compatibility `munmap` still rejects partial releases. A synthetic
GGUF regression checks metadata, disk rows, mapping extents, and resource cleanup:

```powershell
python tools/windows/build.py cpu --test test_model_disk_tail
build/windows-arm64/cpu/test_model_disk_tail.exe
```

The upstream Qwen CUDA reference kernels also pass on N1X. Build/run them with:

```powershell
python tools/windows/build.py cuda --test test_qwen4_cuda
build/windows-arm64/cuda/test_qwen4_cuda.exe
```

## CUDA correctness and resource checks

Build all frontends with `python tools/windows/build.py cuda`. Add repeatable
`--test` options to build `ds4_test`, `test_cuda_tokentile`,
`test_cuda_ssd_cache`, `test_cuda_q8_rows`, and `test_cuda_compressor`.
Numerical kernels and upstream tolerances are unchanged.

- Synthetic attention/tokentile, SSD cache/prefetch/cancellation/teardown,
  Q8 rows/slices, and compressor tests passed; compressor: 0/3,636 failures.
- Greedy native resident, buffered-streaming, and aligned-direct-streaming
  runs all answered the same arithmetic prompt with `4`.
- `ds4_test --logprob-vectors` passed all five official continuation cases
  with an 8 GiB streaming expert cache.
- `ds4_test --local-golden-vectors` passed the frozen 0731 Q2 Metal reference
  at 4,096 tokens: matching top-1, top-5 overlap 5/5, top-20 overlap 18/20,
  top-64 overlap 55/64, top-20 maximum absolute logit difference 2.19098.
  This is a checked-in reference comparison, not a newly run Metal baseline.

For model tests, set `DS4_TEST_MODEL` to the full model path,
`DS4_TEST_SSD_STREAMING=1`, and `DS4_TEST_SSD_STREAMING_CACHE_GB=8`.
Run from the byte-faithful fixture root described below.

`tools/windows/measure.py --out NEW_DIRECTORY -- COMMAND ARGUMENTS` captures
stdout/stderr, one-second samples, and a summary. Repeat `--env NAME=VALUE`
for child-only overrides. Measurements distinguish process private bytes and
logical I/O from system commit, device-wide GPU usage, and machine-wide physical
disk reads. They are not exclusive process attribution or cold-cache benchmarks.

On the 96 GiB GPU reservation, the first resident smoke peaked at 120.4 GiB
system commit against a 126.5 GiB limit. Short 8 GiB streaming runs peaked near
40.7 GiB commit; the full official suite peaked near 46.2 GiB. Keep contexts
conservative and monitor commit, not only the nominal GPU free-memory figure.
No pagefile or reservation changes were made.

Direct streaming reopens the existing native file handle with aligned
unbuffered I/O instead of using `/proc/self/fd`. Staging uses 4,096-byte
alignment on this machine. `DS4_CUDA_NO_DIRECT_IO=1` selects the buffered
comparison path; `DS4_CUDA_WEIGHT_CACHE_VERBOSE=1` reports direct activation
or fallback. All measured direct runs reported activation without fallback.

## Measured streaming sweep

The same model and byte-faithful story input were run in this order: resident,
buffered 8 GiB, direct 8 GiB, direct 4 GiB, and direct 8 GiB again. Each run used
`--ctx-start 128 --ctx-max 2048 --step-mul 4 --ctx-alloc 2065`,
`--prefill-chunk 128 --gen-tokens 16 --teacher-forced-decode`.
Verbose cache logging was enabled throughout. Cache sizes below are expert-cache
targets, not total GPU budgets. No administrative cache purge was performed;
fixed run order and warm-cache effects prevent treating this as a cold-I/O study.

| Mode | Frontier tokens | Prefill tokens/s | Decode tokens/s |
|---|---:|---:|---:|
| Resident | 128 | 27.99 | 17.94 |
| Resident | 512 | 182.47 | 18.61 |
| Resident | 2048 | 164.72 | 17.85 |
| Buffered 8 GiB | 128 | 5.64 | 2.09 |
| Buffered 8 GiB | 512 | 6.36 | 2.18 |
| Buffered 8 GiB | 2048 | 5.66 | 1.59 |
| Direct 8 GiB | 128 | 8.71 | 2.38 |
| Direct 8 GiB | 512 | 9.96 | 2.37 |
| Direct 8 GiB | 2048 | 9.81 | 1.85 |
| Direct 4 GiB | 128 | 9.03 | 1.49 |
| Direct 4 GiB | 512 | 11.42 | 1.45 |
| Direct 4 GiB | 2048 | 10.54 | 1.49 |
| Direct 8 GiB repeat | 128 | 9.08 | 2.38 |
| Direct 8 GiB repeat | 512 | 10.25 | 2.48 |
| Direct 8 GiB repeat | 2048 | 10.11 | 1.89 |

Memory peaks are sampled once per second. Logical reads belong to the process;
GPU/commit counters and physical disk read rates include other processes.

| Mode | Wall seconds | Private GiB | System commit GiB | GPU GiB | Logical reads GiB | Peak physical reads GiB/s |
|---|---:|---:|---:|---:|---:|---:|
| Resident | 50.21 | 97.72 | 120.85 | 95.81 | 87.48 | 6.44 |
| Buffered 8 GiB | 382.33 | 20.39 | 43.34 | 21.46 | 793.70 | 2.52 |
| Direct 8 GiB | 237.24 | 20.39 | 43.36 | 21.46 | 794.44 | 3.98 |
| Direct 4 GiB | 229.23 | 17.35 | 40.48 | 18.43 | 826.61 | 4.20 |
| Direct 8 GiB repeat | 228.22 | 20.39 | 43.73 | 21.47 | 794.44 | 4.14 |

The 4 GiB case deliberately exercised the cache-pressure warning: 94 dynamic
expert slots versus a 258-expert per-token routed working set. Its lower decode
rate is not a reason to prefer that budget. The 128-token chunk is deliberately
small; these figures are not a claim of maximum achievable prefill throughput.

All 129,280 logits were finite at every frontier. Buffered/direct, 4/8 GiB,
and repeated streaming runs had exactly equal serialized logit values.
Against resident execution, every comparison retained the same top-1 and
top-5/top-20 sets; worst full-vocabulary absolute difference was 0.0136547.
No upstream tolerance was loosened.

Raw CSV stdout, stderr, samples, summaries, and frontier logits remain under
ignored `build/windows-results/benchmark-*` and `benchmark-logits`.
The benchmark uses the numerical implementation published at `410d90b`.
Reproduce a direct-streaming run after setting `DS4_TEST_MODEL`:

```powershell
python tools/windows/measure.py --out build/windows-results/new-direct-run --env DS4_CUDA_WEIGHT_CACHE_VERBOSE=1 -- build/windows-arm64/cuda/ds4-bench.exe -m $env:DS4_TEST_MODEL --cuda --prompt-file build/windows-fixtures/tests/long_context_story_prompt.txt --ctx-start 128 --ctx-max 2048 --step-mul 4 --ctx-alloc 2065 --prefill-chunk 128 --gen-tokens 16 --teacher-forced-decode --ssd-streaming --ssd-streaming-cache-experts 8GB
```

Add `--env DS4_CUDA_NO_DIRECT_IO=1` before `--` for buffered I/O. Omit the
streaming options for resident execution; watch the small remaining commit/GPU
headroom. Use a new measurement directory for every run.

## Live frontend acceptance

- **Server:** three loopback lifecycles passed nonstreaming chat, complete SSE,
  genuine tool-call/result consumption, active client disconnect/recovery,
  and concurrent engine-lock refusal. Native CtrlBreak drained and exited zero
  each time. Unicode/space KV directories persisted shutdown checkpoints;
  both restarts loaded those files, incremented their hit counters, reported
  215 cached tokens, and recalled the stored conversation fact.
- **CLI:** two fresh real-console runs edited and generated `café 中雪`
  identically with greedy sampling/seed 1337, exercised Unicode history,
  exited normally, and restored the original CP437/VT-disabled modes.
- **Eval:** the real split TUI rendered its paused score/case panels, accepted
  Down/Up selection and `q`, and restored the original screen and console modes.
- **Agent:** a real model-driven read/write/bash loop ran in a lambda-named
  working directory, copied a randomized 38-byte file exactly, passed shell
  `cmp`, emitted the shell success marker, and returned `DONE`.
- **Browser:** Edge `156.0.4314.50` passed both automatic discovery and the
  `DS4_CHROME` override. Native CDP extracted a randomized local-page marker,
  reused the session after freeing the web handle, and kept the browser alive
  after the caller exited. Both isolated Unicode-path profiles were closed
  through CDP; personal profiles and unrelated browsers were not used.

Model-backed live checks used 8 GiB streaming caches. CLI/eval used context 1024; server used
2048; the agent used 8192 with prefill chunk 2048. These are bounded lifecycle
and repeatability checks, not a 24-hour endurance certification. Evidence remains
under `build/windows-results/{server-live,terminal-live-bmp,eval-live-calibrated,agent-live}`.
Browser reports are in `build/windows-results/browser-live` and
`build/windows-results/browser-live-auto`; those checks require no model/GPU.

Legacy console capture has two observed limits on this host: supplementary
glyphs become replacement characters even in a direct Win32 write/read probe,
and `GetConsoleCursorInfo` does not reflect VT cursor-hide requests. Visual
checks therefore used BMP Unicode; supplementary input/argv bytes are covered
by native regressions. Neither capture limitation was patched around in ds4.

Git Bash supplies the agent's external `bash` tool; set `DS4_SHELL` to its full
path when discovery is insufficient. The engine itself does not run under MSYS.
Browser-backed tools discover Chrome, Chromium, or Edge; `DS4_CHROME` selects an
explicit executable. Browser startup still requires the agent's normal approval.

## Model identity

Downloaded to `C:/models` from
[`antirez/deepseek-v4-gguf`](https://huggingface.co/antirez/deepseek-v4-gguf/tree/f71f23d552d664e523b422157b2befbf74040380),
revision `f71f23d552d664e523b422157b2befbf74040380`:

`DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf`

- Size: **86,720,111,488 bytes**, matching the pinned file metadata.
- SHA-256: `ca22ae2f838e14077c22bc1c1417b71b45b5e5a3687bd96c2ac6e17fdb6261c0`,
  verified with `certutil -hashfile FILE SHA256`, matching the published LFS hash.
- Download command: `hf download antirez/deepseek-v4-gguf FILENAME --revision f71f23d552d664e523b422157b2befbf74040380 --local-dir C:/models --max-workers 2`.

## Checkout and provenance

Development started from `d34c3fdd43a375c3674ff5b2151aa1a520d9e119`, parent
`ee7e8405232c8971000cc7ba2b945b00bf6d20d3`. The initial branch was
`windows-dev`, tracking `origin/main`, ahead one commit. Only the untracked
Windows plan appeared in initial status. Remotes:

- `origin`: <https://github.com/antirez/ds4.git> (do not push here).
- `fork`: <https://github.com/dwalleck/ds4.git> (development publication target).

The first tested native milestone, `410d90bc9bd3f73003c1ce3657595d6a86c50260`,
is published to `fork/windows-dev`. Its
[Linux/macOS CI run](https://github.com/dwalleck/ds4/actions/runs/41011420079)
passed the existing CPU builds, all frontend help paths, session serialization,
GPU-argument/prompt-prefix regressions, and model-independent server/agent tests.
These are CPU/host checks, not fresh Linux CUDA or macOS Metal inference runs.

The user explicitly approved the temporary vendored C++ compatibility exception
and user-local tool acquisition, with CUDA installation permitted if necessary.
The exception does not permit converting ds4-owned C sources to C++. Vendored
source revisions, selected downstream fixes, and notices belong in
`compat/win32/VENDOR.md`; an upstream-ready implementation must resolve the
project's no-C++ requirement before submission.

The RTX Spark build reference was
`devjeonghwan/vllm-for-rtx-spark@46fde037283ad049f721cd10b97be3f3b7d7cbc1`.
Its native build manifest pins CUDA `13.4.92` and `sm_121a`; no reference engine
or runtime implementation has been copied from it.

## Fixture-byte warning

An initial raw Git-blob audit covered 3,057 payloads. All payload blobs were
unchanged by the initial attributes commit. However, 1,722 working files already
had LF-to-CRLF checkout expansion (4,872,717 extra bytes); no non-EOL difference
was found. Those payload files were left untouched. Corrected `.gitattributes`
now also protects the sibling DeepSeek collections, prompt JSONL, long-context
prompts, vision JSON, and benchmark corpus.

Attributes do not retroactively repair existing working files. Do not bulk-stage
or renormalize scoring data. Numerical verification must use byte-faithful
reference inputs extracted from the recorded Git revision, not those stale CRLF
working copies.

For native numerical runs, `git archive` extracted `tests/test-vectors`, both
long-context prompts, and `speed-bench/promessi_sposi.txt` from the initial
revision into ignored `build/windows-fixtures`. The extracted story prompt is
140,309 bytes with SHA-256
`29363eab21bbbccaeea8e13f669e7ce05e8eafc48e31fcf9b725edabb2058666`;
the long-code-audit prompt is 18,851 bytes with SHA-256
`efae39172d088e3b8da70f408e53eeebe0f5b17fa407a440ed969cd85e4784dd`.
Both match the raw-blob audit; the working payloads remain untouched.
