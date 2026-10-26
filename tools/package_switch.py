"""Package the Switch build into dist/switch/airborne-nx and dist/airborne-nx.zip.

Usage: python tools/package_switch.py [--nro PATH]

Archives the previous measured NRO bundle (top-level files only) to
dist/archive/<sha256 of nro>.zip, installs the new NRO and default.xex,
writes airborne.toml, README.txt and SHA256SUMS.txt, the Coalesced.ini
override (32 audio channels instead of 90) made from the extracted game, and
builds the zip. The game/MOHAGame folder is never read or copied. dist/ holds
game data and is never committed.
"""
from datetime import date
from pathlib import Path
import argparse
import hashlib
import subprocess
import sys
import zipfile

root = Path(__file__).resolve().parents[1]
bundle = root / 'dist/switch/airborne-nx'
archive_dir = root / 'dist/archive'
zip_path = root / 'dist/airborne-nx.zip'
SLUG = 'switch/airborne-nx'
TOP = ['airborne-nx.nro', 'README.txt', 'SHA256SUMS.txt', 'airborne.toml',
       'image.bin', 'xex-header.bin']
OVERRIDE_INI = 'override/MOHAGame/Config/Xenon/Cooked/Coalesced.ini'
PACKED = ['airborne-nx.nro', 'airborne.toml', 'README.txt', 'SHA256SUMS.txt',
          'game/default.xex', OVERRIDE_INI]


def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def git(*args):
    return subprocess.run(['git', *args], cwd=root, check=True,
                          capture_output=True, text=True).stdout.strip()


ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument('--nro', type=Path, help='NRO to install (default: the only rex/out/switch/*.nro)')
args = ap.parse_args()
nro = args.nro
if nro is None:
    found = sorted((root / 'rex/out/switch').glob('*.nro'))
    if len(found) != 1:
        raise SystemExit(f'expected exactly one NRO in rex/out/switch, found {len(found)}')
    nro = found[0]
if not nro.is_file():
    raise SystemExit(f'NRO not found: {nro}')
xex = root / 'extracted/base/default.xex'
if not xex.is_file():
    raise SystemExit(f'missing {xex}')

# 1. Archive the previous measured build.
archived = None
old = bundle / 'airborne-nx.nro'
if old.exists():
    archived = archive_dir / f'{sha256(old)}.zip'
    if archived.exists():
        print(f'archive already exists: {archived.name}')
        archived = None
    else:
        archive_dir.mkdir(parents=True, exist_ok=True)
        names = [n for n in TOP if (bundle / n).is_file()]
        with zipfile.ZipFile(archived, 'w', zipfile.ZIP_DEFLATED) as z:
            for n in names:
                z.write(bundle / n, n)
        print(f'archived {", ".join(names)} -> {archived}')
    for n in ('image.bin', 'xex-header.bin'):
        (bundle / n).unlink(missing_ok=True)

# 2. Install the new build.
(bundle / 'game').mkdir(parents=True, exist_ok=True)
(bundle / 'airborne-nx.nro').write_bytes(nro.read_bytes())
(bundle / 'game/default.xex').write_bytes(xex.read_bytes())

# The game's software mixer (RWAudioCore) mixes up to [XeAudio.XeAudioDevice] MaxChannels voices on the CPU; the
# Xbox 360 value is 90. 32 cut the audio thread from 64-72 % to 54-59 % of a core in Adanti's battles with no
# missing sound heard (docs/HARDWARE_NOTES.md). The app substitutes files under <NRO folder>/override.
ini = root / 'extracted/base/MOHAGame/Config/Xenon/Cooked/Coalesced.ini'
(bundle / OVERRIDE_INI).parent.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, '-I', str(root / 'tools/coalesced.py'), 'set', str(ini), str(bundle / OVERRIDE_INI),
                'Cooked\\Xe-MOHAEngine.ini', '[XeAudio.XeAudioDevice]', 'MaxChannels=32'], check=True)

# 3. Switch config overrides (read from the NRO's folder).
(bundle / 'airborne.toml').write_text(
    '# AirborneNX Switch overrides (read from the NRO folder). Command-line\n'
    '# arguments (nxlink) still win over these.\n'
    'log_file = "sdmc:/switch/airborne-nx/result.txt"\n'
    'log_level = "info"\n'
    '# The native Vulkan renderer (the Xenos emulation is far too slow here).\n'
    'nfsmw_renderizador = "nativo"\n'
    '# Scene resolution: 1280x720 (the Xbox 360\'s), 1120x624, 1040x576,\n'
    '# 960x544, 880x496 or 800x448; smaller ones are faster and softer.\n'
    'airborne_scene_resolution = "1280x720"\n'
    '# Horizon priority of the game audio thread (RWAudioCore Dac), decimal: 44 = 0x2C,\n'
    '# above the game threads (0x3B), so the sound gets the CPU before the frame rate does.\n'
    'guest_audio_thread_priority = 44\n', encoding='utf-8')

# 4. Build ID and README.
nro_sha = sha256(bundle / 'airborne-nx.nro')
dirty = '-dirty' if git('status', '--porcelain') else ''
build_id = f'{git("rev-parse", "--short", "HEAD")}{dirty}+{nro_sha[:12]}'
(bundle / 'README.txt').write_text(f"""AirborneNX (Medal of Honor: Airborne) for the Nintendo Switch
Build: {build_id}
Date: {date.today().isoformat()}

ReXGlue runtime, native renderer on Vulkan (NVK). The game text is German unless
an English Localization/Coalesced.int is under override/ (see docs/STATUS.md).
Settings: airborne.toml next to the NRO (scene resolution, audio priority).

The project is on hold: the game runs, but heavy levels reach only 10-15 FPS
and the sound stutters there. docs/STATUS.md in the repository explains why.

Install (SD card):
  /switch/airborne-nx/airborne-nx.nro      - the application
  /switch/airborne-nx/game/MOHAGame        - the game files (your own dump)
  /switch/airborne-nx/game/default.xex     - the game executable
  /switch/airborne-nx/override/            - replaced game files (Coalesced.ini)
  /switch/airborne-nx/result.txt           - the log (created at start)

Update: extract the `switch` folder of airborne-nx.zip to the root of the SD card,
replacing files. The game files stay.

Start: title takeover mode - hold R while starting any installed game, hbmenu
opens; choose airborne-nx.
""", encoding='utf-8')

# 5. Checksums.
sums = ['airborne-nx.nro', 'airborne.toml', 'README.txt', 'game/default.xex', OVERRIDE_INI]
(bundle / 'SHA256SUMS.txt').write_text(
    ''.join(f'{sha256(bundle / n)}  {n}\n' for n in sums), encoding='utf-8')

# 6. Zip.
zip_path.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
    for n in PACKED:
        z.write(bundle / n, f'{SLUG}/{n}')

# 7. Summary.
print(f'build ID:  {build_id}')
print(f'NRO size:  {(bundle / "airborne-nx.nro").stat().st_size} bytes')
print(f'zip size:  {zip_path.stat().st_size} bytes ({zip_path})')
print(f'archive:   {archived if archived else "none created"}')
