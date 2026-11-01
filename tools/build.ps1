param(
    [ValidateSet('coordinator','gateway')][string]$Board = 'coordinator',
    [string]$Port,
    [switch]$Flash,
    [switch]$Monitor,
    [string]$IdfPath = 'C:\Espressif\frameworks\esp-idf-v5.3.1',
    [string]$ToolsPath = 'C:\Espressif',
    [string]$PythonEnv = 'C:\Espressif\python_env\idf5.3_py3.11_env'
)
$ErrorActionPreference = 'Stop'
if (($Flash -or $Monitor) -and -not $Port) { throw 'For flash/monitor, specify -Port COMx.' }
$project = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$python = Join-Path $PythonEnv 'Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) { throw "Missing ESP-IDF Python: $python" }
$env:IDF_TOOLS_PATH = $ToolsPath
$env:IDF_PYTHON_ENV_PATH = $PythonEnv
$env:PATH = "$PythonEnv\Scripts;$env:PATH"
$env:PYTHONUTF8 = '1'
. (Join-Path $IdfPath 'export.ps1')
# Both Kconfig and objdump in ESP-IDF 5.3 need an actual ASCII path on Windows.
# Drive aliases are resolved back to the original path by this toolchain.
$hasher = [System.Security.Cryptography.SHA256]::Create()
try { $hash = [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($project))).Replace('-','').Substring(0,12) }
finally { $hasher.Dispose() }
$stageBase = Join-Path $ToolsPath "lamp-builds\$hash"
$sourceRoot = $project
$target = 'esp32c6'
$appName = 'hue_lamp'
if ($Board -eq 'gateway') {
    $stageBase += '-gateway'
    $sourceRoot = Join-Path $project 'gateway'
    $target = 'esp32'
    $appName = 'lamp_gateway'
}
$stage = if ($Board -eq 'gateway') { Join-Path $stageBase 'gateway' } else { $stageBase }
if ($stage -match '[^\x00-\x7F]') { throw 'Choose an ASCII -ToolsPath for the build cache.' }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
foreach ($name in @('CMakeLists.txt','sdkconfig.defaults','partitions.csv','dependencies.lock','sdkconfig','main')) {
    $source = Join-Path $sourceRoot $name
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $stage -Recurse -Force }
}
Copy-Item -LiteralPath (Join-Path $project 'common') -Destination $stageBase -Recurse -Force
$idf = Join-Path $IdfPath 'tools\idf.py'
$idfArgs = @('-C', $stage, '-B', (Join-Path $stage 'build'), "-DIDF_TARGET=$target")
Write-Host "Building a source copy in $stage"
& $python $idf @idfArgs build
if ($LASTEXITCODE -ne 0) { throw "ESP-IDF build failed with exit code $LASTEXITCODE" }
foreach ($name in @('dependencies.lock','sdkconfig')) {
    if (Test-Path -LiteralPath (Join-Path $stage $name)) {
        Copy-Item -LiteralPath (Join-Path $stage $name) -Destination $sourceRoot -Force
    }
}
$output = Join-Path $project "build\$Board"
foreach ($name in @("$appName.bin","$appName.elf","$appName.map",'flash_args','flasher_args.json',
        'bootloader\bootloader.bin','partition_table\partition-table.bin','ota_data_initial.bin')) {
    $destination = Join-Path $output $name
    New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath (Join-Path "$stage\build" $name) -Destination $destination -Force
}
Write-Host "Firmware copied to $output"
& $python (Join-Path $PSScriptRoot 'package_firmware.py') (Join-Path $output "$appName.bin") (Join-Path $output "$appName.zota")
if ($LASTEXITCODE -ne 0) { throw 'OTA packaging failed' }
if ($Flash -or $Monitor) {
    $idfArgs += @('-p', $Port)
    if ($Flash) { $idfArgs += 'flash' }
    if ($Monitor) { $idfArgs += 'monitor' }
    & $python $idf @idfArgs
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF flash/monitor failed with exit code $LASTEXITCODE" }
}
