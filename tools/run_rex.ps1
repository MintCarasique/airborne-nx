# Runs the PC build of rex/ (built by tools\build_rexglue.ps1 -App) on extracted/base.
#
#   powershell -ExecutionPolicy Bypass -File tools\run_rex.ps1 [-Config RelWithDebInfo] [-Seconds N]
#       [-ShotsAt 20,30 -ShotDir <dir>] [-- extra --cvar=value ...]
#
# With -Seconds the game is stopped after N seconds (unattended boot checks).
# -ShotsAt captures the game window (only it) at those seconds into
# <ShotDir>\shot_<s>.png. The log goes to rex\run.log; its path is printed at the end.
param([string]$Config = 'RelWithDebInfo', [int]$Seconds = 0, [string]$ShotsAt = '', [string]$ShotDir = '',
      [Parameter(ValueFromRemainingArguments = $true)] [AllowEmptyCollection()] [string[]]$Extra = @())
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$rex = Join-Path $root 'rex'
$bin = Join-Path $rex "out\build\win-amd64-$($Config.ToLower())"
$exe = Get-ChildItem $bin -Recurse -Filter airborne.exe | Select-Object -First 1
if (-not $exe) { throw "airborne.exe not found under $bin; build with tools\build_rexglue.ps1 -App" }
# A string, split here: through powershell -File an int[] arrives as one "20,30" string.
$shots = @($ShotsAt -split ',' | Where-Object { $_ } | ForEach-Object { [int]$_ } | Sort-Object)
if ($shots.Count -and -not $ShotDir) { throw '-ShotsAt needs -ShotDir' }
Copy-Item (Join-Path $rex 'airborne.toml') $exe.DirectoryName -Force
$log = Join-Path $rex 'run.log'
$arguments = @("--game_data_root=$(Join-Path $root 'extracted\base')", "--log_file=$log") + $Extra
Write-Host "$($exe.FullName) $($arguments -join ' ')"

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Native -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
public struct RECT { public int Left, Top, Right, Bottom; }
'@
# Physical pixels: without this a scaled display (125 %) reports a smaller
# client rect and the capture keeps only its top-left part.
[void][Native.Win]::SetProcessDPIAware()

# Only the game window (PrintWindow), never the rest of the desktop.
function Save-Shot([System.Diagnostics.Process]$process, [string]$path) {
    $process.Refresh()
    $handle = $process.MainWindowHandle
    $rect = New-Object Native.Win+RECT
    if ($handle -eq [IntPtr]::Zero -or -not [Native.Win]::GetClientRect($handle, [ref]$rect) -or $rect.Right -le 0) {
        Write-Host "screenshot: no game window for $path"; return
    }
    $bitmap = New-Object System.Drawing.Bitmap $rect.Right, $rect.Bottom
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    $ok = [Native.Win]::PrintWindow($handle, $dc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $graphics.ReleaseHdc($dc)
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose()
    Write-Host "screenshot: $path (PrintWindow=$ok)"
}

$process = Start-Process $exe.FullName -ArgumentList $arguments -WorkingDirectory $exe.DirectoryName -PassThru
$started = [Diagnostics.Stopwatch]::StartNew()
foreach ($at in $shots) {
    $wait = [int]($at * 1000 - $started.ElapsedMilliseconds)
    if ($wait -gt 0 -and $process.WaitForExit($wait)) { break }
    if ($process.HasExited) { break }
    Save-Shot $process (Join-Path $ShotDir "shot_$at.png")
}
if ($Seconds -gt 0) {
    $wait = [Math]::Max(0, [int]($Seconds * 1000 - $started.ElapsedMilliseconds))
    if (-not $process.WaitForExit($wait)) { Stop-Process $process -Force; Write-Host "stopped after $Seconds s" }
    else { Write-Host "exited with $($process.ExitCode)" }
} else {
    $process.WaitForExit(); Write-Host "exited with $($process.ExitCode)"
}
Write-Host "log: $log"
