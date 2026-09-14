param(
    [string]$Cli = '',
    [string]$Libraries = '',
    [string]$Port = ''
)
$ErrorActionPreference = 'Stop'
$sketchRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path # Build the sketch next to this helper.
if (-not $Cli) {
    $installedCli = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($installedCli) { $Cli = $installedCli.Source }
    else { $Cli = Join-Path $env:LOCALAPPDATA 'Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe' }
}
if (-not (Test-Path -LiteralPath $Cli -PathType Leaf)) { throw 'Arduino CLI not found. Pass -Cli with its executable path.' }
if (-not $Libraries) { $Libraries = Join-Path (Split-Path -Parent $sketchRoot) 'libraries' }
foreach ($dependency in @(@('LovyanGFX','1.2.26'),@('ArduinoJson','7.4.3'))) {
    $properties = Join-Path $Libraries ($dependency[0] + '/library.properties')
    if (-not (Test-Path -LiteralPath $properties)) { throw ('Missing library: ' + $dependency[0]) }
    if ((Get-Content -LiteralPath $properties) -notcontains ('version=' + $dependency[1])) { throw ('Expected ' + $dependency[0] + ' ' + $dependency[1] + '. Install the tested version in the supplied library folder.') }
}
$coreJson = & $Cli core list --format json # Pin compilation to the recorded SDK instead of silently using another installed release.
if ($LASTEXITCODE -ne 0) { throw 'Cannot query Arduino cores.' }
$cores = $coreJson | ConvertFrom-Json
if (-not ($cores.platforms | Where-Object { $_.id -eq 'esp32:esp32' -and $_.installed_version -eq '3.3.10' })) { throw 'Install esp32:esp32@3.3.10 before building.' }
$board = 'esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc'
$output = Join-Path $sketchRoot 'build'
& $Cli compile --fqbn $board --libraries $Libraries --build-path $output $sketchRoot # Never modifies shared framework files or configuration secrets.
if ($LASTEXITCODE -ne 0) { throw 'Sketch compilation failed.' }
if ($Port) {
    & $Cli upload --fqbn $board --port $Port --input-dir $output $sketchRoot # Flash only when the caller explicitly chooses a port.
    if ($LASTEXITCODE -ne 0) { throw 'Upload failed.' }
}
