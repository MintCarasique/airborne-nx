"""Package the Switch build into dist/switch/airborne-nx and dist/airborne-nx.zip.

Usage: python tools/package_switch.py [--nro PATH]

Archives the previous measured NRO bundle (top-level files only) to
dist/archive/<sha256 of nro>.zip, installs the new NRO and default.xex,
writes airborne.toml, README.txt and SHA256SUMS.txt, and builds the zip.
The game/MOHAGame folder is never read or copied.
"""
from datetime import date
from pathlib import Path
import argparse
import hashlib
import subprocess
import zipfile

root = Path(__file__).resolve().parents[1]
bundle = root / 'dist/switch/airborne-nx'
archive_dir = root / 'dist/archive'
zip_path = root / 'dist/airborne-nx.zip'
SLUG = 'switch/airborne-nx'
TOP = ['airborne-nx.nro', 'README.txt', 'SHA256SUMS.txt', 'airborne.toml',
       'image.bin', 'xex-header.bin']
PACKED = ['airborne-nx.nro', 'airborne.toml', 'README.txt', 'SHA256SUMS.txt',
          'game/default.xex']


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
    'airborne_scene_resolution = "1280x720"\n', encoding='utf-8')

# 4. Build ID and README.
nro_sha = sha256(bundle / 'airborne-nx.nro')
dirty = '-dirty' if git('status', '--porcelain') else ''
build_id = f'{git("rev-parse", "--short", "HEAD")}{dirty}+{nro_sha[:12]}'
(bundle / 'README.txt').write_text(f"""AirborneNX (Medal of Honor: Airborne) для Nintendo Switch
Сборка: {build_id}
Дата: {date.today().isoformat()}

Рантайм ReXGlue, нативный рендерер на Vulkan (NVK). Язык игры: немецкий.
Разрешение сцены задаётся в airborne.toml (airborne_scene_resolution).

Установка (SD-карта):
  /switch/airborne-nx/airborne-nx.nro      - приложение
  /switch/airborne-nx/game/MOHAGame        - файлы игры (уже на месте)
  /switch/airborne-nx/game/default.xex     - исполняемый файл игры
  /switch/airborne-nx/result.txt           - лог (создаётся при запуске)

Обновление: распакуйте папку `switch` из airborne-nx.zip в корень SD-карты
с заменой файлов. Файлы игры при этом остаются.

Запуск: режим перехвата тайтла - удерживайте R при запуске любой
установленной игры, откроется hbmenu; выберите airborne-nx.

Что прислать обратно:
  1. файл result.txt;
  2. короткое описание: появился ли титульный экран / интро,
     на каком месте произошёл вылет или зависание.
""", encoding='utf-8')

# 5. Checksums.
sums = ['airborne-nx.nro', 'airborne.toml', 'README.txt', 'game/default.xex']
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
