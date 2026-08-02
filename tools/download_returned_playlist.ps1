[CmdletBinding()]
param(
    [string]$PlaylistUrl,
    [string]$UrlList,
    [string]$ManifestPath,
    [ValidateSet('Chrome', 'Edge', 'Firefox')]
    [string]$UseBrowserCookies
)

$ErrorActionPreference = 'Stop'
if (-not $PlaylistUrl -and -not $UrlList) {
    throw 'Provide -PlaylistUrl or a CSV -UrlList containing case_id,url columns.'
}
$ytDlp = Get-Command yt-dlp -ErrorAction SilentlyContinue
if (-not $ytDlp) {
    throw 'yt-dlp was not found on PATH. Install yt-dlp and retry.'
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$experimentRoot = Split-Path -Parent $scriptRoot
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $experimentRoot 'manifest.json'
}
$manifest = $null
if (Test-Path -LiteralPath $ManifestPath) {
    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
}

$cookieArgs = @()
if ($UseBrowserCookies) {
    # Only the browser name is passed. Cookie values are never printed or saved.
    $cookieArgs = @('--cookies-from-browser', $UseBrowserCookies.ToLowerInvariant())
}
$h264Format = 'bestvideo[width=1920][height=1080][vcodec^=avc1]/bestvideo[width=1920][height=1080][vcodec^=h264]'
$fallbackFormat = 'bestvideo[width=1920][height=1080]'
$downloadRoots = [System.Collections.Generic.List[string]]::new()

function Invoke-ReturnedDownload {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$OutputTemplate,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $commonArgs = @(
        '--no-part', '--restrict-filenames', '--write-info-json',
        '--output', $OutputTemplate
    ) + $cookieArgs
    & $ytDlp.Source @commonArgs '--format' $h264Format $Url
    if ($LASTEXITCODE -ne 0) {
        Write-Warning 'No complete 1920x1080 H.264/avc1 stream was available; using the best 1080p video-only stream without merge or transcode.'
        & $ytDlp.Source @commonArgs '--format' $fallbackFormat $Url
        if ($LASTEXITCODE -ne 0) { throw "yt-dlp download failed for $Url" }
    }
    if (-not $downloadRoots.Contains($Destination)) {
        $downloadRoots.Add($Destination)
    }
}

if ($UrlList) {
    if (-not $manifest) {
        throw '-UrlList requires a readable experiment manifest for case mapping.'
    }
    $rows = Import-Csv -LiteralPath $UrlList
    foreach ($row in $rows) {
        if (-not $row.case_id -or -not $row.url) {
            throw 'Every URL-list row must contain case_id and url.'
        }
        $case = $manifest.cases | Where-Object case_id -eq $row.case_id
        if (-not $case) { throw "Unknown manifest case: $($row.case_id)" }
        if (@($case).Count -ne 1) { throw "Ambiguous manifest case: $($row.case_id)" }
        $group = if ($case.session_group) { $case.session_group } else { 'returned_downloads' }
        $destination = if ($group -eq 'returned_downloads') {
            Join-Path $experimentRoot $group
        } else {
            Join-Path (Join-Path $experimentRoot 'returned') $group
        }
        $template = Join-Path $destination ("$($row.case_id) [%(id)s].%(ext)s")
        Invoke-ReturnedDownload -Url $row.url -OutputTemplate $template -Destination $destination
    }
} else {
    $destination = Join-Path $experimentRoot 'returned_downloads'
    $template = Join-Path $destination '%(title)s [%(id)s].%(ext)s'
    Invoke-ReturnedDownload -Url $PlaylistUrl -OutputTemplate $template -Destination $destination
}

$files = foreach ($root in $downloadRoots) {
    Get-ChildItem -LiteralPath $root -File |
        Where-Object { $_.Extension -in @('.mp4', '.webm', '.mkv') }
}
$index = @()
foreach ($file in $files) {
    $metadata = $null
    $infoPath = Join-Path $file.DirectoryName ($file.BaseName + '.info.json')
    if (Test-Path -LiteralPath $infoPath) {
        try {
            $metadata = Get-Content -LiteralPath $infoPath -Raw | ConvertFrom-Json
        }
        catch {
            Write-Warning "Could not parse yt-dlp metadata for $($file.Name)."
        }
    }
    $index += [ordered]@{
        file = $file.FullName
        codec = if ($metadata) { $metadata.vcodec } else { 'unknown' }
        width = if ($metadata) { $metadata.width } else { 0 }
        height = if ($metadata) { $metadata.height } else { 0 }
        file_size = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$index | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (
    Join-Path $experimentRoot 'returned_downloads_index.json') -Encoding utf8

if ($manifest) {
    $names = ($files.Name -join ' ')
    foreach ($case in $manifest.cases) {
        if ($names -notmatch [regex]::Escape($case.case_id)) {
            Write-Warning "Expected case ID $($case.case_id) was not found in downloaded filenames."
        }
    }
}
Write-Output "Downloaded $(@($files).Count) video-only files. Metadata index: $(Join-Path $experimentRoot 'returned_downloads_index.json')"
