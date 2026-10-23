# AirborneNX

Порт Xbox 360 версии **Medal of Honor: Airborne** на Nintendo Switch (homebrew) через статическую
перекомпиляцию PowerPC → C++ на [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), со слоем Switch из
порта NFS: Most Wanted (`tools/reference-nfsmw`).

> Репозиторий не содержит файлов игры и кода, сгенерированного из неё. Нужен собственный дамп Xbox 360
> (GOD-контейнер с `default.xex`). Игра, извлечённые файлы, сгенерированный C++ и сборки исключены в `.gitignore`.

## Состояние

На PC (D3D12, эмуляция Xenos) игра проходит заставки и показывает титульный экран. Сборки под Switch на ReXGlue
ещё нет. План и открытые вопросы: [docs/ASSESSMENT.md](docs/ASSESSMENT.md); факты с железа:
[docs/HARDWARE_NOTES.md](docs/HARDWARE_NOTES.md).

## Сборка и запуск на PC

Нужны VS Build Tools 2022 с Windows 11 SDK, LLVM (clang 18+) в `C:\Program Files\LLVM`, Python, git;
cmake и ninja берутся из `tools/toolchain/venv`.

```powershell
# GOD-контейнер -> extracted/base (default.xex и MOHAGame/)
python tools/extract_god.py "<дамп>/454107F6/00007000/<id>" extracted/base analysis/files.json
powershell -File tools\build_rexglue.ps1         # SDK + codegen rex/airborne_manifest.toml
powershell -File tools\build_rexglue.ps1 -SkipSdk -SkipCodegen -App -Config RelWithDebInfo
powershell -File tools\run_rex.ps1 -Seconds 60   # лог: rex/run.log
```

## Структура

| Путь | Содержимое |
|---|---|
| `rex/` | проект ReXGlue: манифест, ручные правки анализа (`overrides.toml`, `missed_functions.toml`), приложение |
| `tools/` | извлечение GOD/XEX, сборка и запуск, шейдеры, NVK, SDK devkitPro, упаковка на SD |
| `tools/recompile.ps1` | XenonRecomp/XenonAnalyse — только для анализа (образ XEX, список функций для `missed_functions.py`) |
| `tools/patches/` | изменения к XenonRecomp и XenosRecomp |
| `config/` | конфигурация XenonRecomp и закреплённые версии графических зависимостей |
| `analysis/` | результаты аппаратных прогонов и таблицы образа XEX (сырые логи хранятся локально) |
| `docs/` | план и заметки |
