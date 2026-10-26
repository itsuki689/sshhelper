param(
    [Parameter(Mandatory = $true)][long]$RunId,
    [Parameter(Mandatory = $true)][long]$ArtifactId,
    [Parameter(Mandatory = $true)][string]$ArchivePath,
    [string]$RunMetadataPath = '',
    [string]$ArtifactMetadataPath = '',
    [string]$Repository = 'itsuki689/sshhelper',
    [string]$Destination = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$Destination) { $Destination = Join-Path $PSScriptRoot '..\dist' }
Add-Type -AssemblyName System.IO.Compression.FileSystem

# The archive may be downloaded with gh or a connected GitHub client. Always
# recheck its identity against GitHub, never infer provenance from its filename.
$api = "https://api.github.com/repos/$Repository"
if ($RunMetadataPath -or $ArtifactMetadataPath) {
    if (!$RunMetadataPath -or !$ArtifactMetadataPath) { throw 'Both saved GitHub API responses are required.' }
    $run = Get-Content -LiteralPath $RunMetadataPath -Raw | ConvertFrom-Json
    $artifact = Get-Content -LiteralPath $ArtifactMetadataPath -Raw | ConvertFrom-Json
} else {
    $headers = @{}
    if ($env:GH_TOKEN) { $headers.Authorization = 'Bearer ' + $env:GH_TOKEN }
    elseif ($env:GITHUB_TOKEN) { $headers.Authorization = 'Bearer ' + $env:GITHUB_TOKEN }
    $run = Invoke-RestMethod -Uri "$api/actions/runs/$RunId" -Headers $headers -TimeoutSec 30
    $artifact = Invoke-RestMethod -Uri "$api/actions/artifacts/$ArtifactId" -Headers $headers -TimeoutSec 30
}
if ($run.status -ne 'completed' -or $run.conclusion -ne 'success' -or
    $run.id -ne $RunId -or $artifact.id -ne $ArtifactId -or
    $run.repository.full_name -cne $Repository -or $run.head_repository.full_name -cne $Repository -or
    $run.path -ne '.github/workflows/windows-ci.yml' -or $run.head_sha -notmatch '^[0-9a-f]{40}$') {
    throw 'Select a successful repository Windows CI run.'
}
if ($artifact.expired -or $artifact.workflow_run.id -ne $RunId -or
    $artifact.workflow_run.repository_id -ne $run.repository.id -or
    $artifact.workflow_run.head_repository_id -ne $run.head_repository.id -or
    $artifact.workflow_run.head_sha -cne $run.head_sha -or $artifact.digest -notmatch '^sha256:([0-9a-f]{64})$') {
    throw 'Artifact does not belong to the selected source/run, is expired, or lacks a SHA256 digest.'
}
$archiveHash = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($artifact.digest -cne "sha256:$archiveHash") { throw 'Downloaded archive does not match the GitHub artifact digest.' }

