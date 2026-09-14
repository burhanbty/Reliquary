[CmdletBinding()]
param([Parameter(Mandatory)][string]$Executable, [string[]]$Arguments=@(), [Parameter(Mandatory)][string]$LogRoot, [int]$TimeoutSeconds=60)
$ErrorActionPreference='Stop'
$LogRoot=[IO.Path]::GetFullPath($LogRoot)
New-Item -ItemType Directory -Path $LogRoot -Force | Out-Null
$info=[Diagnostics.ProcessStartInfo]::new()
$info.FileName=(Resolve-Path -LiteralPath $Executable).Path
$info.WorkingDirectory=$LogRoot
$info.UseShellExecute=$false
$info.CreateNoWindow=$true
$info.RedirectStandardOutput=$true
$info.RedirectStandardError=$true
foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
foreach($key in @('QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','VCPKG_ROOT','QT_QPA_PLATFORM','QTDIR','QML2_IMPORT_PATH','QT_SCALE_FACTOR','QT_SCREEN_SCALE_FACTORS')){$info.Environment.Remove($key) | Out-Null}
$info.Environment['PATH']="$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
$info.Environment['TEMP']=$LogRoot
$info.Environment['TMP']=$LogRoot
$process=[Diagnostics.Process]::new();$process.StartInfo=$info
if(-not $process.Start()){throw 'Process start failed'}
$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
if(-not $process.WaitForExit($TimeoutSeconds*1000)){$process.Kill($true);throw "Timeout: $Executable"}
$stdout.GetAwaiter().GetResult() | Set-Content "$LogRoot/stdout.txt" -Encoding utf8
$stderr.GetAwaiter().GetResult() | Set-Content "$LogRoot/stderr.txt" -Encoding utf8
$code=$process.ExitCode
"ExitCode=$code`nPATH=$($info.Environment['PATH'])" | Set-Content "$LogRoot/result.txt" -Encoding utf8
$process.Dispose()
if($code -ne 0){throw "$Executable exited with $code; see $LogRoot"}
