[CmdletBinding()]
param(
 [Parameter(Mandatory)][string]$TripletDir,
 [Parameter(Mandatory)][string]$RedistDir,
 [Parameter(Mandatory)][string]$YtDlp,
 [string]$Iscc,
 [string]$Python='python',
 [string]$BuildDir='build-package-public-x64',
 [string]$TestBuildDir='build-package-final-x64',
 [string]$Toolchain,
 [string]$ExpectedHead,
 [switch]$UseValidatedBuilds
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Set-Location $repo
$sourceDigest=& "$PSScriptRoot/source-digest.ps1"
$head=(& git -c "safe.directory=$($repo.Replace('\','/'))" rev-parse HEAD).Trim()
if($LASTEXITCODE -ne 0){throw 'Cannot read HEAD'}
if($ExpectedHead -and $ExpectedHead -ne $head){throw 'Source HEAD changed'}
$version=[regex]::Match((Get-Content CMakeLists.txt -Raw),'project\(media_storage VERSION ([0-9.]+)').Groups[1].Value
if($version -ne '1.5.0'){throw 'The reviewed dependency inventory targets Reliquary 1.5.0'}
$icon=Join-Path $repo 'assets/windows/reliquary.ico'
if(-not (Test-Path -LiteralPath $icon)){throw 'BLOCKED: approved Reliquary icon missing'}
if(-not $Iscc){
 $cmd=Get-Command ISCC.exe -ErrorAction SilentlyContinue
 if($cmd){$Iscc=$cmd.Source}
 foreach($candidate in @("${env:ProgramFiles(x86)}/Inno Setup 6/ISCC.exe","$env:ProgramFiles/Inno Setup 6/ISCC.exe","$env:LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe")){
  if(-not $Iscc -and (Test-Path -LiteralPath $candidate)){$Iscc=$candidate}
 }
}
if(-not $Iscc -or -not (Test-Path -LiteralPath $Iscc)){throw 'BLOCKED: Inno Setup 6 missing; no automatic installation'}
$innoHelp=& $Iscc /? 2>&1
if(($innoHelp -join "`n") -notmatch 'Inno Setup 6'){throw 'ISCC is not Inno Setup 6'}
if((Get-FileHash -LiteralPath $YtDlp).Hash -ne '52FE3C26DCF71FBDC85B528589020BB0B8E383155CFA81B64DD447BBE35E24B8'){throw 'Unreviewed yt-dlp binary'}
$validation=Join-Path $repo 'release/validation'
New-Item -ItemType Directory -Path $validation -Force | Out-Null
$innoHelp | Set-Content "$validation/iscc-help.txt"
if(-not $UseValidatedBuilds){
 if(-not $Toolchain){throw 'Supply the existing vcpkg toolchain; dependency installation is disabled'}
 foreach($spec in @(@($TestBuildDir,'ON'),@($BuildDir,'OFF'))){
  $directory=$spec[0];$tests=$spec[1]
  if(Test-Path -LiteralPath $directory){throw "Clean build directory already exists: $directory"}
  & cmake -S $repo -B $directory -G 'NMake Makefiles' '-DCMAKE_BUILD_TYPE=Release' '-DBUILD_GUI=ON' "-DBUILD_TESTS=$tests" '-DMEDIA_STORAGE_STATIC_BINARIES=OFF' '-DVCPKG_MANIFEST_INSTALL=OFF' "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" "-DVCPKG_INSTALLED_DIR=$([IO.Path]::GetDirectoryName((Resolve-Path $TripletDir).Path))" "-DRELIQUARY_WINDOWS_ICON=$icon"
  if($LASTEXITCODE -ne 0){throw 'Configure failed'}
  & cmake --build $directory
  if($LASTEXITCODE -ne 0){throw 'Build failed'}
  if($tests -eq 'ON'){
   & ctest --test-dir $directory -j 1 --output-on-failure --output-log "$validation/full-suite-final.log"
   if($LASTEXITCODE -ne 0){throw 'Full suite failed; no retries'}
   @{sourceDigest=$sourceDigest;testLogSha=(Get-FileHash "$validation/full-suite-final.log").Hash;total=502} | ConvertTo-Json | Set-Content "$validation/test-source-stamp.json"
  }
 }
}
$testLog=Get-Content "$validation/full-suite-final.log" -Raw
if($testLog -notmatch '100% tests passed, 0 tests failed out of 502|100% tests passed out of 502'){throw 'Required 502/502 validation is missing'}
$stamp=Get-Content "$validation/test-source-stamp.json" -Raw | ConvertFrom-Json
if($stamp.sourceDigest -ne $sourceDigest -or $stamp.testLogSha -ne (Get-FileHash "$validation/full-suite-final.log").Hash -or $sourceDigest -ne (& "$PSScriptRoot/source-digest.ps1")){throw 'Test evidence is stale for the current source'}
$cache=Get-Content "$BuildDir/CMakeCache.txt" -Raw
if($cache -notmatch 'BUILD_TESTS:BOOL=OFF' -or $cache -notmatch 'CMAKE_BUILD_TYPE:STRING=Release'){throw 'Public build must be Release with test hooks disabled'}
$stageName="Reliquary-$version-x64"
& "$PSScriptRoot/stage-runtime.ps1" -BuildDir $BuildDir -TripletDir $TripletDir -RedistDir $RedistDir -StageName $stageName
$stage=Join-Path $repo "release/staging/$stageName"
Copy-Item -LiteralPath $YtDlp -Destination "$stage/yt-dlp.exe"
Copy-Item -LiteralPath "$BuildDir/vidstorex_tr.qm" -Destination "$stage/translations/vidstorex_tr.qm"
& "$PSScriptRoot/audit-licenses.ps1" -StageDir $stage -TripletDir $TripletDir | ConvertTo-Json -Depth 6 | Set-Content "$validation/license-audit.json"
& $Python "$PSScriptRoot/validate-pe.py" "$stage/Reliquary.exe" --icon $icon --amd64 > "$validation/pe-app.json"
if($LASTEXITCODE -ne 0){throw 'PE/icon audit failed'}
$ver=(Get-Item "$stage/Reliquary.exe").VersionInfo
if($ver.ProductVersion -ne $version -or $ver.FileVersion -ne "$version.0" -or $ver.ProductName -ne 'Reliquary' -or $ver.OriginalFilename -ne 'Reliquary.exe'){throw 'VERSIONINFO mismatch'}
$ver | Select-Object ProductName,FileDescription,ProductVersion,FileVersion,OriginalFilename | ConvertTo-Json | Set-Content "$validation/version-resource.json"
$bad=@(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.pdb','.obj','.lib','.exp','.dpapi' -or $_.Name -match '^(Qt6.*d\.dll|youtube_oauth_client\.json|youtube_sync_state\.json|cookies\.txt|job_state\.json)$' })
if($bad.Count){throw 'Forbidden staging file'}
& "$PSScriptRoot/run-isolated.ps1" -Executable "$stage/Reliquary.exe" -Arguments '--smoke-test' -LogRoot "$validation/stage-public-smoke"
& "$PSScriptRoot/run-isolated.ps1" -Executable "$stage/yt-dlp.exe" -Arguments '--version' -LogRoot "$validation/stage-public-ytdlp"
$run=Join-Path $validation ('candidate-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$run/zip/Reliquary" -Force | Out-Null
Copy-Item -Path "$stage/*" -Destination "$run/zip/Reliquary" -Recurse
$zipName="Reliquary-v$version-portable-x64.zip"
$setupName="Reliquary-Setup-v$version-x64.exe"
Compress-Archive -LiteralPath "$run/zip/Reliquary" -DestinationPath "$run/$zipName" -CompressionLevel Optimal
Expand-Archive -LiteralPath "$run/$zipName" -DestinationPath "$run/extracted"
function Assert-Tree([string]$Target){
 $sourceFiles=@(Get-ChildItem -LiteralPath $stage -Recurse -File)
 foreach($file in $sourceFiles){$relative=[IO.Path]::GetRelativePath($stage,$file.FullName);$dest=Join-Path $Target $relative;if(-not (Test-Path -LiteralPath $dest) -or (Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $dest).Hash){throw "Runtime tree mismatch: $relative"}}
}
Assert-Tree "$run/extracted/Reliquary"
& "$PSScriptRoot/run-isolated.ps1" -Executable "$run/extracted/Reliquary/Reliquary.exe" -Arguments '--smoke-test' -LogRoot "$run/portable-smoke"
& "$PSScriptRoot/run-isolated.ps1" -Executable "$run/extracted/Reliquary/yt-dlp.exe" -Arguments '--version' -LogRoot "$run/portable-ytdlp"
& $Iscc "/DAppVersion=$version" "/DStageDir=$stage" "/DOutputPath=$run" "/DIconFile=$icon" "$PSScriptRoot/Reliquary.iss" > "$run/iscc.log"
if($LASTEXITCODE -ne 0){throw 'Installer compilation failed'}
& $Python "$PSScriptRoot/validate-pe.py" "$run/$setupName" --icon $icon > "$run/pe-installer.json"
if($LASTEXITCODE -ne 0){throw 'Installer icon audit failed'}
$registry='HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{DD6EA603-FC48-40C9-ACCC-15914FE7E5B2}_is1'
foreach($key in @($registry,$registry.Replace('HKCU:','HKLM:'),'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\{DD6EA603-FC48-40C9-ACCC-15914FE7E5B2}_is1')){if(Test-Path $key){throw 'Existing Reliquary installation detected; refusing to replace its registration'}}
$install=Join-Path $run 'installed'
$group='Reliquary Packaging Validation '+[guid]::NewGuid().ToString('N')
$setup=Start-Process -FilePath "$run/$setupName" -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER',"/DIR=`"$install`"","/GROUP=`"$group`"",'/TASKS=""',"/LOG=`"$run/install.log`"") -WindowStyle Hidden -PassThru -Wait
if($setup.ExitCode -ne 0){throw "Installer failed: $($setup.ExitCode)"}
try {
 Assert-Tree $install
 $registration=Get-ItemProperty $registry
 if($registration.DisplayName -ne 'Reliquary' -or $registration.DisplayVersion -ne $version -or $registration.Publisher -ne 'Burhan Talha Yazıcı / BTY'){throw 'Installed Apps metadata mismatch'}
 $registration | Select-Object DisplayName,DisplayVersion,Publisher,InstallLocation | ConvertTo-Json | Set-Content "$run/installed-metadata.json"
 $shortcut=Join-Path ([Environment]::GetFolderPath('Programs')) "$group/Reliquary.lnk"
 $shell=New-Object -ComObject WScript.Shell
 $link=$shell.CreateShortcut($shortcut)
 if($link.TargetPath -ne "$install\Reliquary.exe"){throw 'Start Menu target mismatch'}
 "Target=$($link.TargetPath)`nIcon=$($link.IconLocation)" | Set-Content "$run/shortcut.txt"
 & "$PSScriptRoot/run-isolated.ps1" -Executable "$install/Reliquary.exe" -Arguments '--smoke-test' -LogRoot "$run/installed-smoke"
 & "$PSScriptRoot/run-isolated.ps1" -Executable "$install/yt-dlp.exe" -Arguments '--version' -LogRoot "$run/installed-ytdlp"
 'PRESERVE_USER_DATA' | Set-Content "$install/user-created-archive.txt"
} finally {
 $uninstaller=Join-Path $install 'unins000.exe'
 if(Test-Path -LiteralPath $uninstaller){$uninstall=Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',"/LOG=`"$run/uninstall.log`"") -WindowStyle Hidden -PassThru -Wait;if($uninstall.ExitCode -ne 0){throw 'Uninstaller failed'}}
}
if(Test-Path "$install/Reliquary.exe"){throw 'Uninstaller left application executable'}
if(Test-Path $registry){throw 'Uninstaller left registration'}
if(Test-Path -LiteralPath $shortcut){throw 'Uninstaller left Start Menu shortcut'}
if((Get-Content "$install/user-created-archive.txt" -Raw).Trim() -ne 'PRESERVE_USER_DATA'){throw 'Uninstaller removed user data'}
$out=Join-Path $repo 'release/out'
New-Item -ItemType Directory -Path $out -Force | Out-Null
foreach($name in @($setupName,$zipName)){if(Test-Path -LiteralPath "$out/$name"){throw "Refusing to overwrite existing public artifact: $name"}}
Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object { '{0}  {1}  {2}' -f (Get-FileHash -LiteralPath $_.FullName).Hash,$_.Length,[IO.Path]::GetRelativePath($stage,$_.FullName) } | Set-Content "$run/release-manifest.txt" -Encoding utf8
foreach($name in @($setupName,$zipName)){Copy-Item -LiteralPath "$run/$name" -Destination $out}
@($setupName,$zipName) | ForEach-Object {'{0}  {1}' -f (Get-FileHash -LiteralPath "$out/$_").Hash,$_} | Set-Content "$out/SHA256SUMS.txt" -Encoding ascii
foreach($line in Get-Content "$out/SHA256SUMS.txt"){$hash,$name=$line -split '  ',2;if((Get-FileHash -LiteralPath "$out/$name").Hash -ne $hash){throw 'Checksum verification failed'}}
Copy-Item "$run/release-manifest.txt" $out
"HEAD=$head`nValidation=$run`nStage=$stage`nUNSIGNED" | Set-Content "$validation/package-result.txt"
Write-Output "Package validation completed: $run"
Write-Output "Unsigned local artifacts: $out"
