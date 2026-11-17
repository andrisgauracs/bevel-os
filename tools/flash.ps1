# Build, flash over the board's console port and restart the serial daemon (Windows).
# Usage: tools\flash.ps1 [-Port COM13] [-Target esp32p4|esp32s3] [-NoFlash]
# Run it in a shell where ESP-IDF is active (idf.py on the PATH). Without -Target the board that is
# selected now is built; with it, the project switches to that board first (idf.py set-target), which
# reconfigures the build directory. COM13 is the P4 board's USB-Serial-JTAG port; the S3 devkit
# flashes over its own COM port, so pass -Port for that one.
param([string]$Port = $env:BV_PORT, [string]$Target, [switch]$NoFlash)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if ($Target) {
    idf.py set-target $Target 2>&1 | Select-String -Pattern "error|Set target" | ForEach-Object { $_.Line }
    if ($LASTEXITCODE -ne 0) { Write-Output "SET-TARGET FAILED"; exit 1 }
}
if (-not $Port) { $Port = "COM13" }

$out = idf.py build 2>&1
$out | Select-String -Pattern "error|warning: |undefined reference|bevel_os.bin binary size" |
    ForEach-Object { $_.Line.Substring(0, [Math]::Min(260, $_.Line.Length)) } | Sort-Object -Unique
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED"; exit 1 }
if ($NoFlash) { exit 0 }

python tools\bv.py quit *> $null
Start-Sleep -Milliseconds 500
idf.py -p $Port flash 2>&1 | Select-String -Pattern "Hash of data verified|rror|Hard resetting" | Select-Object -Last 5 | ForEach-Object { $_.Line }
New-Item -ItemType Directory -Force build | Out-Null
Set-Content -Path build\serial.log -Value "" -NoNewline
$env:BV_PORT = $Port
Start-Process -FilePath python -ArgumentList "tools\bv.py", "daemon" -WindowStyle Hidden
Start-Sleep -Seconds 4
python tools\bv.py log 40
