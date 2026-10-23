param([switch]$BuildTools, [switch]$Analyse)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Push-Location $projectRoot
$previousPath = $env:PATH
try {
    $env:PATH = "$projectRoot\tools\toolchain\llvm-mingw-20261020-ucrt-x86_64\bin;$projectRoot\tools\toolchain\venv\Scripts;$env:PATH"
    if ($BuildTools) {
        cmake -S tools/XenonRecomp -B tools/XenonRecomp/build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' '-DCMAKE_CXX_FLAGS=-include cstdlib'
        if ($LASTEXITCODE) { throw 'Configure failed' }
        cmake --build tools/XenonRecomp/build --target XenonAnalyse XenonRecomp -j 8
        if ($LASTEXITCODE) { throw 'Build failed' }
        clang++ -std=c++17 -O2 -include cstdlib -Itools/XenonRecomp/XenonUtils -Itools/XenonRecomp/thirdparty/simde tools/inspect_xex.cpp tools/XenonRecomp/build/XenonUtils/libXenonUtils.a tools/XenonRecomp/build/thirdparty/disasm/libdisasm.a -o tools/inspect_xex.exe
        if ($LASTEXITCODE) { throw 'Inspector build failed' }
    }
    if ($Analyse) {
        & tools/inspect_xex.exe extracted/base/default.xex analysis/image
        if ($LASTEXITCODE) { throw 'XEX load failed' }
        & tools/XenonRecomp/build/XenonAnalyse/XenonAnalyse.exe extracted/base/default.xex analysis/airborne_switch_tables.toml *> analysis/xenon_analyse.log
        if ($LASTEXITCODE) { throw 'Analysis failed' }
    }
    python tools/recover_airborne_switches.py
    if ($LASTEXITCODE) { throw 'Airborne switch validation failed' }
    New-Item -ItemType Directory -Force generated/ppc | Out-Null
    # Generation can shrink the unit count. Remove only the previous numbered
    # units so old trailing files cannot be compiled into the next build.
    $generatedRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'generated/ppc'))
    Get-ChildItem -LiteralPath $generatedRoot -File | Where-Object { $_.Name -match '^ppc_recomp\.\d+\.cpp$' } | ForEach-Object {
        if ($_.Directory.FullName -ne $generatedRoot) { throw 'Unexpected generated output path' }
        Remove-Item -LiteralPath $_.FullName
    }
    & tools/XenonRecomp/build/XenonRecomp/XenonRecomp.exe config/airborne.toml tools/XenonRecomp/XenonUtils/ppc_context.h *> analysis/recompile.log
    if ($LASTEXITCODE) { throw 'Recompiler failed' }
    python tools/summarize_recompile.py
    if ($LASTEXITCODE) { throw 'Diagnostic summary failed' }
    Write-Output 'Generation completed. Inspect analysis/recompile_summary.json; exit 0 does not establish correctness.'
} finally {
    $env:PATH = $previousPath
    Pop-Location
}
