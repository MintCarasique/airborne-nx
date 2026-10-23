## Latest: visible intro on Switch

Four native movie draws confirmed in f4bac5364158; user reports visible but slow intro. Performance changes await hardware measurement. See [checkpoint](intro_performance.md).

# Airborne Xbox 360 — initial input audit

Latest measured run: real VI event waiting works (513 successes), but the title
does not reach file loading within 120 seconds. The main thread progresses
slowly and is sampled in a native mutex wait. See [startup contention](startup_contention.md)
and `startup_contention_progress.json`. The current stable build removes shared
service-lock contention from per-thread TLS reads/writes and buffers ordinary
SD logging while preserving immediate failure flushes. Local contention/log
failure checks and original Windows entry pass; target speed is not established.

Latest hardware log: default-display borrowing and vsync-event acquisition work,
but a manual eventClear on the autoclear event stops startup before movie/file
loading. See [SDK event contract correction](vblank_event_contract.md) and
`vblank_event_progress.json`. The measured build is archived; the current stable
airborne-nx build removes the manual reset and uses SDK eventWait. Its actual
linked wait/reset/retry/cleanup path was inspected; new target execution pending.

Hardware update: [0.7 display failure and 0.7.1 correction](v07_display_failure.md).
The supplied 0.7 log stops on VI default-display reopening before any game file
opens/movie draws. One black initialization frame was queued; no intro was seen.
0.7.1 borrows the libnx-owned display, passes local lifetime/link checks, and
reuses the existing content directory. Its target execution remains pending.

Latest development (2026-10-23): [native movie rendering 0.7](movie_rendering.md).
Windows now renders/presents actual decoded intro frames through Vulkan; a new
Switch NRO and complete local SD folder are ready, with target execution pending.
Current stop is XMA context control. Menu/gameplay are not demonstrated. Earlier
boundaries below are historical; measured 0.6 hardware evidence remains intact.

Date: 2026-10-22

Current graphics milestone: the full Airborne ARM64 runtime now links with a
locally built Horizon NVK Vulkan driver. The shader pipeline compiles and validates
969 real shader containers; 67 embedded WMV shaders remain unsupported. Windows
guest startup creates real Vulkan resources and reaches ring initialization.
There is no game frame or Switch Vulkan execution evidence yet. See
[Vulkan implementation and remaining work](vulkan_port.md),
`graphics_progress.json` and the detailed build/shader reports. The hardware-tested
v0.4 package and its result remain unchanged; no new hardware probe was packaged.

## Current port status

The matching v0.3 user hardware log passes all included startup-prerequisite
phases. See the hardware evidence section below. Full game startup/graphics/audio
are not implemented on Switch. All 317 generated CPU units now compile with devkitA64 and link together with
libnx into an AArch64 ELF. Xbox imports in this link check abort explicitly; the
ELF has not been executed. The shared startup runtime now compiles and links as v0.4 and attempts the real
game entry, and the supplied v0.4 log now confirms the graphics-service boundary on Switch.
The user also reported a post-result exit failure; see the evidence section.
No repeat of v0.3 or unchanged v0.4 is required.

## Result

Extracted 1,244 files (6,342,775,234 bytes) into `../extracted/base/` directly from the supplied GOD container. Inputs and the separate Title Update were not modified. No ISO was generated. The upstream god2xex extractor was inspected but not executed; extraction uses `../tools/extract_god.py`.

Every extracted file's length and SHA-256 was rechecked after writing. All 1,548,581 source data blocks match their stored SHA-1 hashes; all 7,592 group hash tables match their entries in the part master tables. No mismatches. This is an internal integrity check, not verification of Microsoft's signature, the master-table chain, or equivalence to an original retail dump.

## Executable

- File: `../extracted/base/default.xex`
- Format: XEX2; 6,270,976 bytes
- SHA-256: `8050b225e8c4ec7dfeb5aea23cce3e4123163480d243b45c7b3f3da6f05d98e6`
- Title ID: `454107F6`; Media ID: `5E593EE0`
- XEX execution version and base version: `0.0.0.2` (raw `0x00000002`; not a marketing patch number)
- Original PE name: `MOHAGame-XeFinal.pe`
- Image base: `0x82000000`; entry point: `0x82C5C310`
- Image size recorded in security header: 20,054,016 bytes
- Encryption: normal (1); compression: normal/LZX (2)
- Import libraries: `xboxkrnl.exe` (316 records), `xam.xex` (228 records). Records include import slots/thunks; these counts are NOT counts of distinct functions.
- Static library metadata includes D3D9, D3DX9, XGRAPHC, XAUD, XMEDIA, XNET and XONLINE; version 2.0.5632.0.

The initial extraction audit is complete. Subsequent executable loading and trial recompilation are documented below.

## Assets and update

- 1,196 `.xxx` files under CookedXenon. 1,179 start with the big-endian Unreal package magic; 17 audio/music/stream files have different signatures and require separate format analysis.
- 38 `.wmv` files. 37 have the ASF header GUID. `MOHA_Shell_EAID_DEU.wmv` has a different header; its format/content requires investigation. Its source blocks pass the GOD integrity check.
- Coalesced.ini and Xbox360TOC.txt are not plain text at their starts; do not assume they can be edited as ordinary INI/TXT files.
- Title Update container has matching Title ID AND Media ID. This is preliminary compatibility evidence, not proof that its patch can be applied. It has not been extracted or applied.

## Next milestone

The user-supplied Switch CPU probe result passes. Extend target guest-memory support and the GPU backend from the current host initialization boundary. Preserve the base executable as the reference build. Assess TU contents independently before deciding which executable revision to target. A valid input dump does not yet establish that this game can run acceptably on Switch.

## Executable analysis and first C++ generation

Built XenonAnalyse and XenonRecomp from commit `ddd128bcca99fe8bfbb99bea583c972351fa6ace`, with their pinned submodules. Local toolchain: llvm-mingw 20261020 / Clang 23.1.4, CMake 4.4.4, Ninja 1.13.2. The old fmt dependency requires `-include cstdlib` with this toolchain. Local instruction and function-analysis changes are preserved in `../tools/patches/xenonrecomp-airborne.patch`.

