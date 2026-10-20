# Called from the dedicated Windows runner's tagged checkout; no development tree is reused.
$ErrorActionPreference = 'Stop'
function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments)
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE" }
}
foreach ($name in 'ANDROID_HOME', 'JAVA_HOME', 'MSYS2_ROOT') {
    if (-not [Environment]::GetEnvironmentVariable($name)) { throw "$name is required on the runner." }
}
if ($env:EXPECTED_CERT_SHA256 -notmatch '^[0-9a-fA-F]{64}$') {
    throw 'Set repository variable ANDROID_SIGNING_CERT_SHA256 to the Thor release certificate fingerprint.'
}
$env:PYTHONUNBUFFERED = '1'
$ndkBin = Join-Path $env:ANDROID_HOME 'ndk/27.3.13750724/prebuilt/windows-x86_64/bin'
$env:PATH = "$env:MSYS2_ROOT/ucrt64/bin;$env:MSYS2_ROOT/usr/bin;$ndkBin;$env:PATH"
$python = if ($env:EMERALD_PYTHON) { $env:EMERALD_PYTHON } else { (Get-Command python).Source }
$make = Join-Path $ndkBin 'make.exe'
# Non-NTFS work drives do not report ownership. Trust only this checkout and
# its generated upstream tree, for this process and its children (not globally).
$configCount = if ($env:GIT_CONFIG_COUNT) { [int]$env:GIT_CONFIG_COUNT } else { 0 }
foreach ($relative in '.', 'build/upstream') {
    $safePath = [IO.Path]::GetFullPath((Join-Path $PWD $relative)).Replace('\', '/')
    [Environment]::SetEnvironmentVariable("GIT_CONFIG_KEY_$configCount", 'safe.directory')
    [Environment]::SetEnvironmentVariable("GIT_CONFIG_VALUE_$configCount", $safePath)
    $configCount++
}
$env:GIT_CONFIG_COUNT = "$configCount"
$commit = git rev-parse HEAD
if ($LASTEXITCODE) { throw 'Cannot identify the checked-out commit.' }
$version = if ($env:RELEASE_TAG) { $env:RELEASE_TAG } else { '0.4.0-ci' }
# Validate before any expensive work; manual runs build an artifact without creating a release.
Invoke-Checked $python @('-c', 'from tools.thor_dev import release_version; import sys; print(release_version(sys.argv[1]))', $version)
Invoke-Checked $python @('-m', 'pip', 'install', 'Pillow')
Invoke-Checked $python @('-m', 'unittest', 'discover', '-s', 'tools/tests', '-v')
Invoke-Checked $python @('tools/bootstrap.py')
Invoke-Checked $make @('-C', 'build/upstream', '-j16', 'tools', 'generated', "PYTHON=$python")
Invoke-Checked $python @('3ds_port/tests/voxel_seam_test.py', '--cc', 'gcc')
Invoke-Checked $python @('3ds_port/tests/voxel_terrace_foot_test.py')
Invoke-Checked $python @('3ds_port/tests/gameplay_hooks_test.py', '--tree', 'build/upstream', '--cc', 'gcc')
Invoke-Checked $python @('tools/thor_dev.py', 'build', '--release', '--skip-pack', '--jobs', '16', '--version', $version, '--commit', $commit)
$apk = 'build/Emerald3DS-Thor.apk'
$signer = Join-Path $env:ANDROID_HOME 'build-tools/35.0.0/apksigner.bat'
$details = & $signer verify --verbose --print-certs $apk 2>&1
if ($LASTEXITCODE) { throw 'APK signature verification failed.' }
$fingerprint = [regex]::Match(($details -join "`n"), '(?m)^Signer #1 certificate SHA-256 digest:\s*([0-9a-fA-F]{64})\s*$')
if (-not $fingerprint.Success -or $fingerprint.Groups[1].Value -ine $env:EXPECTED_CERT_SHA256) {
    throw 'APK does not match the Thor release signing certificate.'
}
Invoke-Checked $python @('tools/release_audit.py', '--repo', '.', '--strict')
New-Item -ItemType Directory -Force 'build/release' | Out-Null
Copy-Item -LiteralPath $apk -Destination 'build/release/Emerald3DS-Thor.apk' -Force
$hash = (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $PWD 'build/release/SHA256SUMS.txt'), "$hash  Emerald3DS-Thor.apk`n")
$changelog = Get-Content -LiteralPath 'CHANGELOG.md' -Raw
$heading = [regex]::Escape(($version -replace '^v', '' -split '-')[0])
$notes = [regex]::Match($changelog, "(?ms)^## $heading(?:\s|$)[^\n]*\n(.*?)(?=^## |\z)")
if (-not $notes.Success) {
    $notes = [regex]::Match($changelog, '(?ms)^## Unreleased[^\n]*\n(.*?)(?=^## |\z)')
}
if (-not $notes.Success) { throw 'Add release highlights to CHANGELOG.md before tagging.' }
[IO.File]::WriteAllText((Join-Path $PWD 'build/release/NOTES.md'), $notes.Groups[1].Value.Trim() + "`n")
Write-Output "Verified release certificate: $($fingerprint.Groups[1].Value)"
