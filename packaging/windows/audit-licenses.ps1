[CmdletBinding()]
param([Parameter(Mandatory)][string]$StageDir,[Parameter(Mandatory)][string]$TripletDir)
$ErrorActionPreference='Stop'
$StageDir=(Resolve-Path -LiteralPath $StageDir).Path
$TripletDir=(Resolve-Path -LiteralPath $TripletDir).Path
$noticeSources=Get-Content "$PSScriptRoot/licenses/sources.json" -Raw | ConvertFrom-Json
foreach($record in $noticeSources){
 $path=Join-Path "$PSScriptRoot/licenses" $record.file
 if(-not (Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path).Hash -ne $record.sha256){throw "Missing or changed reviewed notice: $($record.file)"}
}
$rows=@(
'qtbase|6.10.3|LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only; bundled notices|Qt6*.dll;generic/*.dll;imageformats/*.dll;networkinformation/*.dll;platforms/*.dll;styles/*.dll;tls/*.dll',
'ffmpeg|8.1.1#1|GPL-2.0-or-later (x264 enabled)|avcodec-62.dll;avformat-62.dll;avutil-60.dll;swscale-9.dll;swresample-6.dll',
'x264|0.164.3108#2|GPL-2.0-or-later|libx264-164.dll',
'libsodium|1.0.22|ISC|libsodium.dll',
'brotli|1.2.0|MIT|brotlicommon.dll;brotlidec.dll',
'bzip2|1.0.8#6|bzip2-1.0.6|bz2.dll',
'double-conversion|3.4.0|BSD-3-Clause|double-conversion.dll',
'freetype|2.14.3|FTL OR GPL-2.0-only|freetype.dll',
'harfbuzz|14.2.0#2|MIT|harfbuzz.dll',
'libpng|1.6.58|libpng-2.0|libpng16.dll',
'md4c|0.5.3|MIT|md4c.dll',
'pcre2|10.47|BSD-3-Clause|pcre2-16.dll',
'zlib|1.3.2|Zlib|z.dll')
$inventory=[Collections.Generic.List[object]]::new()
$covered=@{}
$files=Get-ChildItem -LiteralPath $StageDir -Recurse -File
foreach($row in $rows){
 $name,$version,$license,$patterns=$row.Split('|')
 $notice=Join-Path $TripletDir "share/$name/copyright"
 $spdx=Join-Path $TripletDir "share/$name/vcpkg.spdx.json"
 if(-not (Test-Path -LiteralPath $notice)){throw "Missing copyright: $name"}
 $metadata=Get-Content -LiteralPath $spdx -Raw | ConvertFrom-Json
 $port=@($metadata.packages | Where-Object SPDXID -EQ 'SPDXRef-port')[0]
 if($port.versionInfo -ne $version){throw "Unreviewed dependency version: $name $($port.versionInfo)"}
 $actual=@($files | Where-Object { $relative=[IO.Path]::GetRelativePath($StageDir,$_.FullName).Replace('\','/');@($patterns.Split(';') | Where-Object {$relative -like $_}).Count -gt 0 })
 if(-not $actual.Count){throw "Missing component: $name"}
 $bundled=@($actual | ForEach-Object {$relative=[IO.Path]::GetRelativePath($StageDir,$_.FullName).Replace('\','/');$covered[$relative]=$true;$relative})
 Copy-Item -LiteralPath $notice -Destination "$StageDir/licenses/$name.txt"
 $inventory.Add([ordered]@{component=$name;version=$version;files=$bundled;source=$port.downloadLocation;license=$license;notice="licenses/$name.txt";packageAbi=($metadata.packages | Where-Object SPDXID -EQ 'SPDXRef-binary').versionInfo})
}
foreach($file in $files | Where-Object { $_.Name -match '^(MSVCP|VCRUNTIME|VCOMP)' }){
 $covered[$file.Name]=$true
 $inventory.Add([ordered]@{component='Microsoft Visual C++ runtime';version=$file.VersionInfo.FileVersion;files=@($file.Name);source='Visual Studio 2022 VC/Redist/MSVC/14.44.35112/x64 (unmodified)';license='Microsoft Visual Studio redistributable code terms';notice='licenses/MSVC-REDIST.txt'})
}
$ytHash=(Get-FileHash -LiteralPath "$StageDir/yt-dlp.exe" -Algorithm SHA256).Hash
if($ytHash -ne '52FE3C26DCF71FBDC85B528589020BB0B8E383155CFA81B64DD447BBE35E24B8'){throw 'Unreviewed yt-dlp binary'}
$covered['yt-dlp.exe']=$true
$inventory.Add([ordered]@{component='yt-dlp Windows PyInstaller bundle';version='2026.07.04';files=@('yt-dlp.exe');sha256=$ytHash;source='https://github.com/yt-dlp/yt-dlp/tree/997fa140840a08df3938b40da470c78049fef1f6';license='Unlicense for yt-dlp; bundled third-party licenses apply separately';notice='licenses/yt-dlp-THIRD_PARTY_LICENSES.txt and supplemental notices'})
foreach($name in @('Reliquary.exe','media_storage.exe','media_storage.dll')){$covered[$name]=$true}
foreach($file in $files | Where-Object Extension -In '.exe','.dll'){
 $relative=[IO.Path]::GetRelativePath($StageDir,$file.FullName).Replace('\','/')
 if(-not $covered.ContainsKey($relative)){throw "Unmapped bundled executable: $relative"}
}
Copy-Item -Path "$PSScriptRoot/licenses/*" -Destination "$StageDir/licenses" -Force
$inventory.Add([ordered]@{component='Wirehair';version='repository snapshot';files=@('Reliquary.exe','media_storage.exe','media_storage.dll (static linkage)');source='src/libs/wirehair in the Reliquary source revision';license='BSD-3-Clause';notice='licenses/wirehair.h.txt'})
$inventory.Add([ordered]@{component='CRC++; PicoSHA2; xxHash';version='repository snapshot';files=@('Reliquary.exe','media_storage.exe','media_storage.dll (compiled code)');source='src/libs in the Reliquary source revision';license='See preserved source notices';notice='licenses/CRC.h.txt; licenses/picosha2.h.txt; licenses/xxhash.h.txt'})
$inventory | ConvertTo-Json -Depth 6 | Set-Content "$StageDir/dependency-provenance.json" -Encoding utf8
$lines=[Collections.Generic.List[string]]::new()
$lines.Add('Reliquary 1.5.0 - Third-party notices and provenance')
$lines.Add('Reliquary is GPL-3.0-or-later. See LICENSE.txt. This distribution includes GPL-enabled FFmpeg and x264; it is not an LGPL-only FFmpeg build.')
$lines.Add('Dependency versions and vcpkg provenance are in dependency-provenance.json. Full license texts are in licenses/. FreeType is used under its FTL option.')
$lines.Add('Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved.')
$lines.Add('yt-dlp is the unmodified Windows 2026.07.04 bundle, SHA-256 '+$ytHash+'. Its Python 3.10/OpenSSL 1.1 notices supplement the upstream aggregate. setuptools vendored modules have separate notices. Subversions not independently encoded are identified by the enclosing bundle, not guessed.')
$lines.Add('This product includes software developed by the OpenSSL Project for use in the OpenSSL Toolkit (http://www.openssl.org/). This product includes cryptographic software written by Eric Young (eay@cryptsoft.com).')
$lines.Add('Windows system DLLs are not redistributed. Microsoft runtime files are unmodified app-local redistributables. FFmpeg shared libraries serve the native application; standalone ffmpeg.exe/ffprobe.exe are not invoked by Reliquary. Optional yt-dlp postprocessing requiring those tools is not included.')
$lines.Add('Source: https://github.com/burhanbty/Reliquary . Before public publication, provide the matching Reliquary source revision and corresponding dependency sources/build patches alongside the binary download under their applicable licenses. Local packaging does not publish a source offer or create a release URL.')
foreach($item in $inventory){$lines.Add("$($item.component) | $($item.version) | $($item.license) | $($item.notice)")}
$lines | Set-Content "$StageDir/THIRD_PARTY_NOTICES.txt" -Encoding utf8
$inventory