$stage = Join-Path ([IO.Path]::GetTempPath()) ('airsftp-package-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($stage) | Out-Null
try {
    $zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ArchivePath).Path)
    try {
        # Extract only these exact members; no credentials, logs, traversal or backups enter dist.
        foreach ($name in @('AirSFTP.exe', 'sshhelper-tool.exe', 'ctest-results.xml', 'logs/toolchain.log')) {
            $entries = @($zip.Entries | Where-Object { $_.FullName -ieq $name })
            if ($entries.Count -ne 1 -or $entries[0].FullName -cne $name) { throw "Missing/ambiguous archive member: $name" }
            $target = Join-Path $stage $name
            [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entries[0], $target)
        }
    } finally { $zip.Dispose() }
    [xml]$report = Get-Content -LiteralPath (Join-Path $stage 'ctest-results.xml') -Raw
    $suite = $report.testsuite
    $cases = @($suite.testcase)
    if ([int]$suite.tests -ne $cases.Count -or [int]$suite.failures -ne 0 -or
        [int]$suite.skipped -ne 0 -or [int]$suite.disabled -ne 0 -or $cases.Count -eq 0 -or
        @($cases | Where-Object { $_.status -ne 'run' -or $_.SelectNodes('failure|error|skipped').Count -gt 0 }).Count -gt 0) {
        throw 'The artifact does not contain a fully passing CTest report.'
    }
    foreach ($required in @('host_trust', 'app_host_trust', 'mcp_stdio')) {
        if (@($cases | Where-Object { $_.name -ceq $required }).Count -ne 1) { throw "Required regression missing: $required" }
    }
    $toolchain = Get-Content -LiteralPath (Join-Path $stage 'logs/toolchain.log') -Raw
    function VersionLine([string]$Pattern) {
        $match = [regex]::Match($toolchain, $Pattern, [Text.RegularExpressions.RegexOptions]::Multiline)
        if (!$match.Success) { throw "Toolchain information missing: $Pattern" }
        $match.Value.Trim()
    }
    $files = @('AirSFTP.exe', 'sshhelper-tool.exe') | ForEach-Object {
        $path = Join-Path $stage $_
        [ordered]@{ name = $_; size_bytes = (Get-Item -LiteralPath $path).Length;
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $info = [ordered]@{
        schema_version = 1
        source_commit = $run.head_sha
        source_url = "https://github.com/$Repository/commit/$($run.head_sha)"
        ci = [ordered]@{ repository = $Repository; run_id = $RunId; run_attempt = $run.run_attempt;
            run_url = $run.html_url; head_sha = $run.head_sha; conclusion = $run.conclusion;
            workflow = $run.path; artifact_id = $ArtifactId; artifact_name = $artifact.name;
            artifact_created_at = $artifact.created_at; archive_sha256 = $archiveHash }
        toolchain = [ordered]@{ compiler = (VersionLine '^gcc(?:\.exe)? .+$');
            cmake = (VersionLine '^cmake version .+$'); ninja = (VersionLine '^\d+\.\d+\.\d+\s*$');
            node = (VersionLine '^v\d+\.\d+\.\d+\s*$') }
        tests = [ordered]@{ total = $cases.Count; failures = 0; skipped = 0; names = @($cases | ForEach-Object { $_.name }) }
        files = @($files)
        delivery_commit_lookup = 'git log -1 --format=%H -- dist/build-info.json'
    }
    $utf8 = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText((Join-Path $stage 'build-info.json'), ($info | ConvertTo-Json -Depth 8) + "`n", $utf8)
    $sums = @($files | ForEach-Object { "$($_.sha256)  $($_.name)" }) -join "`n"
    [IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS'), $sums + "`n", $utf8)
    & (Join-Path $PSScriptRoot 'verify-dist.ps1') -Directory $stage
    $destinationPath = [IO.Path]::GetFullPath($Destination).TrimEnd('\', '/')
    if ($destinationPath -eq [IO.Path]::GetPathRoot($destinationPath).TrimEnd('\', '/')) {
        throw 'The distribution destination must not be a drive root.'
    }
    [IO.Directory]::CreateDirectory($destinationPath) | Out-Null
    $backup = Join-Path (Split-Path -Parent $destinationPath) ('build\dist-backups\' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($backup) | Out-Null
    $moved = @()
    $installed = @()
    try {
        foreach ($name in @('AirSFTP.exe', 'sshhelper-tool.exe', 'build-info.json', 'SHA256SUMS')) {
            $target = Join-Path $destinationPath $name
            if (Test-Path -LiteralPath $target) {
                # A mapped Windows EXE can be renamed while it cannot be overwritten.
                [IO.File]::Move($target, (Join-Path $backup $name))
                $moved += $name
            }
        }
        foreach ($name in @('AirSFTP.exe', 'sshhelper-tool.exe', 'build-info.json', 'SHA256SUMS')) {
            Copy-Item -LiteralPath (Join-Path $stage $name) -Destination (Join-Path $destinationPath $name)
            $installed += $name
        }
        & (Join-Path $PSScriptRoot 'verify-dist.ps1') -Directory $destinationPath
    } catch {
        foreach ($name in $installed) { [IO.File]::Delete((Join-Path $destinationPath $name)) }
        foreach ($name in $moved) { [IO.File]::Move((Join-Path $backup $name), (Join-Path $destinationPath $name)) }
        throw
    }
    Write-Output "Previous distribution retained outside dist: $backup"
} finally {
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (![IO.Path]::GetFullPath($stage).StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $stage) -notmatch '^airsftp-package-[0-9a-f]{32}$') { throw 'Unsafe temporary cleanup path.' }
    Remove-Item -LiteralPath $stage -Recurse -Force
}
