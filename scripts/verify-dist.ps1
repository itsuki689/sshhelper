param([string]$Directory = '')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$Directory) { $Directory = Join-Path $PSScriptRoot '..\dist' }

$root = (Resolve-Path -LiteralPath $Directory).Path
$info = Get-Content -LiteralPath (Join-Path $root 'build-info.json') -Raw | ConvertFrom-Json
if ($info.schema_version -ne 2 -or $info.product -cne 'sshhelper' -or $info.source_commit -notmatch '^[0-9a-f]{40}$' -or
    ($null -ne $info.ci -and $info.ci.head_sha -cne $info.source_commit)) {
    throw 'Invalid distribution provenance.'
}
$names = @('sshhelper.exe', 'sshhelper-tool.exe')
$sums = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $root 'SHA256SUMS')) {
    if ($line -notmatch '^([0-9a-fA-F]{64})  (sshhelper\.exe|sshhelper-tool\.exe)$') { throw 'Invalid SHA256SUMS entry.' }
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
        throw "Distribution checksum mismatch: $name. Download the complete Release ZIP again."
    }
    $status = $records[0].authenticode.status
    if ($status -eq 'NotSigned') {
        $reader = New-Object IO.BinaryReader([IO.File]::OpenRead($file.FullName))
        try {
            $reader.BaseStream.Position = 0x3c; $pe = $reader.ReadUInt32()
            $reader.BaseStream.Position = $pe + 24; $magic = $reader.ReadUInt16()
            if ($magic -notin @(0x10b, 0x20b)) { throw 'Invalid PE header.' }
            $directories = if ($magic -eq 0x20b) { 112 } else { 96 }
            $reader.BaseStream.Position = $pe + 24 + $directories + 4 * 8 + 4
            if ($reader.ReadUInt32() -ne 0) { throw 'EXE signature state differs from manifest.' }
        } finally { $reader.Dispose() }
    } elseif ($status -eq 'Valid') {
        Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security')
        $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
        if ($signature.Status.ToString() -ne 'Valid' -or
            $signature.SignerCertificate.Thumbprint -cne $records[0].authenticode.signer_thumbprint) {
            throw 'Authenticode signer validation failed.'
        }
    } else { throw 'Unsupported signature state.' }
    Write-Output "$hash  $name (Authenticode: $($records[0].authenticode.status))"
}
Write-Output "Verified sshhelper $($info.version), source $($info.source_commit)"
