# Run from an x64 MSVC developer shell. Diagnostic staging is not a public release.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][string]$TripletDir,
    [Parameter(Mandatory)][string]$RedistDir,
    [string]$StageName = 'diagnostic-runtime'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if ($StageName -notmatch '^[a-zA-Z0-9_.-]+$' -or $StageName -in '.', '..') {
    throw 'StageName must be a single directory name.'
}
$stageRoot = Join-Path $repo 'release/staging'
$stage = [IO.Path]::GetFullPath((Join-Path $stageRoot $StageName))
if (-not $stage.StartsWith(([IO.Path]::GetFullPath($stageRoot) + [IO.Path]::DirectorySeparatorChar), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Staging path escapes release/staging.'
}
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$TripletDir = (Resolve-Path -LiteralPath $TripletDir).Path
$RedistDir = (Resolve-Path -LiteralPath $RedistDir).Path
$deploy = Join-Path $TripletDir 'tools/Qt6/bin/windeployqt.exe'
$dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source
foreach ($required in @($deploy, "$BuildDir/media_storage_gui.exe", "$BuildDir/media_storage.exe", "$BuildDir/media_storage.dll")) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing runtime input: $required" }
}
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -LiteralPath "$BuildDir/media_storage_gui.exe" -Destination "$stage/Reliquary.exe"
Copy-Item -LiteralPath "$BuildDir/media_storage.exe", "$BuildDir/media_storage.dll" -Destination $stage

# windeployqt comes from the exact triplet used for the build. CRT is resolved below.
& $deploy --release --no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw "$stage/Reliquary.exe"
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed: $LASTEXITCODE" }
if (-not (Test-Path "$stage/platforms/qwindows.dll")) { throw 'Missing platforms/qwindows.dll' }
# Qt's deployment scanner may copy Windows' own ICU DLL. Keep that OS component
# on Windows rather than redistributing it without an SDK redistribution grant.
foreach ($file in Get-ChildItem -LiteralPath $stage -File -Filter 'icu*.dll') {
    $systemFile = Join-Path "$env:SystemRoot/System32" $file.Name
    if ((Test-Path -LiteralPath $systemFile) -and
        (Get-FileHash -LiteralPath $file.FullName).Hash -eq
        (Get-FileHash -LiteralPath $systemFile).Hash) {
        Remove-Item -LiteralPath $file.FullName
    }
}

# Resolve the complete imported DLL closure, including deployed Qt plugins.
$sources = @{}
foreach ($directory in @("$TripletDir/bin", "$RedistDir/Microsoft.VC143.CRT", "$RedistDir/Microsoft.VC143.OpenMP")) {
    if (-not (Test-Path -LiteralPath $directory)) { throw "Missing runtime directory: $directory" }
    foreach ($file in Get-ChildItem -LiteralPath $directory -File -Filter '*.dll') { $sources[$file.Name] = $file.FullName }
}
$audited = @{}
$audit = [Collections.Generic.List[string]]::new()
do {
    $pending = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.dll', '.exe' -and -not $audited.ContainsKey($_.FullName) })
    foreach ($file in $pending) {
        $headers = & $dumpbin /headers $file.FullName
        if ($LASTEXITCODE -ne 0 -or -not ($headers -match '8664 machine')) { throw "Not an AMD64 PE: $($file.Name)" }
        $imports = & $dumpbin /dependents $file.FullName
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect imports: $($file.Name)" }
        foreach ($line in $imports) {
            if ($line -notmatch '^\s+([a-zA-Z0-9_.-]+\.dll)\s*$') { continue }
            $dll = $Matches[1]
            $local = Join-Path $stage $dll
            if (Test-Path -LiteralPath $local) { $kind = 'app-local' }
            elseif ($sources.ContainsKey($dll)) {
                Copy-Item -LiteralPath $sources[$dll] -Destination $local
                $kind = 'app-local'
            } elseif ($dll -match '^(api-ms-win-|ext-ms-win-)' -or
                      (Test-Path -LiteralPath (Join-Path "$env:SystemRoot/System32" $dll))) {
                if ($dll -match '^(msvcp|vcruntime|vcomp|concrt)') { throw "MSVC runtime must be app-local: $dll" }
                $kind = 'Windows system'
            } else { throw "Missing dependency: $($file.Name) -> $dll" }
            $audit.Add("$($file.Name) -> $dll [$kind]")
        }
        $audited[$file.FullName] = $true
    }
} while ($pending.Count -gt 0)

$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath "$repo/LICENSE.txt" -Destination $stage
# Include installed dependency notices; never copy SDK headers, metadata or libraries.
foreach ($license in Get-ChildItem -LiteralPath "$TripletDir/share" -Filter copyright -Recurse -File) {
    Copy-Item -LiteralPath $license.FullName -Destination (Join-Path $licenses ($license.Directory.Name + '.txt'))
}
foreach ($source in @('wirehair/wirehair.h', 'CRC.h', 'picosha2.h', 'xxhash.h')) {
    $content = Get-Content -LiteralPath "$repo/src/libs/$source" -Raw
    $notice = [regex]::Match($content, '(?s)/\*.*?\*/').Value
    if (-not $notice) { throw "Missing embedded library notice: $source" }
    $notice | Set-Content -LiteralPath (Join-Path $licenses ((Split-Path $source -Leaf) + '.txt')) -Encoding utf8
}
@'
Reliquary is licensed under GPL-3.0-or-later; see LICENSE.txt.
Installed vcpkg dependency license texts are preserved under licenses/.
Wirehair and other embedded library notices are extracted from their source headers.
Qt translations for Reliquary are embedded at :/i18n/vidstorex_tr.qm.
This diagnostic staging tree is NOT a distributable release: approved icon,
external tool provenance/licenses and release validation must be completed first.
'@ | Set-Content -LiteralPath "$stage/THIRD_PARTY_NOTICES.txt" -Encoding utf8

$bad = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
    $_.Extension -in '.pdb', '.obj', '.lib', '.exp', '.dpapi' -or
    $_.Name -match '^(Qt6.*d\.dll|youtube_oauth_client\.json|youtube_sync_state\.json|cookies\.txt|job_state\.json)$'
})
if ($bad.Count) { throw "Forbidden staging content: $($bad.Name -join ', ')" }
$reportDir = Join-Path $repo 'release/validation'
New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
$audit | Sort-Object -Unique | Set-Content -LiteralPath "$reportDir/dependencies.txt" -Encoding utf8
Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    '{0}  {1}  {2}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.Length, [IO.Path]::GetRelativePath($stage, $_.FullName)
} | Set-Content -LiteralPath "$reportDir/diagnostic-manifest.txt" -Encoding utf8
Write-Output "Diagnostic staging: $stage"
Write-Output "windeployqt: $deploy"
