param(
    [string]$Sdk = $env:ANDROID_HOME,
    [string]$Jdk = $env:JAVA_HOME,
    [string]$Gradle = '',
    [string]$Stage = 'C:\Espressif\lamp-builds\94A74550A775-android',
    [switch]$Release,
    [string[]]$Tasks = @('testDebugUnitTest', 'lintDebug', 'assembleDebug')
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (!$Sdk -or !(Test-Path "$Sdk\platforms\android-35\android.jar")) { throw 'Provide -Sdk with Android SDK platform 35.' }
if (!$Jdk -or !(Test-Path "$Jdk\bin\java.exe")) { throw 'Provide -Jdk with JDK 17.' }
if (!$Gradle) { $Gradle = (Get-Command gradle.bat -ErrorAction Stop).Source }
if (!(Test-Path $Gradle)) { throw 'Provide -Gradle with Gradle 8.9 gradle.bat.' }
$stagePath = [System.IO.Path]::GetFullPath($Stage)
if ($stagePath -match '[^\x00-\x7f]') { throw 'Stage must have an ASCII path for Android build tools.' }
New-Item -ItemType Directory -Force -Path $stagePath | Out-Null
$env:JAVA_HOME = $Jdk
$env:ANDROID_HOME = $Sdk
$env:PATH = "$Jdk\bin;$env:PATH"
if ($Release) {
    $keyDirectory = Join-Path $projectRoot '.cache\android-signing'
    $keyFile = Join-Path $keyDirectory 'zlamp.p12'
    $keyConfig = Join-Path $keyDirectory 'signing.json'
    New-Item -ItemType Directory -Force -Path $keyDirectory | Out-Null
    if (!(Test-Path -LiteralPath $keyConfig)) {
        if (Test-Path -LiteralPath $keyFile) { throw 'Signing key exists but password file is missing. Restore signing.json; do not replace the key.' }
        $random = New-Object byte[] 32
        $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
        try { $rng.GetBytes($random) } finally { $rng.Dispose() }
        $secret = [Convert]::ToBase64String($random)
        [IO.File]::WriteAllText($keyConfig, (@{password=$secret} | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    }
    $env:ZLAMP_STORE_PASSWORD = (Get-Content -LiteralPath $keyConfig -Raw | ConvertFrom-Json).password
    if (!(Test-Path -LiteralPath $keyFile)) {
        & "$Jdk\bin\keytool.exe" -genkeypair -keystore $keyFile -storetype PKCS12 -storepass:env ZLAMP_STORE_PASSWORD -alias zlamp -keyalg RSA -keysize 3072 -validity 10000 -dname 'CN=Z-lamp local development' -noprompt
        if ($LASTEXITCODE) { throw 'Signing key generation failed.' }
    }
    $env:ZLAMP_KEYSTORE = $keyFile
    if (!$PSBoundParameters.ContainsKey('Tasks')) { $Tasks = @('testDebugUnitTest', 'lintRelease', 'assembleRelease') }
}
# Synchronize only managed source directories; do not delete build caches or SDK files.
foreach ($part in @('app\src')) {
    $destination = [System.IO.Path]::GetFullPath((Join-Path $stagePath $part))
    if (!$destination.StartsWith($stagePath + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid staging path.' }
    if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath (Join-Path "$projectRoot\android" $part) -Destination $destination -Recurse
}
foreach ($file in @('settings.gradle', 'build.gradle', 'gradle.properties', 'app\build.gradle')) {
    Copy-Item -LiteralPath (Join-Path "$projectRoot\android" $file) -Destination (Join-Path $stagePath $file) -Force
}
$sdkPath = $Sdk.Replace('\', '/')
[IO.File]::WriteAllText("$stagePath\local.properties", "sdk.dir=$sdkPath`n", [Text.UTF8Encoding]::new($false))
Push-Location $stagePath
try {
    & $Gradle --no-daemon --console=plain @Tasks
    if ($LASTEXITCODE) { throw "Android build failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
    if ($Release) { Remove-Item Env:ZLAMP_STORE_PASSWORD, Env:ZLAMP_KEYSTORE -ErrorAction SilentlyContinue }
}
$output = "$projectRoot\build\android"
New-Item -ItemType Directory -Force -Path $output | Out-Null
$variant = if ($Release) { 'release' } else { 'debug' }
$artifact = "$stagePath\app\build\outputs\apk\$variant\app-$variant.apk"
if (Test-Path -LiteralPath $artifact) {
    $metadata = Get-Content -LiteralPath "$stagePath\app\build\outputs\apk\$variant\output-metadata.json" -Raw | ConvertFrom-Json
    $version = $metadata.elements[0].versionName
    if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Unexpected APK version name.' }
    $name = if ($Release) { "Z-lamp-$version.apk" } else { "Z-lamp-$version-debug.apk" }
    Copy-Item -LiteralPath $artifact -Destination "$output\$name" -Force
}
if (Test-Path "$stagePath\app\build\reports") { Copy-Item "$stagePath\app\build\reports" $output -Recurse -Force }
Write-Host "Android artifacts: $output"
