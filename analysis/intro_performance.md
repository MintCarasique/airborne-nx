# Intro performance checkpoint

Measured Switch baseline: f4bac5364158a9e30766ef4c0baa5a6b656b197b730086ec1215c9b298377756.
Raw log: hardware/switch_entry_f4bac5364158_845658fe7943.txt (SHA256 845658fe7943500bd65232ceae76e36f21c14c9b8593a57d12048e1385a62d04).
Four completed native movie draws; user confirms intro visible, very slow. Log ends with repeated KeWaitForSingleObject; no cleanup/watchdog record. No measured FPS or clean exit claim.

Changes in the pending build:
- Game threads use libnx documented preemptive priority 0x3b on cores 0..2, with allowed-priority/core checks and priority readback. Previously all threads used default core and 0x2c/0x2b priorities. Virtual Xbox priorities remain metadata; exact relative scheduling is incomplete. No extra cores enabled. This fixes a starvation risk; its target contribution remains unmeasured.
- Exact required-byte calculation scans only the last occupied 32x32 tile, because tile intervals are disjoint increasing 4096-byte regions. Linear geometry uses a direct formula. 37,668 comparisons against full enumeration passed, including partial tiles, 720p and 4096x4096. PC timings are not Switch speed claims.
- Switch checks the first offscreen transfer/readback. Subsequent frames copy directly to VI, eliminating the unused duplicate color image transfer/readback/memcmp. Fences, semaphores, error handling remain; Windows validates every frame. The movie shader readback used for EDRAM remains necessary.
- Repeated single-object wait traces are sampled (first 8, then each 4096); aggregate counts and errors retained.
- MOVIE_FRAME records sequence, uptime, inter-draw interval. GPU_STAGE_TIME records movie upload/draw/readback, resolve, decode/present. Inter-draw interval includes game work/waits; not pure decoder time.

Validation: thread_policy_validation.log tests production helper with all syscall failures, unavailable masks and priority mismatch. front_extent_validation.log tests exact bounds. Full host checks and original entry rerun in host_probe_validate.log / host_probe_entry.log; host still stops at XMADisableContext. Target compile and package hashes in switch_entry_current_summary.json.

Remaining: target speed unmeasured; XMA context/decoding and general menu rendering incomplete. Full clip/gameplay not claimed. 120-second watchdog unchanged. Measured baseline artifacts archived before overwrite.
