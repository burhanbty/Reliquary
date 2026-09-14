$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$files=@(& git -c "safe.directory=$($repo.Replace('\','/'))" -C $repo ls-files -- CMakeLists.txt src include tests)
if($LASTEXITCODE -ne 0){throw 'Cannot inventory source'}
$files+=@('assets/windows/reliquary.ico','packaging/windows/Reliquary.rc.in')
$lines=@($files | Sort-Object -Unique | ForEach-Object {$_+' '+(Get-FileHash -LiteralPath (Join-Path $repo $_) -Algorithm SHA256).Hash})
$bytes=[Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
