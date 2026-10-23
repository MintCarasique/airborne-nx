# Runs the PC build of rex/ (built by tools\build_rexglue.ps1 -App) on extracted/base.
#
#   powershell -ExecutionPolicy Bypass -File tools\run_rex.ps1 [-Config RelWithDebInfo] [-Seconds N] [-- extra --cvar=value ...]
#
# With -Seconds the game is stopped after N seconds (unattended boot checks);
# -Screenshot <png> captures the game window (only it) just before stopping it.
# The log goes to rex\run.log; its path is printed at the end.
param([string]$Config = 'RelWithDebInfo', [int]$Seconds = 0, [string]$Screenshot = '',
      [Parameter(ValueFromRemainingArguments = $true)] [AllowEmptyCollection()] [string[]]$Extra = @())
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$rex = Join-Path $root 'rex'
$bin = Join-Path $rex "out\build\win-amd64-$($Config.ToLower())"
$exe = Get-ChildItem $bin -Recurse -Filter airborne.exe | Select-Object -First 1
if (-not $exe) { throw "airborne.exe not found under $bin; build with tools\build_rexglue.ps1 -App" }
Copy-Item (Join-Path $rex 'airborne.toml') $exe.DirectoryName -Force
$log = Join-Path $rex 'run.log'
$arguments = @("--game_data_root=$(Join-Path $root 'extracted\base')", "--log_file=$log") + $Extra
Write-Host "$($exe.FullName) $($arguments -join ' ')"
$process = Start-Process $exe.FullName -ArgumentList $arguments -WorkingDirectory $exe.DirectoryName -PassThru
if ($Seconds -gt 0) {
    if (-not $process.WaitForExit($Seconds * 1000)) {
        if ($Screenshot) {
            # Only the game window (PrintWindow), never the rest of the desktop.
            Add-Type -AssemblyName System.Drawing
            Add-Type -Namespace Native -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
            $process.Refresh()
            $handle = $process.MainWindowHandle
            $rect = New-Object Native.Win+RECT
            if ($handle -ne [IntPtr]::Zero -and [Native.Win]::GetClientRect($handle, [ref]$rect) -and $rect.Right -gt 0) {
                $bitmap = New-Object System.Drawing.Bitmap $rect.Right, $rect.Bottom
                $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
                $dc = $graphics.GetHdc()
                $ok = [Native.Win]::PrintWindow($handle, $dc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
                $graphics.ReleaseHdc($dc)
                $bitmap.Save($Screenshot, [System.Drawing.Imaging.ImageFormat]::Png)
                Write-Host "screenshot: $Screenshot (PrintWindow=$ok)"
            } else { Write-Host 'screenshot: no game window found' }
        }
        Stop-Process $process -Force; Write-Host "stopped after $Seconds s"
    }
    else { Write-Host "exited with $($process.ExitCode)" }
} else {
    $process.WaitForExit(); Write-Host "exited with $($process.ExitCode)"
}
Write-Host "log: $log"
