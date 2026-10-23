# 120-second black screen: observed progress and contention correction

Measured build: e05d08748c721358d0acfcd45ad905eac48b404131dd3d36e396abc249b44289.
Raw log SHA256: e7bd9a3c60bf313bcf5ced69c7db1566ddbba19589e047813d725c6992f5e272.
The user saw black for 120 seconds. The log records 513 successful real VI event
waits and 256 presentations of the known-zero initialization buffer, no game
file opens or movie draws, then the 120-second watchdog. Cleanup succeeds.
Required TOC/config/movie files were found. Repeating display-event capability
checks or moving game content again is unnecessary.

Main-thread imports progress across the entire trace: 2,999 detailed calls,
2,713 of them KeTlsGetValue. The watchdog samples native svcArbitrateLock.
Symbolization uses nx_unwind_throw anchor 0x4a04882a4, ELF symbol 0x9d2a4,
load base 0x4a03eb000, sampled PC 0x4a695cf90 => ELF PC 0x6571f90. This
demonstrates a mutex wait at the snapshot, not a proved permanent deadlock.
The old logger makes every line an unbuffered SD write and explicitly flushes
it; all imported TLS reads also queue behind the same shared service mutex
used by GPU work. These are concrete avoidable serialization sources. Their
individual contribution to the 120-second target duration is not measured.

Correction:
- Keep allocation/free synchronization, but read/write the calling thread's
  permanent TLS block before acquiring the global service mutex. Bounds and
  return values stay unchanged. Atomic stop checks retain cooperative shutdown.
- Buffer normal report writes in 64 KiB, flush at most every 250 ms through
  normal trace calls, and force errors/watchdog/cleanup/exit records immediately.
  Preserve the first sink errno even after later errors or successful flushes.
- Record aggregate TLS calls, service-lock wait totals/maxima, report write/
  flush counts, and native watchdog LR/FP/arguments. No per-call performance
  probe or extra hardware exercise is inserted. The deadline remains 120 s.

Local checks exercise the actual implementation, not a replacement game:
two native threads perform 8,000 TLS operations on distinct blocks while the
global service mutex remains held; both complete and retain their own values.
The report writer replays 50,001 ordered records with two explicit flushes
(libc may also drain a full buffer). The critical last error is readable after
the forced flush. Injected EIO then ENOSPC and recovery preserve the first EIO.
See `buffered_report_validation.log` and `host_probe_validate.log`.

Windows original entry continues through actual movie rendering to the known
XMA context-control stop. These checks do not establish startup speed or a
visible game image on Switch. Measured artifacts are archived before the
stable airborne-nx output is replaced. Collect one game run, not new isolated
tests for display/memory/CPU contracts already established by prior logs.
