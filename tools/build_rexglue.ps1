# Builds the ReXGlue SDK from tools/reference-nfsmw/sdk for Windows and runs the
# code generator on rex/airborne_manifest.toml.
#
#   powershell -ExecutionPolicy Bypass -File tools\build_rexglue.ps1 [-SkipSdk] [-SkipCodegen] [-App]
#       [-Config Release|RelWithDebInfo|Debug] [-Jobs N]
#
# Needs: VS Build Tools 2022 with the Windows 11 SDK, LLVM (clang 18+, MSVC target)
# in C:\Program Files\LLVM, git, python, and cmake/ninja from tools/toolchain/venv.
# The llvm-mingw toolchain does not work here: the SDK's FFmpeg config and D3D12
# code expect the MSVC CRT.
param([switch]$SkipSdk, [switch]$SkipCodegen, [switch]$App, [string]$Config = 'Release', [int]$Jobs = 12)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ref = Join-Path $root 'tools\reference-nfsmw'
$sdk = Join-Path $ref 'sdk'
$thirdparty = Join-Path $sdk 'thirdparty'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$llvm = 'C:\Program Files\LLVM\bin'
$venv = Join-Path $root 'tools\toolchain\venv\Scripts'
$rexglue = Join-Path $sdk "out\win-amd64\$Config\rexglue.exe"

function Invoke-Msvc([string]$command) {
    cmd /c "`"$vcvars`" >nul 2>nul && set `"PATH=$llvm;$venv;%PATH%`" && $command"
    if ($LASTEXITCODE -ne 0) { throw "failed ($LASTEXITCODE): $command" }
}

if (-not $SkipSdk) {
    # The reference clone is sparse; the SDK build also needs these folders.
    git -C $ref sparse-checkout add sdk/cmake sdk/resources sdk/switch_compat sdk/thirdparty
    if ($LASTEXITCODE -ne 0) { throw 'sparse-checkout failed' }
    # Our fixes to the SDK. The patch is the source of truth: the files it touches
    # are reset to the reference commit and the patch is applied again, so a patch
    # that grew since the last build still applies. Edits to those files that are
    # not in the patch are lost; export them first:
    #   git -C tools/reference-nfsmw diff -- sdk/src > tools/patches/rexglue-airborne.patch
    $patch = Join-Path $root 'tools\patches\rexglue-airborne.patch'
    $touched = git -C $ref apply --numstat $patch | ForEach-Object { ($_ -split "`t")[2] }
    if ($LASTEXITCODE -ne 0) { throw "cannot read $patch" }
    git -C $ref checkout -- @touched
    git -C $ref apply $patch
    if ($LASTEXITCODE -ne 0) { throw "cannot apply $patch" }
    if (-not (Test-Path (Join-Path $thirdparty 'fmt'))) {
        python (Join-Path $ref 'tools\fetch_thirdparty.py')
        if ($LASTEXITCODE -ne 0) { throw 'fetch_thirdparty failed' }
    }
    # fetch_thirdparty.py skips .git, but sdk/thirdparty/CMakeLists.txt checks
    # <submodule>/.git to decide that a submodule is initialized.
    Get-ChildItem $thirdparty -Directory | ForEach-Object {
        $marker = Join-Path $_.FullName '.git'
        if (-not (Test-Path $marker)) { New-Item -ItemType Directory $marker | Out-Null }
    }
    # Upstream symlinks come out of a Windows clone as text files holding the
    # target path (libmspack/cabextract/mspack/*, moltenvk includes). Replace
    # each with a copy of its target.
    Get-ChildItem $thirdparty -Recurse -File -Force |
        Where-Object { $_.Length -le 512 -and $_.FullName -notmatch '\\\.git\\' } | ForEach-Object {
            $text = [IO.File]::ReadAllText($_.FullName)
            # \z, not $: a real file ending in a newline must not match.
            if ($text -notmatch '\A[\w.+-]*(/[\w.+-]+)+\z') { return }
            $target = [IO.Path]::GetFullPath((Join-Path $_.DirectoryName $text))
            if (-not (Test-Path $target)) { return }
            Remove-Item $_.FullName -Force
            Copy-Item $target $_.FullName -Recurse
            Write-Host "symlink $($_.FullName.Substring($thirdparty.Length + 1)) -> $text"
        }
    Invoke-Msvc "cd /d `"$sdk`" && cmake --preset win-amd64"
    Invoke-Msvc "cd /d `"$sdk`" && cmake --build out/build/win-amd64 --config $Config"
}

if (-not $SkipCodegen) {
    Push-Location (Join-Path $root 'rex')
    try {
        # The codegen stamp tracks the manifest and the XEX, not the generator:
        # after a generator change it would report "up to date".
        $stamp = 'generated\default\codegen.stamp'
        if ((Test-Path $stamp) -and (Get-Item $rexglue).LastWriteTime -gt (Get-Item $stamp).LastWriteTime) {
            Remove-Item $stamp
        }
        # Through cmd: Windows PowerShell turns native stderr into terminating errors.
        cmd /c "`"$rexglue`" codegen airborne_manifest.toml > codegen.log 2>&1"
        if ($LASTEXITCODE -ne 0) { Get-Content codegen.log -Tail 30; throw 'codegen failed, see rex/codegen.log' }
        Get-Content codegen.log -Tail 2
    } finally { Pop-Location }
}

if ($App) {
    # The PC build of rex/. It takes the SDK as a subdirectory, so the SDK is
    # compiled again under rex/out with the app's configuration. Both GPU
    # backends are built; the gpu_backend cvar picks one (Vulkan is what the
    # Switch runs, through NVK).
    $rex = Join-Path $root 'rex'
    $preset = "win-amd64-$($Config.ToLower())"
    # The init template's presets omit the -march the SDK presets set; the SDK
    # uses SSSE3 intrinsics and does not build without it.
    Invoke-Msvc ("cd /d `"$rex`" && cmake --preset $preset `"-DREXSDK_DIR=$($sdk -replace '\\','/')`"" +
        " -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2" +
        " -DREXGLUE_USE_D3D12=ON -DREXGLUE_USE_VULKAN=ON")
    Invoke-Msvc "cd /d `"$rex`" && cmake --build out/build/$preset -j $Jobs"
}
