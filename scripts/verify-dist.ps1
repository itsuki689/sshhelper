param([string]$Directory = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$Directory) { $Directory = Join-Path $PSScriptRoot '..\dist' }

$root = (Resolve-Path -LiteralPath $Directory).Path
$info = Get-Content -LiteralPath (Join-Path $root 'build-info.json') -Raw | ConvertFrom-Json
if ($info.schema_version -ne 1 -or $info.source_commit -notmatch '^[0-9a-f]{40}$' -or
    $info.ci.head_sha -cne $info.source_commit -or $info.ci.conclusion -ne 'success') {
    throw 'Invalid distribution provenance.'
}
$names = @('AirSFTP.exe', 'sshhelper-tool.exe')
$sums = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $root 'SHA256SUMS')) {
    if ($line -notmatch '^([0-9a-fA-F]{64})  (AirSFTP\.exe|sshhelper-tool\.exe)$') { throw 'Invalid SHA256SUMS entry.' }
    if ($sums.ContainsKey($Matches[2])) { throw 'Duplicate SHA256SUMS entry.' }
    $sums[$Matches[2]] = $Matches[1].ToLowerInvariant()
}
if (@($info.files).Count -ne 2 -or $sums.Count -ne 2) { throw 'Expected exactly two executables.' }
foreach ($name in $names) {
    $records = @($info.files | Where-Object { $_.name -ceq $name })
    if ($records.Count -ne 1) { throw "Missing or duplicate manifest entry: $name" }
    $file = Get-Item -LiteralPath (Join-Path $root $name)
    # Use the framework directly: a minimal PATH/module environment must work.
    $stream = [IO.File]::OpenRead($file.FullName)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
    if ($records[0].sha256 -cne $hash -or $sums[$name] -cne $hash -or $records[0].size_bytes -ne $file.Length) {
        throw "Distribution checksum mismatch: $name. Download the complete project again."
    }
    Write-Output "$hash  $name"
}
Write-Output "Verified source $($info.source_commit); CI $($info.ci.run_url)"
