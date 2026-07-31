[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$PlaylistUrl,
    [ValidateSet('Chrome', 'Edge', 'Firefox')]
    [string]$UseBrowserCookies
)

$ErrorActionPreference = 'Stop'
$ytDlp = Get-Command yt-dlp -ErrorAction SilentlyContinue
if (-not $ytDlp) {
    throw 'yt-dlp was not found on PATH. Install yt-dlp and retry.'
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$experimentRoot = Split-Path -Parent $scriptRoot
$outputRoot = Join-Path $experimentRoot 'returned_downloads'
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

$commonArgs = @(
    '--yes-playlist',
    '--no-part',
    '--restrict-filenames',
    '--output', (Join-Path $outputRoot '%(title)s [%(id)s].%(ext)s')
)
if ($UseBrowserCookies) {
    # Only the browser name is passed. Cookie values are never printed or saved.
    $commonArgs += @('--cookies-from-browser', $UseBrowserCookies.ToLowerInvariant())
}

$h264Format = 'bestvideo[width=1920][height=1080][vcodec^=avc1]/bestvideo[width=1920][height=1080][vcodec^=h264]'
$fallbackFormat = 'bestvideo[width=1920][height=1080]'
$probeArgs = @('--flat-playlist', '--print', '%(id)s', $PlaylistUrl)
$expectedIds = @('R00', 'R01', 'G04', 'R02', 'R03', 'G05')

try {
    & $ytDlp.Source @commonArgs '--format' $h264Format $PlaylistUrl
    if ($LASTEXITCODE -ne 0) { throw 'H264_SELECTION_FAILED' }
}
catch {
    Write-Warning 'No complete 1920x1080 H.264/avc1 selection was available; falling back to the best 1080p video-only stream. No merge or transcode will be performed.'
    & $ytDlp.Source @commonArgs '--format' $fallbackFormat $PlaylistUrl
    if ($LASTEXITCODE -ne 0) { throw 'yt-dlp playlist download failed.' }
}

$files = Get-ChildItem -LiteralPath $outputRoot -File |
    Where-Object { $_.Extension -in @('.mp4', '.webm', '.mkv') }
$index = @()
foreach ($file in $files) {
    $metadataJson = & $ytDlp.Source '--dump-single-json' '--no-playlist' $file.FullName 2>$null
    $metadata = $null
    try { $metadata = $metadataJson | ConvertFrom-Json } catch { }
    $index += [ordered]@{
        file = $file.Name
        codec = if ($metadata) { $metadata.vcodec } else { 'unknown' }
        width = if ($metadata) { $metadata.width } else { 0 }
        height = if ($metadata) { $metadata.height } else { 0 }
        file_size = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$index | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (
    Join-Path $outputRoot 'returned_downloads_index.json') -Encoding utf8

$names = ($files.Name -join ' ')
foreach ($id in $expectedIds) {
    if ($names -notmatch [regex]::Escape($id)) {
        Write-Warning "Expected case ID $id was not found in downloaded titles."
    }
}
foreach ($file in $files) {
    if (-not ($expectedIds | Where-Object { $file.Name -match [regex]::Escape($_) })) {
        Write-Warning "Extra playlist video: $($file.Name)"
    }
}
Write-Output "Downloaded $($files.Count) video-only files to $outputRoot"
