[CmdletBinding()]
param([string]$IconFile,[string]$Iscc,[string]$TripletDir,[string]$YtDlp)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$blockers=[Collections.Generic.List[string]]::new()
$head=& git -c "safe.directory=$($repo.Replace('\','/'))" -C $repo rev-parse HEAD
if($LASTEXITCODE -ne 0){throw 'Cannot read HEAD'}
$version=[regex]::Match((Get-Content "$repo/CMakeLists.txt" -Raw),'project\(media_storage VERSION ([0-9.]+)').Groups[1].Value
if($version -ne (Get-Content "$repo/vcpkg.json" -Raw | ConvertFrom-Json).'version-string'){$blockers.Add('Version mismatch')}
if(-not $IconFile){$IconFile=Join-Path $repo 'assets/windows/reliquary.ico'}
if(-not (Test-Path -LiteralPath $IconFile)){$blockers.Add('approved Reliquary icon missing')}else{
 $bytes=[IO.File]::ReadAllBytes((Resolve-Path $IconFile))
 if($bytes.Length -lt 6 -or [BitConverter]::ToUInt16($bytes,2) -ne 1){$blockers.Add('Invalid ICO header')}
}
if(-not $Iscc){
 $command=Get-Command ISCC.exe -ErrorAction SilentlyContinue
 if($command){$Iscc=$command.Source}
 foreach($path in @("${env:ProgramFiles(x86)}/Inno Setup 6/ISCC.exe","$env:ProgramFiles/Inno Setup 6/ISCC.exe","$env:LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe")){if(-not $Iscc -and (Test-Path -LiteralPath $path)){$Iscc=$path}}
}
if(-not $Iscc -or -not (Test-Path -LiteralPath $Iscc)){$blockers.Add('Inno Setup 6 missing')}else{
 $help=& $Iscc /? 2>&1
 if(($help -join "`n") -notmatch 'Inno Setup 6'){$blockers.Add('ISCC is not Inno Setup 6')}
}
if(-not $TripletDir -or -not (Test-Path -LiteralPath "$TripletDir/tools/Qt6/bin/windeployqt.exe")){$blockers.Add('Matching windeployqt missing')}
if(-not $YtDlp -or -not (Test-Path -LiteralPath $YtDlp)){$blockers.Add('Required pinned yt-dlp binary missing')}elseif((Get-FileHash -LiteralPath $YtDlp).Hash -ne '52FE3C26DCF71FBDC85B528589020BB0B8E383155CFA81B64DD447BBE35E24B8'){$blockers.Add('yt-dlp does not match reviewed 2026.07.04 bundle')}
foreach($file in @('LICENSE.txt','packaging/windows/licenses/yt-dlp-THIRD_PARTY_LICENSES.txt','packaging/windows/licenses/python310-LICENSE.txt','packaging/windows/licenses/MSVC-REDIST.txt')){if(-not (Test-Path -LiteralPath "$repo/$file")){$blockers.Add("Missing notice: $file")}}
Write-Output "Version=$version HEAD=$head ISCC=$Iscc Icon=$IconFile"
if($blockers.Count){$blockers | ForEach-Object {Write-Output "BLOCKED: $_"};exit 1}
Write-Output 'Inputs found. package.ps1 still requires source-matched tests, runtime/license audit and install/uninstall validation. Source is committed only after validation.'