- The loader successfully decrypted and LZX-decompressed the image (20,054,016 bytes), checking compressed-block hashes along the way.
- Parsed 16 PE sections. Main `.text`: `0x82280000`, size `0xD8EBAC`; additional code resides in `.embsec_` sections.
- `.pdata` is available for function boundary analysis.
- Resolved 267 named import thunks. See `image/import_symbols.tsv`.
- `image/image_loader_normalized.bin` is a mapped image **after** the upstream loader normalizes import records and replaces import thunks. It is not an untouched PE file. `recompile_summary.json` includes PE section metadata parsed from bounded 8-byte names; upstream section-name presentation can otherwise run into the following fields.
- XenonAnalyse detected 457 switch tables; two validated Airborne-specific supplements bring the current total to 459. Automatic tables remain heuristic results requiring runtime validation.
- All eight register helper signatures had a single match, with repeated register sequences inspected. Their addresses are saved in `../config/airborne.toml`. Optimizations remain disabled.
- Initial generation produced 319 C++ translation units. Current generation produces 317 units, totaling 258,363,755 bytes, in `../generated/ppc/`. The 80,879 generated function definitions include inferred boundaries and padding; this is not a verified count of original game functions. There are no duplicate generated definition names.

### Implemented instruction and boundary changes

Added translation for `vslh`, `vsrah`, `vspltish`, `vandc`, `vnor`, `vmaxsh`, `vminsh`, `vavguh`, `vsrh`, `vsrab`, and `vsububm`. These use the same reversed vector-byte representation as the upstream generator. Immediate splats explicitly sign-extend five bits, shifts mask their counts, and destination/source aliasing is covered by tests.

The instruction test generator calls the actual modified Recompiler and compiles/executes its emitted C++. A separate scalar oracle uses guest big-endian byte arrays, arithmetic division for signed shifts, and unsigned arithmetic for wraparound. All 32 splat immediates, source/destination alias combinations, boundary values and deterministic random inputs are tested. The expanded vector suite now passes 206,400 executions with native SIMD and another 206,400 with `SIMDE_NO_NATIVE`. Portable SIMD on x86 is not an ARM64 execution test.

Added a VSCR state field and a shared saturating-integer helper, which sets the sticky SAT bit and preserves existing register bits. Implemented `vsubshs`, `vaddsbs`, `vaddsws`, `vpkswss`, `vpkswus`, `vpkshss`, and `vpkuhus`; existing `vaddshs`, `vaddubs`, `vadduws`, `vsubsws`, `vsububs`, and `vpkshus` paths now also track SAT. Pack operations build their results in a temporary to preserve all input lanes when destination aliases a source. This does not establish complete VSCR behavior for every previously implemented instruction; in particular the existing signed FP conversion and other floating-point paths still require auditing.

Implemented `vcmpequh`, `vcmpgtsh`, `vcmpgtsw`, and `vcmpgtuw`, including dotted CR6 behavior, and fixed the missing CR6 update for existing `vcmpgtuh.`. Added `vrlh` and `vsel128`. Tests check all/none/mixed comparisons, unchanged CR6 for nondotted forms, SAT clear/set/preservation, aliasing, word/halfword/byte endpoints, and packing order.

Implemented `eqv`, `mulhd`, `cror`, and `crorc`. GPR tests use Python arbitrary-precision integer references (including signed multiply-high), and CR tests exercise every destination bit with source/destination overlap. 137,216 emitted scalar executions pass. Record forms of the new GPR operations compare the full signed 64-bit result and propagate XER.SO.

