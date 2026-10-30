# Native Windows ARM64 development

Experimental native Windows ARM64 support is developed on `windows-dev`.
CPU frontends, native CUDA inference, and SSD streaming have passed the checks
below. Live frontend lifecycle and longer soak checks remain separate gates in
[WINDOWS_PORT_PLAN.md](WINDOWS_PORT_PLAN.md).

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

Native memory streams operate directly on caller buffers; session serialization
does not introduce disk-backed snapshot copies. Distributed, tensor-parallel,
RDMA, and multi-GPU configurations fail explicitly before loading a model.

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
or fallback. The short direct run reported activation without fallback.

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
