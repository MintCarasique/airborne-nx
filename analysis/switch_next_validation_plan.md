# Next Switch validation bundle

User requirement: tests must target actual game needs, provide detailed results
in one run, and minimize manual SD-card/log transfers. Do not issue another
isolated v0.2 mapping retry as the next hardware milestone.

Before requesting hardware:

1. Validate memory-state contracts against the kernel and SDK; locally model the
   actual `Normal` source rejection and failure cleanup, rather than accepting
   every aligned source in a mock.
2. Cross-compile the revised backend and progress toward the actual game startup
   runtime. Fix compile/link errors on PC before making another hardware package.
3. Build a staged runner that records each phase as PASS, FAIL, BLOCKED or SKIP.
   Continue independent phases after a failure when memory cleanup is safe.
4. Cover, in one bundle, capabilities/address-space constraints; mapped image and
   required heap/stack accesses; physical aliases used by the game; native worker
   threads with PCR/TLS and events; timing/FP state needed by startup. Do not
   repeat the already passing CPU suite merely to obtain the same evidence.
5. Where the target runtime is ready, attempt the real entry 0x82C5C310 and record
   the next genuine unsupported service. Do not fabricate GPU success to advance.

Required diagnostics:

- Build ID, HOS, launch mode and relevant syscall hints/own-process handle.
- For each mapping: operation, guest address, exact source/destination and size,
  result code with decoded module/description, memory state/attributes/permissions
  across the requested range, IPC/device references, and cleanup outcome.
- The first unexpected error retained separately from subsequent failures.
- Dependency-aware phase results so one blocked memory phase does not hide
  independent thread/timer diagnostics. Bounded execution and flushed logs.
- A final concise summary and complete machine-readable detail in one result
  file; report partial logs on crash without classifying them as PASS.

v0.2 finding: 4 GiB VA reservation succeeded. The first image/stack mapping
returned 0xD401: module 1, description 106, InvalidMemoryState. The old runner
short-circuited these operations and did not log their arguments or region states.
Kernel source establishes that Normal heap pages lack CanMapProcess. The revised
backend first borrows the backing into a writable AliasCodeData staging view and
then maps guest aliases from that view. This correction is not yet hardware
validated. No new console run is required just to reconfirm the old rejection.


Implemented v0.3 bundle
-----------------------
The next bundle is now implemented as the combined startup-prerequisite runner,
not an isolated mapping retry. It includes independent native services,
dependency-aware image/stack/workers, independent physical and heap phases,
process memory/address-space queries and detailed cleanup. A real game entry
attempt remains explicitly BLOCKED because the full Switch startup runtime and
required graphics service are not ready. The shipped scope does not claim this
step complete or validate the full guest PCR/TLS ABI. See README.md for local
validation and the exact included phase scope. The supplied v0.3 log now confirms the corrected mapping contract and included
worker behavior on hardware. Do not request this bundle again. The next milestone
is a complete target CPU build/link and the real startup runtime.


Target CPU build/link milestone completed
----------------------------------------
All 317 generated units now compile into a devkitA64 AArch64 archive and link
successfully with libnx and 267 aborting Xbox-import placeholders. No full-code
target execution is claimed. The next hardware bundle must use the real Switch
startup runtime and attempt game entry; do not ask for a separate full-link-check
run or repeat v0.3 prerequisite measurements. Validate binding/allocator/thread
failure paths locally first, and retain game import traces and unsupported-service
stops in the same detailed report.


v0.4 ready for target entry evidence
----------------------------------
The shared Windows/Switch startup runtime, target memory backend and full CPU
archive now compile/link into a bounded real-entry NRO. Local validation covers
shared host services/startup, memory lifecycle failures and result parsing.
The next required hardware action is one v0.4 run and its result-v0.4.txt, not a
re-run of prerequisite probes. STOPPED denotes the measured service boundary,
not game PASS. Do not advance through VdInitializeEngines with fabricated success.


v0.4 measured boundary / exit correction
----------------------------------------
A matching user log now confirms real target entry through VdInitializeEngines
with successful virtual and physical memory setup. A HOS error was separately
reported after +; its code is unknown and is not in the result log. The normal
exit path is revised to cooperatively join workers and unmap all backing before
returning through libnx. Keep the hardware-tested v0.4 package and this evidence
intact. Validate the correction locally/cross-link only for now, then combine it
with the next substantive graphics/runtime milestone. No isolated exit retry.

Vulkan implementation in progress
---------------------------------
The target renderer direction is now Vulkan through the Horizon Mesa NVK port.
The real Windows guest entry passes VdInitializeEngines with actual Vulkan
resources and reaches ring initialization. Shader extraction/translation and a
partial runtime library are implemented; see vulkan_port.md and the machine
readable shader reports. Native command execution, presentation and Switch NVK
runtime behavior remain unverified. Do not request a separate GPU-init run.