Implemented `vctuxs` and `vcfpuxws128` using binary32 bit decoding, so results do not depend on host rounding/flush modes or undefined out-of-range float casts. Tests use Python binary64 arithmetic as an independent reference for exact binary32 scaling and truncation, all 32 scales, normal/subnormal values, signed zeros, fractions, NaNs, infinities and saturation boundaries. VMX128 tests include registers above 31 and in-place operation. 76,800 conversion executions pass in each of native and portable modes. NaNs yield zero without setting SAT; infinities saturate; truncation precedes integer-range saturation. References: [AltiVec Programming Environments Manual](https://www.nxp.com/docs/en/reference-manual/ALTIVECPEM.pdf), [Xenia instruction decoding and semantics](https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/ppc/ppc_emit_altivec.cc), and [QEMU conversion behavior](https://github.com/qemu/qemu/blob/master/target/ppc/int_helper.c). No QEMU implementation code was copied.

The vector, scalar and conversion suites total 703,616 generated instruction executions. The `frsqrte` suite below adds 49,416, bringing that subtotal to 753,032. The FLOAT16_2 and BLRL tests below bring the current synthetic total to **3,034,862**, plus 512 FPSCR field-mask checks and six function-analysis scenarios. These are focused instruction tests, not a complete PPC conformance suite.

### Reciprocal square root and FPSCR

Implemented `frsqrte` and its dotted form using a high-accuracy estimate for positive finite inputs, explicit bit handling for zeros/infinities/NaNs, and local guest FPSCR exception/result state. [IBM's instruction reference](https://www.ibm.com/docs/ssw_aix_72/assembler/idalangref_frsqrte_instrs.html) specifies a relative-error bound of 1/32 and permits different estimates across implementations. This is an ISA-level estimate implementation, **not a bit-exact model of Xenon's estimate hardware**. The game follows the instruction with single-precision refinement; end-to-end numerical behavior still needs gameplay validation.

The handler quiets SNaNs, raises sticky ZX/VXSNAN/VXSQRT/FX, derives VX/FEX summaries, suppresses destination/FPRF updates for enabled invalid/zero-divide exceptions, classifies committed results in FPRF, and updates CR1 for dotted forms. FR/FI use zero for committed estimates. Guest exception delivery through MSR/OS mechanisms is not implemented. Other inherited floating-point arithmetic handlers still do not model every FPSCR flag.

MFFS now exposes local guest flags along with the host rounding mode. MTFSF observes its field mask. Tests uncovered incorrect rounding readback through the pinned SIMDe portable x86 interface; the context now reads/writes MXCSR directly on x86. ARM64 gets a distinct FPCR-to-PPC rounding table (up/down encodings differ from x86) and a correctly sized 64-bit operand for `msr fpcr`.

Generated-code tests cover every finite binary64 exponent, random mantissas/signs, subnormals, signed zeros, infinities, quiet/signaling NaNs, aliasing, dotted/non-dotted forms, enabled/disabled exceptions, sticky bits and all four rounding modes. Python Decimal at 80-digit precision supplies independent accuracy references. Both native and portable SIMD runs pass 24,708 emitted executions and 256 FPSCR mask checks each. A focused context/FPCR test also compiles to an AArch64 Windows object with static assertions for rounding mappings; this is cross-compilation, not ARM64 execution or a Switch binary.

### Recovered switches and first real game-function execution

`../tools/recover_airborne_switches.py` verifies the baseline XEX SHA-256, exact dispatch/guard opcodes and table contents before merging two supplements into `airborne_switch_tables_merged.toml`:

- `0x828B6D00` / BCTR `0x828B6D24`: twelve byte-offset entries. The two direct callers at `0x828B6F88` and `0x828B73A0` check unsigned R3 <= 11.
- `0x82CDD594` / BCTR `0x82CDD5B8`: sixteen halfword-offset entries. R24 is formed with a four-bit mask and checked <= 15 earlier in the same function; targets remain within its `.pdata` bounds.

The merge runs before every generation, including after a fresh XenonAnalyse run. Both former missing-switch diagnostics disappear. Tests execute the **verbatim generated implementation** of the actual game function `0x828B6D00` in isolation, with the loader-normalized game image mapped into a reserved guest address space. All twelve cases return the expected original guest string addresses, for 768 calls with varied initial state. The rest of the generated translation unit is excluded from this small link because its COFF weak aliases retain unrelated game functions and pull in external runtime dependencies. This is an actual host execution of a real recompiled leaf function, not game boot.

Function analysis now receives successors from existing switch tables, follows their reachable blocks, and preserves reachable case blocks across gaps. A set of known direct-call targets and `.pdata` entries prevents forward tail calls from being absorbed into the caller. The resulting function set is closed over direct calls/tail calls to preserve callable leaf entries and targets across code sections. Six synthetic scenarios pass, covering ordinary BCTR termination, switch successors, duplicate/invalid edges, nesting/self edges, end-of-buffer handling, and forward tail calls.

The original 1,536 boundary error lines involved 74 switch sites, none covered by `.pdata` (saved in `switch_boundary_audit.json`). Current generation emits zero such errors. This is diagnostic progress; function boundaries and the upstream switch-table heuristics still need runtime validation.

### FLOAT16_2, indirect calls and complete host CPU build

Implemented `blrl` with the previous LR as its aligned indirect target and the following guest PC as the new LR. 4,096 generated executions verify target alignment, callback-visible LR, unchanged CTR and callee state preservation.

Implemented the FLOAT16_2 variants used by this executable: `vpkd3d128` type 3 with mask 1 and insertion shifts 0..3; `vupkd3d128` type 3 expands the two halves from guest W into X/Y and sets Z=0, W=1. Packing uses rounding toward zero with finite overflow clamped to the largest finite half; bit-based helpers preserve signed zeros and quiet NaNs. Aliasing and VMX128 registers above 31 are covered. Other pack masks remain unsupported in the generator; none occur as explicit trap sites in this build. The inherited FLOAT16_4 path has not received this audit.

Native and portable modes each pass 1,138,867 emitted half-conversion executions. The tests enumerate all 65,536 half patterns, use an independent sorted-value packing reference, and include three upstream Xenia FLOAT16_2 golden vectors. References: [Xenia vector backend](https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/backend/x64/x64_seq_vector.cc), [pack vectors](https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/ppc/testing/instr_vpkd3d128.s), [unpack vectors](https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/ppc/testing/instr_vupkd3d128.s). This is software-reference validation, not physical Xenon hardware conformance.

All **317 generated translation units** now compile into x86_64 Windows objects and `generated/host/libAirbornePPC.a` (130,154,834 bytes). The complete archive links into a diagnostic executable. Running it verifies **81,146 unique guest mappings**, including 267 external import placeholders, without calling the game entry point. Each placeholder prints its missing import and aborts if called. These are not implementations of Xbox services.

Clang 23 emits a `roundevenf` libcall unavailable in the bundled UCRT. `runtime/host/roundeven.cpp` supplies nearest-even binary32 rounding using integer bits, independent of the current host rounding mode. 1,224,012 checks cover four rounding modes, random bit patterns, exponent boundaries, halfway values and neighbors, infinities, NaN bit preservation and signed zeros. Exception flags are not modeled by this host shim. These checks are separate from the PPC instruction-execution total.

`object_build_summary.json` and `host_link_summary.json` record results. The summary tool attaches them only when their generated-source fingerprint matches the current output. Object compilation uses `-O1`, disables strict aliasing and floating-point contraction, and uses the pinned toolchain/SIMDe dependency. This is a host CPU build and diagnostic link, not a Switch executable or game boot.

### Known blockers

1. Current generation reports **zero unsupported instructions, missing switch descriptions, switch-boundary errors, record-bit warnings, duplicate generated definitions and explicit runtime trap sites**. This establishes generation/build coverage, not full semantic correctness.
2. Existing condition-register/FPSCR/vector handlers still need broader auditing. The focused tests do not establish complete PPC or bit-exact Xenon conformance.
3. The host diagnostic implements 44 service handlers covering 41 of this executable's 267 function imports. Several are limited machine-profile or advisory handlers. The remaining imports stop explicitly. Full guest exceptions, asynchronous I/O, complete object lifetime and a GPU/audio backend remain unimplemented. Graphics, audio, ARM64 execution and Switch integration remain unvalidated.

**The complete game remains experimental and not runnable.** One actual game leaf function has executed in isolation on x86_64 (768 calls). A focused context test compiled to AArch64 Windows. The diagnostic entry execution described below reaches GPU engine initialization and stops at VdInitializeEngines. No completed game boot or execution on Switch has been performed.

### Host runtime: heap, threads and GPU initialization boundary

`../tools/run_host_probe.py` verifies the CPU archive checksum and generated-source fingerprint, builds the Windows x64 host diagnostic, validates its environment and runs the actual guest entry. The normalized image is pinned to SHA-256 `c9349a9389fbee7d2b838189df72433df901ef3918053241adbb3e6844961c48`; the original XEX retains its separate pin. `runtime_manifest.json` recovers and validates all 267 function slots/thunks and ten variable imports, the 262,144-byte default stack and 64 TLS slots with no initial template.

The runtime binds `XexExecutableModuleHandle` to its module/header record and `ExLoadedCommandLine` to the direct-launch command. Debug/cert monitors are disabled. Six remaining variable addresses stay guarded; `ExThreadObjectType` is recognized as an opaque identifier when passed to the object-reference service, without claiming to implement a full kernel type descriptor.

`../runtime/host/virtual_memory.h` tracks guest reservations independently of the host VA reservation. It supports the current 4 KiB/64 KiB virtual arenas, reserve/commit, recommit preserving data, decommit/release, top-down placement, bounds/alignment and protection changes. The original game heap now reserves 1 MiB, commits 64 KiB and allocates further pages as startup progresses. Reset, 16 MiB virtual pages, unsupported protection flags and kernel-wide address allocation policies are outside this implementation. Unsupported requests return failure. Partial commit failure rollback in an existing reservation remains a limitation.

`../runtime/host/address_space.h` provides shared physical backing for the A/C/E address windows. The E window has the documented 0x1000 physical bias, so scalar loads/stores, vector accesses, cache-zero operations, reservation/CAS paths and setjmp-pointer generation now use the common `ppc_guest_ptr` helper. Physical allocations share backing across aliases and enforce size, bounds, page alignment, non-overlap and protection. Cache-policy flags are accepted as hints; host CPU/GPU cache behavior and GPU synchronization are not emulated. Unallocated physical pages remain inaccessible. The CPU function table is read-only; normal image section permissions remain relaxed.

Host services now provide recursive critical sections, 64-slot TLS, initial PCR/KTHREAD fields, events with auto/manual-reset state, timed waits, native guest worker threads with private stacks/PCR/TLS, suspend/resume, thread object references, priority/virtual affinity, delays and system time. The process model remains deliberately partial: APC/alertable waits, complete scheduling semantics, true Xenon memory ordering/reservation behavior and complete object destruction are not established. Closed thread backing records remain retained until probe exit. Event state and service metadata are serialized; blocking operations release that lock. Worker failures stop the entire probe, and main-thread diagnostic stops terminate remaining workers before releasing guest memory.

Guest timebase generation now uses a monotonic **50 MHz diagnostic clock** rather than raw x86 TSC/ARM counter values. `KeQueryPerformanceFrequency` returns the matching frequency. The generated context prologue uses a GCC/Clang-compatible alignment assumption; devkitA64 GCC does not provide Clang's `__builtin_assume`.

Validation results:

- 65,583 virtual-memory checks, including recommit preservation, zeroed new pages, decommit/reuse, page protection and rejected ranges/flags.
- 812 physical allocator/alias checks using **actual emitted** scalar/vector memory instructions, cache-zero and reservation/CAS sequences across all three aliases.
- 1,273 host service checks covering TLS exhaustion/isolation, recursive critical sections, auto/manual-reset events, four suspended/resumed real guest leaf workers, thread exit results, string conversion, launch-data absence and timebase bounds/monotonicity.
- All twelve real leaf cases execute through the full linked function table; stack guards, import slots and missing-target behavior are checked. Thirteen bounded XEX-field checks and two negative input runs also pass.
- The complete focused PPC regression suite still passes: 3,034,862 synthetic emitted executions, 512 FPSCR mask checks, six function-analysis scenarios and 768 isolated real leaf calls. Host `roundevenf` has an additional 1,224,012 checks. None of these numbers represents full PPC conformance.

The actual startup creates **two guest worker threads**, traverses the game/CRT initialization code and allocates physical video memory. Current stop: **VdInitializeEngines**, return address **0x829C0788**. No fake GPU-success return is supplied. There is no rendering or completed game boot. `host_probe_entry.log` contains the full trace; `host_probe_summary.json` stores counts, excerpts and fingerprints. Thread scheduling can change trace ordering/counts between runs.

The diagnostic machine profile uses VGA/1280x720/60 Hz, direct launch with no launch-data, byte-wise ANSI widening, host-managed file cache hints, and the executable's own privilege flags. These are explicit partial host behaviors, not detected settings of the user's Switch. No game content has yet been opened by an implemented filesystem backend.

References for ABI/layout: [Xenia thread/PCR records](https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xthread.h), [virtual/physical memory mapping](https://github.com/xenia-project/xenia/blob/master/src/xenia/memory.cc), [kernel memory APIs](https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc), [module header semantics](https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/user_module.cc). Runtime implementations were written locally; reference source snapshots are saved for audit.

### First Switch homebrew CPU probe

A real **AArch64 libnx NRO** is built by `../tools/build_switch_probe.py` using devkitA64 GCC 15.2.0. The SDK was extracted locally from the official `devkitpro/devkita64` image, pinned to manifest `sha256:82575ea78651b530b2e232bb3799cfd1fe331514e053d5f724bb4b28191fb79d`; selected layer hashes are verified. No container was executed or system package installed. Direct devkitPro package servers returned HTTP 403, so the official container supplied the SDK. `switch_sdk.json` records the source.

`../dist/airborne-nx-probe.zip` contains an SD-card folder with the NRO, the local normalized `image.bin`, instructions and checksums. On the Switch it verifies that image's SHA-256, runs the verbatim generated leaf `0x828B6D00` 768 times, executes the existing 1,138,867-case generated FLOAT16_2 suite on ARM64 and checks the monotonic timebase. The leaf's byte-load macro uses a checked callback into the image, so this isolated test does not reserve a full 4 GiB guest map or invoke the game entry. The NRO contains neither the full Windows runtime nor a GPU backend.

**Compilation/linking and NRO/ELF format checks succeeded. The user has supplied a passing hardware-test log.** The app writes `result.txt` beside the NRO and waits for + to exit. The saved log reports HOS 22.1.0 and applet_type=0 (libnx Application). Console model and Atmosphere version were not supplied; do not infer them from the log. This validates target CPU/FPCR behavior before relying on it for further port work; it is not a playable game build. `switch_probe_summary.json` records compiler, source/artifact hashes and scope.

### Received Switch hardware result

The user supplied `result.txt` from CPU probe v0.1. The preserved log and structured evidence are in `hardware/`; the input log SHA-256 is `ecb1e73175f9b23b177ea420f1581d6df44984e08c8989191673ef8f0a275685`. It reports **RESULT=PASS**, normalized-image SHA-256 success, 768 real leaf calls, 1,138,867 emitted FLOAT16_2 executions and a monotonic 50 MHz timebase (500,201 ticks across the requested 10 ms delay). Environment: HOS 22.1.0, libnx Application mode.

This is user-reported hardware evidence, not independently observed console execution or an attestation of the NRO hash. It confirms this isolated test passed; it does not validate the full game, all PPC instructions, the 4 GiB address model, GPU or audio. Original build/package records retain their build-time status. `../tools/import_switch_result.py` validates the expected log fields, copies the evidence into the project and records its checksum; `recompile_summary.json` attaches it separately from current-build fingerprints.

### Switch memory probe v0.2 — hardware mapping failure recorded

`../runtime/switch/guest_memory.h` adds an initial target-side address-space implementation. It reserves a contiguous **4 GiB VA window** through libnx's virtual-memory tracker and maps page-aligned buffers from the existing hbloader heap through the process handle provided by the loader. It checks syscall hints before calling privileged APIs. It does not resize the process heap or consume a second 20 MiB kernel allocation outside the hbloader heap budget. `svcMapMemory` is unsuitable for simultaneous physical aliases because it revokes access to the source; this prototype instead uses self-process `svcMapProcessMemory` / `svcUnmapProcessMemory`.

The new NRO maps the original normalized image at guest 0x82000000 and a 256 KiB stack at 0x10010000. A 64 KiB backing is simultaneously mapped at logical guest A0010000, C0010000 and E000F000; the existing E-window bias makes the last host pointer base+E0010000. The original leaf body runs **without overriding its byte-load macro**. SHA-256 is rechecked through the mapped address before 768 leaf calls. Actual emitted scalar/vector load/store, load-reserve/CAS and cache-zero functions then check alias coherence. Stack guards are checked by kernel memory queries, without deliberately crashing the process. Invalid overlapping, unaligned, zero-sized, biased-out-of-range and threshold-crossing requests are rejected. Cleanup is explicit and checked; an unmap failure retains backing/reservation and exits the process after + rather than returning live aliases to hbloader.

Local validation: **65,609 API/cleanup contract checks** pass against a mock kernel, including injected reservation/map/unmap failures, reverse cleanup, retained backing on unmap failure and fixed-capacity bounds. Of these, 65,536 check zero-initialized backing bytes; this is not 65,609 distinct scenarios or Horizon validation. Twelve parser tests exercise synthetic log fixtures and reject incomplete/contradictory or unknown-build PASS logs; these fixtures are not imported as hardware evidence.

The v0.2 NRO is compiled and linked with devkitA64/libnx. Its ELF target, NRO magic, source/artifact hashes and ZIP integrity are checked. `switch_memory_probe_summary.json` and `../dist/airborne-nx-memory-v0.2.zip` are separate from v0.1 artifacts and the user's saved CPU PASS. The log includes a source-derived build ID. **The user ran v0.2: the VA reservation passed, then the first mapping failed with 0xD401 (InvalidMemoryState).** It is a small mapping proof, not a complete virtual/physical allocator, a 512 MiB physical pool, full runtime, game boot or renderer.

The received log is preserved in `hardware/` and matches the original v0.2 build ID. Do not request another run of the unchanged v0.2. The requested virtual range was reserved at 0x0000000D51AAB000; the map operation returned module 1, description 106. Image and stack mapping were short-circuited by the old runner, so the log does not independently identify which operation failed. Cleanup passed and no instruction/alias checks ran.

API references: [libnx SVC definitions](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h), [libnx virtual-memory API](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/virtmem.h), [Atmosphère process-memory validation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_process_memory.cpp). The implementation was written locally from the API contracts; no kernel implementation was copied.

### v0.2 root cause and revised test workflow

The user explicitly requested that hardware tests cover only actual game needs and be batched and detailed to reduce SD/log transfers. This is recorded in the root `AGENTS.md` and `switch_next_validation_plan.md`. No small mapping-only retry is being requested.

The previous heap-alias design was incorrect: [the kernel memory-state definitions](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/include/mesosphere/kern_k_memory_block.hpp) give ordinary Normal heap pages no CanMapProcess flag, whereas code-data aliases carry it. This explains the observed 0xD401 rejection and should have been checked before the v0.2 hardware request. The old mock omitted this restriction.

The revised local backend allocates backing from the existing heap, borrows it with MapProcessCodeMemory into a reserved staging view, sets the staging view to RW, and uses that view as the guest-mapping source. Cleanup first removes guest aliases, then restores the borrowed heap pages and releases staging/VA reservations. The design does not execute code from those pages or resize the loader's heap. API diagnostics now include the exact call, pointers, size, result and segmented source/destination region state, attributes, permissions and references.

The updated mock explicitly rejects raw Normal sources, requires the staging/permission transition and tests failures in code mapping, permission setting and restoration. **65,621 local checks pass, using a mock kernel.** The revised backend and its two generated-code units also passed AArch64 object compilation; `switch_memory_compile_summary.json` records this evidence. This local revision was not hardware validated at that point; the subsequent v0.3 log below confirms the measured corrected paths. It is not a playable build. The old v0.2 NRO is retained as historical evidence rather than silently replaced. A future hardware bundle must combine relevant memory, worker/PCR/TLS/event and actual-startup checks with dependency-aware results and a detailed single log; see the validation plan.

Re-run generation from the workspace in PowerShell:

```powershell
./tools/recompile.ps1
# Also rebuild local tools and re-run XEX loading / switch discovery:
./tools/recompile.ps1 -BuildTools -Analyse
# Execute generated instruction tests and function-analysis tests:
./tools/test_tools.ps1
# Optional syntax-only check:
./tools/test_tools.ps1 -CheckGenerated
# Compile all host CPU objects and link/run the mapping diagnostic:
python tools/compile_generated.py
python tools/link_host_check.py
# Validate guest mappings and run entry until the expected missing service:
python tools/run_host_probe.py
# Build/package the isolated Switch CPU test with the local SDK:
python tools/build_switch_probe.py
python tools/package_switch_probe.py
# Build/package the target-side memory proof (keeps v0.1 separate):
python tools/build_switch_probe.py --kind memory
python tools/package_switch_probe.py --kind memory
python tools/summarize_recompile.py
```

These commands assume the current local toolchain and cloned dependencies remain installed. To recreate the modified dependency, check out the pinned XenonRecomp commit with its submodules and apply `tools/patches/xenonrecomp-airborne.patch` from within that checkout. The scripts overwrite derived outputs and remove stale numbered generated C++ units when the function count shrinks. They never modify the input XEX.

Next: finish local contract/cross-build checks and combine the game-relevant target-runtime phases into a detailed staged hardware bundle. Do not ask the user to repeat v0.2. Extend the Xenos-to-Switch rendering backend from the observed host VdInitializeEngines boundary. The isolated CPU probe passed on the user's console; complete runtime execution still requires a separate target build and hardware validation. The full Switch runtime, graphics/audio and filesystem services remain substantial work.

## Reproducibility and references

- `files.json`: paths, source sectors, byte sizes and SHA-256 for every extracted file.
- `audit.json`: executable metadata, library inventory, TU metadata and integrity results.
- `../tools/audit_game.py`: rerun metadata/integrity checks with Python 3.
- SVOD layout reference: https://github.com/raburton/god2iso/blob/7ff3c919949b65cd031bf8ad150327f337ff1116/Form1.cs
- XEX structure reference: https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/util/xex2_info.h (snapshot saved as xex2_info.h).

The extractor supports this dump's unshifted XDVDFS layout; it is not advertised as a general GOD converter. It refuses an existing output directory.


## Combined Switch startup prerequisites v0.3

`../dist/airborne-nx-startup-v0.3.zip` replaces the isolated memory retry as the
next hardware milestone. The v0.1 PASS and v0.2 failure remain separate evidence.
The current sources are `../runtime/switch/memory_probe.cpp`, `guest_memory.h`
and `startup_services.h`. The build command remains
`python tools/build_switch_probe.py --kind memory`; it now builds the v0.3 startup
bundle and records `switch_startup_probe_summary.json` without overwriting v0.2.

One run covers native worker creation/join, libnx TLS isolation, per-thread FP
rounding state, manual/auto-reset user events and timeout behavior, the guest
clock, process address-space/memory-budget information, staged image/stack
mapping, SHA256, 768 direct real-leaf calls, another 256 leaf calls in two workers
with private guest stacks/R13 scratch slots, physical scalar/vector/atomic/cache
coherence, 4 MiB heap zero initialization/coherence across 1024 pages, guards,
invalid mapping rejection and cleanup. Heap and physical phases can run after an
image phase failure. Native services run before guest mappings.

`result-v0.3.txt` logs flushed phase starts/results, syscall arguments, memory
states, decoded failures and the first unexpected failure. Worker waits are
bounded; a failure to join terminates the process so worker state cannot outlive
the runner. A cleanup failure retains resources and terminates after the exit
prompt. Native services are exercised again with mapped guest worker contexts;
the previously passing FLOAT16 CPU suite is not repeated.

Local memory lifecycle tests passed 65,621 mock-kernel assertions. Protocol tests
passed 46 synthetic checks across v0.2/v0.3, including blocked/missing/duplicate
phases, mismatched builds, incomplete/trailing logs and false PASS results. These
are local checks, not console evidence. The libnx NRO is cross-compiled and linked
with devkitA64; packaging checks source/artifact SHA256, AArch64 ELF, NRO header,
and ZIP CRC integrity. A matching v0.3 hardware PASS has now been received; see the evidence section below.

Scope remains prerequisites, not game startup: R13 preservation/private scratch
slots do not prove the full guest PCR/TLS ABI; no full heap allocator, 512 MiB
physical pool, game entry, graphics or audio is implemented in this NRO.
`game_entry=BLOCKED` is expected even when all included prerequisites pass.


### User-reported v0.3 hardware PASS

The supplied result-v0.3.txt matches build
`811084736791fbcc2e829be4effe120aad4706eac5b9340d995dcac725533b2b`
and ends with `FIRST_FAILURE=none`, `CHECKS=1569`, `RESULT=PASS`.
Evidence is saved as
`hardware/switch_startup_v0.3_334a87647c17.txt` (SHA256
`334a87647c17d5855f689de7784250c2e870f7b362bf97f70124ca6b9373eb78`).
The parser records `hardware/switch_startup_probe_result.json` separately from
build-time validation. The log reports HOS 22.1.0, libnx Application mode.

All included phases passed: native services, mapped image/stack, guest workers,
physical aliases, guest heap, guards and cleanup. The log confirms writable
staging regions with memory type 9 and guest aliases with type 15, RW permissions,
zero unexpected errors; all MapProcessCodeMemory/MapProcessMemory calls and
corresponding unmaps returned success. It reports 768 direct leaf calls, two
workers with 128 guest leaf calls each, and coherent heap writes across 1024 pages.
The previous v0.2 InvalidMemoryState failure is resolved for these measured paths.

This is user-supplied hardware evidence, not independently observed execution or
an attestation of the binary hash. Do not repeat these phases just for the same
evidence. Full guest PCR/TLS semantics, large physical allocation, full game entry,
graphics/audio/gameplay remain unvalidated. `game_entry=BLOCKED` is expected.
Process UsedMemory includes libnx's reserved heap; subtracting it from TotalMemory
is not a reliable measurement of remaining malloc space or game RAM demand.
The next local milestone is compiling/linking all generated CPU code for Switch,
then implementing the startup runtime with the confirmed primitives.


## Complete target CPU build pipeline

`python tools/compile_switch_generated.py --jobs 8` builds every generated CPU
translation unit with the locally pinned devkitA64 GCC/libnx headers, ARMv8-A,
Cortex-A57 tuning, PIE, strict FP contraction/fast-math settings and SIMDe NEON.
It produces the private `../generated/switch-cpu/libAirbornePPC.a` and
`switch_object_build_summary.json`. Each cache entry checks generated source,
headers, flags, compiler version, rotate adaptation and the existing object hash.
The SDK/SIMDe installation is the pinned local toolchain. A diagnostic `--limit`
subset can never publish the full-build summary or archive.

The generated Clang rotate builtins are adapted to unsigned modulo-width helpers
in `../runtime/switch/compiler_compat.h` by GCC-specific build flags. The generated
CPU code and the v0.3 sources are not rewritten. An independent bit-step oracle
passed 533,540 local comparisons, including zero/full-width counts, high-bit
counts, edge patterns and deterministic random values. Details are recorded in
`switch_compiler_compat_validation.json`; this is not target execution evidence.

`python tools/link_switch_check.py` verifies the archive/dependency hashes and
links every generated guest function into a libnx AArch64 ELF with 267 explicit
aborting import placeholders. The reference to the complete function mapping
retains the game CPU functions. It emits `switch_link_summary.json` and a linker
map, validates ELF architecture and records text/data/BSS sizes. This ELF is a
local link check, not a new console test or implemented game runtime. There is no
new NRO to transfer merely for compilation/link validation.


### Target CPU build/link completed

All **317 generated units compiled with zero failures**. The complete
AArch64 archive is 124,463,408 bytes (SHA256
`586d124524ef4123038a7bf9271269d8b857ff6892712360de5db0becea86e82`). The full libnx link completed with exit 0 and no
linker diagnostics. Its ELF is 94,200,840 bytes, machine AArch64 (183),
with section totals text=80,278,476, data=1,314,240, BSS=18,320 bytes.
The ELF SHA256 is `4a3fa82ba8285239a7675805472d1bbc21858493f3f244cf68349afbd8b9a78a`.

This milestone establishes target compilation and resolution of all host-side
symbols while retaining the complete guest function mapping. Its 267 Xbox import
placeholders abort; no guest entry was called and no full-code ARM64 execution or
performance result is claimed. The next implementation step is the Switch startup
runtime (allocation, import/variable binding, guest PCR/TLS, synchronization and
thread ABI), using the primitives already confirmed by the v0.3 log. Graphics
remains a separate missing backend; the host startup currently stops honestly at
VdInitializeEngines. No new console package is issued for the link check.


## Switch real guest-entry runtime v0.4

`../runtime/common/startup_services.inc` now contains the startup import handlers
shared by Windows and Switch. Platform-specific memory/thread/clock/lock adapters
are supplied by `../runtime/switch/platform_compat.h` and `mapped_arena.h`.
Windows validation still passes 1,273 service checks and real guest startup still
stops at `VdInitializeEngines`, LR `0x829c0788`. No graphics success is fabricated.

Switch virtual commits directly borrow heap backing into AliasCodeData at the
guest destination, followed by the one-time RW transition. Later virtual page
permission changes use SetMemoryPermission. Physical allocations use a lazily
created 512 MiB staging pool and map only allocated slices into A/C/E windows;
those SharedCode aliases support RW/unmapped, and explicitly reject read-only.
Both heap allocators are the existing startup-profile algorithms, not a complete
Xbox kernel implementation. The kernel state distinction was checked against
[the primary memory-state definitions](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/include/mesosphere/kern_k_memory_block.hpp)
and [permission implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_page_table_base.cpp).
No kernel implementation code was copied.

New memory lifecycle tests pass **68 mock-kernel checks**, covering alignment,
overlap, holes, protection, decommit validation, physical alias cleanup, partial
alias-map failure, mapping/permission rollback and resource retention after an
unmap failure. These are local contract tests, not Horizon emulation. Probe/log
parsers pass **63 synthetic checks** across v0.2/v0.3/v0.4, including incomplete,
contradictory, missing-stage and mismatched-build logs; fixtures are not imported
as hardware evidence.

`../tools/build_switch_entry.py` links the complete 317-unit ARM64 CPU archive
with real startup import dispatch instead of the 267 aborting placeholders.
`entry_runtime.cpp` verifies the pinned normalized image and 16 KiB XEX header,
builds the complete function lookup table, binds 277 import records, initializes
PCR/TLS and calls real guest entry `0x82C5C310`. An initial unwind bridge compiled
with the CPU archive flags checks exception propagation before real guest frames;
its AArch64 frame and .eh_frame metadata were inspected locally. The generated
CPU archive itself has not been rebuilt or changed.

The supervisor writes a flushed single report with Build ID, environment, an
ASLR symbol anchor, memory operations and states, import arguments and guest
LR/SP/R13/CTR. Its watchdog is 120 seconds. A custom libnx exception hook attempts
to append ARM64 registers/PC/LR/SP/FAR/ESR through a separate descriptor. Logging
failure is shown on the console. Remaining guest workers keep their memory until
whole-process exit after +; no mappings are freed under live workers.

The v0.4 NRO is cross-compiled and linked, not yet executed on Switch. The bundle
is `../dist/airborne-nx-entry-v0.4.zip`; build/artifact hashes are recorded in
`switch_entry_summary.json`. It is not a playable build. Expected diagnostic
completion is `RESULT=STOPPED`, not PASS: the exact target runtime boundary must
be learned from the user log. There is no renderer/audio/gameplay implementation.
The v0.1/v0.3 hardware PASS and v0.2 failure remain separate historical evidence.

Build/package commands:

```
python tools/build_switch_entry.py
python tools/package_switch_probe.py --kind entry
```


## v0.4 target entry evidence and post-result exit failure

The user log matches build
`7c22a4a387d553e2f5762868a4332550e419d18cc5baad045a9c20df9d9517f1`.
It records real Switch guest startup through 8,649 import calls, two worker
threads and the same graphics boundary as Windows: `VdInitializeEngines`,
LR `0x829c0788`. Full mapping-table setup, generated-frame unwind, virtual
allocation/protection and the 512 MiB staging pool succeeded. No failed memory
SVC, loader error, native fault or watchdog appears in the supplied log. It ends
`EXIT_STATUS=10`, `RESULT=STOPPED`; this is a measured runtime boundary, not a
game PASS or gameplay result.

Saved log: `hardware/switch_entry_v0.4_f6be6b4c8edf.txt`, SHA256
`f6be6b4c8edf993145169a272efa3e2cac085181f240e4f4816aa39e68a9569b`.
The user's separate observation is that pressing + afterwards produced a standard
HOS error. No HOS error code was supplied and that later failure is absent from
the log. Preserve both observations; do not classify the whole application as
having passed or attribute an exact HOS code/cause without evidence.

The v0.4 normal exit directly called svcExitProcess with workers/mappings alive.
It bypassed the libnx service shutdown/loader-return path. The revised source
cooperatively interrupts waits/delays/contended critical-section entry, wakes
event waiters, joins and closes native threads, and only then closes all guest
mappings/backings. It closes report/console resources and returns from main so
libnx performs its normal teardown. If workers or mapping cleanup cannot safely
complete, it retains resources and asks the user to close via HOME instead of
returning borrowed heap pages to the loader. Native fault handling still uses
emergency process termination and is not claimed repaired by the normal-exit fix.

Local validation: 14 native-adapter shutdown checks with mocked SVCs verify
cancellation of indefinite thread waits, long delays and blocked critical-section
entry; 1,273 shared host service checks still pass and host entry retains the
same honest graphics stop. The revised target runtime is validated through
`python tools/build_switch_entry.py --validate-only`, which compiles/links in
`generated/switch-entry-next` without replacing the user-tested v0.4 NRO, ELF,
archive, build identity or hardware result. The exit correction has not yet been
executed on Switch. Do not request an isolated exit-only hardware retry; include
it in the next substantive runtime/graphics bundle.


## Vulkan entry 0.5 (current)

See [vulkan_entry_v0.5.md](vulkan_entry_v0.5.md). Current Windows boundary is VdSwap; the newly packaged Switch/NVK build has not been run on hardware. Older boundary/build descriptions above are historical. No game image is claimed.


## Supplied Switch result for 0.5

The user log for build `fefdd112f4ab29aded0c3a2d88487456ec31c8607daeb3c8abf348bfad991b1c` confirms NVK initialization on NVIDIA Tegra X1 (GM20B), the 1280x720 image allocation (3,932,160 bytes), 72 PM4 packets, and the same VdSwap boundary as Windows. It records 8,782 imports and no native fault, watchdog or failed operation. Shutdown joins/closes four workers and the initial thread, destroys Vulkan resources, unmaps guest memory and ends CLEANUP=PASS / EXIT_ACTION=RETURN_TO_LOADER. This records cleanup before returning; subsequent loader/HOS behavior is not captured. No game image or actual GPU draw submission is validated. Do not repeat NVK initialization or exit-only hardware checks.

Evidence: `analysis/hardware/switch_entry_v0.5_result.json`; saved log SHA256 `bf12b9e2d449d405b96c84c57f9c822e2ff1503d829645ee04c342c02353bb4a`. Earlier NOT_RUN descriptions in this document describe the pre-test state. The packaged 0.5 NRO/ELF/ZIP are retained unchanged.


## Native startup graphics 0.6

See [vulkan_entry_v0.6.md](vulkan_entry_v0.6.md). The packaged development build now performs actual native depth clears and the restricted startup resolve/guest-pixel transfer on Windows, reaches 1,024 retired PM4 packets, and stops at an unmatched GPU wait. VI presentation and window handoff are implemented and cross-linked but have not run on Switch. The measured 0.5 hardware result remains valid and separate.


## Supplied 0.6 hardware evidence

The supplied log matches build `ba51b874a908715cb90a4725560b1a59231042e415fc5ec43a6004289d9c8f70`. Intact records show two successful GPU depth clears, the restricted EDRAM resolve, the 1280x720 VI surface and three-image swapchain, actual queue submission/fence completion, `vkQueuePresentKHR rc=0` and `DISPLAY_QUEUED_GUEST_PIXELS`. It reaches the same 1,024-packet unmatched wait as Windows. All recorded shutdown/join/unmap results succeed and the log ends CLEANUP=PASS / EXIT_ACTION=RETURN_TO_LOADER. Queueing pixels is not independent visual observation or game rendering validation.

The raw file also contains two corrupted text regions around interrupt handling, including invalid UTF-8 and a NUL at byte 1,706,341. The first invalid UTF-8 byte is 1,706,342 (zero-based). The explicit interrupt-dispatch text records are absent; missing lines are not reconstructed. The report is classified USER_REPORTED_PARTIAL_EVIDENCE_CORRUPTED_LOG, retaining individual intact graphics milestones. Concurrent logging is a possible cause, not an established diagnosis. A dedicated logging/flush mutex has been added for subsequent builds; target verification is pending. No isolated repeat hardware run is requested.

Original bytes are preserved as `analysis/hardware/switch_entry_v0.6_20c80321cb3f.txt`, SHA256 `20c80321cb3f4cee358d600b48e5719b1027dcacafcfb2f9966b3249749ee5d9`. Parsed details are in `analysis/hardware/switch_entry_v0.6_result.json`. The tested 0.6 NRO/ELF/ZIP remain unchanged. Earlier NOT_RUN descriptions refer to the pre-test state.


## Development after measured 0.6: display completion and audio startup

See [startup_after_v06.md](startup_after_v06.md). The Windows guest now passes the old 1,024-packet display wait through the game's own vblank interrupt handler, initializes the XMA context table, starts the XMA worker and registers an XAudio callback worker with a real Windows PCM output device. The current bounded stop is `NetDll_XNetStartup`, LR `82a98a54`. This is later initialization, not a game screen. No XMA decoding or game audio playback has been observed.

The Switch runtime adds actual VI vblank event waits and a Horizon PCM output path. Cross-compilation is recorded separately from target execution. The user-tested 0.6 artifacts and hardware observations remain unchanged; no new hardware run is requested for this milestone.


## Real game resource loading

See [resource_loading.md](resource_loading.md) and `resource_loading_progress.json`. The Windows guest now reads the extracted Xbox 360 TOC and cooked configuration, passes the second drained-ring initialization, and stops at a new native raster pipeline/resource-binding requirement after 1,960 retired PM4 packets. The captured shader pair compiles and validates as SPIR-V. The full AArch64 runtime links successfully; no new NRO or Switch execution is claimed. Earlier entry boundaries in this document are historical.
