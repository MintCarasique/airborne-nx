# Vulkan game-entry milestone 0.5

The new Switch package runs the recompiled title with the shared startup services,
native Mesa NVK Vulkan device, and a partial Xenos command processor. It has not
been executed on Switch. It does not render game frames or provide gameplay.
The hardware-tested v0.4 files and their recorded result remain separate.

Windows entry now reaches `VdSwap`, LR `0x829b3b08`. The observed request is
1280x720, format 6, tiled, pitch 1280, front buffer `0xff657000`. This is a
request to display a guest buffer, not a rendered or presented frame. Before
this point, 72 PM4 packets and 8,758 import calls completed. Twenty-four startup
draws were conservatively proven to have neither raster outputs nor memory
exports and retired as no-ops. Other draws still stop explicitly.

Implemented command handling includes ring wrapping, indirect lists, shader loads,
constant/register writes, memory writes with endian controls, read-pointer
writeback and supported event completion writes. The observed ring has size
32 KiB (encoded size 12), not 4 KiB. Unmatched waits, video-clock counters,
interrupt dispatch and unsupported draw/presentation paths stop before retirement.
The HLE system buffer descriptor points to owned guest memory with a generation
lease. VdSwap logs its arguments and stops without acknowledging presentation.

The new scalar MMIO hook is compiled into both complete 317-unit CPU archives.
Additional startup support covers dispatcher events, current-thread references,
APC-disable nesting, IRQL bookkeeping, and physical RW cache-policy changes.
Native FP initialization now masks native traps on every guest thread, while
guest exception flags remain represented by the recompiled runtime. Native API
boundaries preserve incoming FP control. This fixed the Windows inexact trap
encountered after physical-buffer setup.

Local verification: 1,273 shared startup-service checks; 29 PM4/MMIO contract
checks including memory-export/fetch/visible-output rejection and event ordering;
focused FP bootstrap/inexact-arithmetic/flush/API-restoration regression checks;
successful actual Windows entry to the explicit VdSwap boundary; successful
complete AArch64 compile/link, NRO and ZIP/header checks. Shader corpus results
remain 969 validated SPIR-V modules out of 1,036 containers, with 67 unsupported
WMV variants. These shaders are not yet bound to title draw commands.

Package: `dist/airborne-nx-entry-v0.5.zip`. Build identity, source fingerprints and
artifact hashes: `analysis/switch_entry_v0.5_summary.json`. One hardware run
collects NVK initialization, actual title commands, the first unsupported stage,
and cooperative cleanup results into `result-v0.5.txt`. The previously reported
HOS failure after + has a proposed cleanup fix in this package, with no claim
that target execution has verified it.

Remaining rendering work: Vulkan pipeline/resource binding for Xenos draws,
texture/EDRAM resolve and native presentation, plus video timing/interrupt
contracts when reached. The hardware log is needed to determine whether NVK and
the shared runtime reach the same boundary on Horizon.


## Supplied Switch result for 0.5

The user log for build `fefdd112f4ab29aded0c3a2d88487456ec31c8607daeb3c8abf348bfad991b1c` confirms NVK initialization on NVIDIA Tegra X1 (GM20B), the 1280x720 image allocation (3,932,160 bytes), 72 PM4 packets, and the same VdSwap boundary as Windows. It records 8,782 imports and no native fault, watchdog or failed operation. Shutdown joins/closes four workers and the initial thread, destroys Vulkan resources, unmaps guest memory and ends CLEANUP=PASS / EXIT_ACTION=RETURN_TO_LOADER. This records cleanup before returning; subsequent loader/HOS behavior is not captured. No game image or actual GPU draw submission is validated. Do not repeat NVK initialization or exit-only hardware checks.

Evidence: `analysis/hardware/switch_entry_v0.5_result.json`; saved log SHA256 `bf12b9e2d449d405b96c84c57f9c822e2ff1503d829645ee04c342c02353bb4a`. Earlier NOT_RUN descriptions in this document describe the pre-test state. The packaged 0.5 NRO/ELF/ZIP are retained unchanged.
