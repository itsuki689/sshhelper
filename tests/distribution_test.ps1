$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$verify = Join-Path $project 'scripts\verify-dist.ps1'
$root = Join-Path ([IO.Path]::GetTempPath()) ('airsftp-dist-test-' + [guid]::NewGuid().ToString('N'))
$dist = Join-Path $root 'dist'
[IO.Directory]::CreateDirectory($dist) | Out-Null
function Reject([scriptblock]$Action, [string]$Message) {
    try { & $Action | Out-Null } catch { return }
    throw "Expected rejection: $Message"
}
try {
    foreach ($name in @('AirSFTP.exe', 'sshhelper-tool.exe', 'build-info.json', 'SHA256SUMS')) {
        Copy-Item -LiteralPath (Join-Path $project "dist\$name") -Destination $dist
    }
    & $verify -Directory $dist
    $exe = Join-Path $dist 'AirSFTP.exe'
    $stream = [IO.File]::Open($exe, 'Open', 'Write')
    try { $stream.WriteByte(0) } finally { $stream.Dispose() }
    Reject { & $verify -Directory $dist } 'tampered executable'
    [IO.Directory]::CreateDirectory((Join-Path $root 'scripts')) | Out-Null
    Copy-Item -LiteralPath $verify -Destination (Join-Path $root 'scripts')
    Copy-Item -LiteralPath (Join-Path $project 'start.bat') -Destination $root
    $launchInfo = New-Object Diagnostics.ProcessStartInfo
    $launchInfo.FileName = $env:ComSpec
    $launchInfo.Arguments = '/d /c ""{0}""' -f (Join-Path $root 'start.bat')
    $launchInfo.UseShellExecute = $false; $launchInfo.CreateNoWindow = $true
    $launchInfo.RedirectStandardOutput = $true; $launchInfo.RedirectStandardError = $true
    $launch = [Diagnostics.Process]::Start($launchInfo)
    if (!$launch.WaitForExit(5000)) { $launch.Kill(); throw 'Launch refusal timed out.' }
    if ($launch.ExitCode -eq 0) { throw 'start.bat accepted a modified executable.' }
    Copy-Item -LiteralPath (Join-Path $project 'dist\AirSFTP.exe') -Destination $exe -Force
    $manifest = Join-Path $dist 'build-info.json'
    $info = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $info.ci.head_sha = '0000000000000000000000000000000000000000'
    $info | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifest -Encoding UTF8
    Reject { & $verify -Directory $dist } 'mismatched source and CI head'
    Copy-Item -LiteralPath (Join-Path $project 'dist\build-info.json') -Destination $manifest -Force
    Add-Content -LiteralPath (Join-Path $dist 'SHA256SUMS') -Value ('0' * 64 + '  unexpected.exe')
    Reject { & $verify -Directory $dist } 'extra checksum entry'
    Copy-Item -LiteralPath (Join-Path $project 'dist\SHA256SUMS') -Destination (Join-Path $dist 'SHA256SUMS') -Force
    [IO.File]::Delete((Join-Path $dist 'sshhelper-tool.exe'))
    Reject { & $verify -Directory $dist } 'missing companion executable'
    Write-Output 'Distribution integrity, provenance mismatch, missing file and launch refusal passed.'
} finally {
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (![IO.Path]::GetFullPath($root).StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $root) -notmatch '^airsftp-dist-test-[0-9a-f]{32}$') { throw 'Unsafe fixture cleanup path.' }
    Remove-Item -LiteralPath $root -Recurse -Force
}
