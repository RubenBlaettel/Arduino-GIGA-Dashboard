<#
.SYNOPSIS
  Compile the GIGA dashboard sketch, optionally upload it.

.DESCRIPTION
  Uses the project-pinned LVGL 9.2.2 in lib/, never the global sketchbook version. With -Upload the PC bridge is paused (bridge/.pause)
  so the serial port is free, and resumed afterwards.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\build.ps1
  powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Upload
#>
param(
  [switch]$Upload,
  [string]$Port = ""
)
$ErrorActionPreference = "Stop"

$Root   = Split-Path -Parent $PSScriptRoot
$Sketch = Join-Path $Root "Dashboard"
$Build  = Join-Path $Root "build"
$Fqbn   = "arduino:mbed_giga:giga"
$Pause  = Join-Path $Root "bridge\.pause"

$candidates = @(
  "$env:LOCALAPPDATA\Programs\arduino-ide\resources\app\lib\backend\resources\arduino-cli.exe",
  "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
)
$Cli = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Cli) {
  $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
  if ($cmd) { $Cli = $cmd.Source } else { throw "arduino-cli not found. Install the Arduino IDE 2.x." }
}

$libs = @(
  "--library", (Join-Path $Root "lib\lvgl")
)

# Not in the repository: LVGL (fetched at the pinned version) and the fonts (generated locally).
if (-not (Test-Path (Join-Path $Root "lib\lvgl\lvgl.h"))) {
  throw "LVGL 9.2.2 missing. Run: git clone --depth 1 --branch v9.2.2 https://github.com/lvgl/lvgl.git lib/lvgl"
}
if (-not (Test-Path (Join-Path $Sketch "src\fonts\fonts.h"))) {
  throw "Fonts missing. Run: python tools\build_fonts.py"
}

Write-Host "Compiling $Sketch ..."
& $Cli compile -b $Fqbn @libs --build-path $Build --warnings none $Sketch
if ($LASTEXITCODE -ne 0) { throw "Compile failed ($LASTEXITCODE)" }

if (-not $Upload) { return }

if (-not $Port) {
  $boards = (& $Cli board list --format json | ConvertFrom-Json)
  $list = if ($boards.detected_ports) { $boards.detected_ports } else { $boards }
  $giga = $list | Where-Object { $_.matching_boards.fqbn -contains $Fqbn } | Select-Object -First 1
  $Port = if ($giga) { $giga.port.address } else { "COM6" }
}

Write-Host "Pausing the bridge and uploading to $Port ..."
Set-Content -Path $Pause -Value "upload" -Encoding ascii
try {
  Start-Sleep -Seconds 2   # the bridge checks the pause file every loop and releases the port
  & $Cli upload -b $Fqbn -p $Port --input-dir $Build $Sketch
  if ($LASTEXITCODE -ne 0) { throw "Upload failed ($LASTEXITCODE)" }
} finally {
  Remove-Item $Pause -ErrorAction SilentlyContinue
}
Write-Host "Done. The bridge reconnects on its own."
