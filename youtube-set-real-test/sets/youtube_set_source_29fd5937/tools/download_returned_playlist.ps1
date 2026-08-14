[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$PlaylistUrl,
  [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$ytDlp = Get-Command yt-dlp -ErrorAction SilentlyContinue
if (-not $ytDlp) { throw 'yt-dlp was not found on PATH. Install yt-dlp and retry.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$log = Join-Path $OutputDirectory 'download_returned_playlist.log'
$template = Join-Path $OutputDirectory '%(title)s [%(id)s].%(ext)s'
& $ytDlp.Source '--no-part' '--format' 'bestvideo[height=1080]/bestvideo[height<=1080]' '--output' $template $PlaylistUrl 2>&1 | Tee-Object -FilePath $log
if ($LASTEXITCODE -ne 0) { throw "yt-dlp failed with exit code $LASTEXITCODE" }
Write-Output "Downloads complete. Recovery reads embedded Video Set metadata, not names or playlist order. Log: $log"
