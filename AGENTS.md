# AirborneNX working instructions

The project targets Medal of Honor: Airborne on Nintendo Switch homebrew.

## Build naming (explicit user request)

- Publish updates to `dist/switch/airborne-nx/airborne-nx.nro` and
  `dist/airborne-nx.zip`. Use `result.txt` for the current hardware log.
- Keep versions/build IDs inside metadata and logs, not in install folders or
  executable filenames. Reuse `airborne-nx/game` across updates.
- Preserve measured builds as internal archives before overwriting them; do not
  create another numbered install directory to preserve a hardware result.

## Hardware testing preference (explicit user request)

- Test only functionality needed for the game or a concrete port blocker.
- Minimize trips between PC and Switch. Batch related checks into a detailed
  hardware build rather than issuing a new NRO for every isolated assumption.
- Investigate SDK/kernel contracts and exercise failure handling locally first.
- A hardware failure must log the failing operation, result code, source and
  destination addresses/sizes, memory states/attributes/permissions and relevant
  capability/environment information, with a build ID. Preserve the first error.
- Continue independent checks after a failure when this is safe; distinguish
  failed, blocked, skipped and passed checks. Always log cleanup results.
- Do not replace a successful hardware result with an untested build claim.
- Do not request another hardware run merely to confirm information already
  established by the user's logs or available primary sources.
