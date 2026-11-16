# Build, flash over the board's USB-Serial-JTAG port and restart the serial daemon (Windows).
# Usage: tools\flash.ps1 [-Port COM13] [-NoFlash]
# Run it in a shell where ESP-IDF is active (idf.py on the PATH).
param([string]$Port = $env:BV_PORT, [switch]$NoFlash)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
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
