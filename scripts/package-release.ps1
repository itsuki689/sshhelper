param(
    [string]$BuildDirectory = 'build',
    [string]$OutputRoot = 'build/package',
    [string]$ArchiveDirectory = 'build/release',
    [string]$ReleaseTag = '',
    [switch]$RequireClean
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = Split-Path -Parent $PSScriptRoot
function Hash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path); $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
$source = (& git -c "safe.directory=$($project.Replace('\','/'))" -C $project rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $source -notmatch '^[0-9a-f]{40}$') { throw 'Cannot determine source commit.' }
$dirty = @(& git -c "safe.directory=$($project.Replace('\','/'))" -C $project status --porcelain --untracked-files=no).Count -gt 0
if ($RequireClean -and $dirty) { throw 'A published package must be built from a clean checkout.' }
$build = [IO.Path]::GetFullPath($BuildDirectory)
[xml]$report = Get-Content -LiteralPath (Join-Path $build 'ctest-results.xml') -Raw
$suite = $report.testsuite; $cases = @($suite.testcase)
if ([int]$suite.tests -ne $cases.Count -or [int]$suite.failures -ne 0 -or [int]$suite.skipped -ne 0 -or
    [int]$suite.disabled -ne 0 -or $cases.Count -eq 0 -or
    @($cases | Where-Object { $_.status -ne 'run' -or $_.SelectNodes('failure|error|skipped').Count }).Count) {
    throw 'A complete passing CTest report is required.'
}
foreach ($required in @('host_trust', 'app_host_trust', 'app_paths', 'mcp_stdio')) {
    if (@($cases | Where-Object { $_.name -ceq $required }).Count -ne 1) { throw "Missing regression: $required" }
}
$output = [IO.Path]::GetFullPath($OutputRoot)
if ((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count) { throw 'Package output must be empty.' }
$dist = Join-Path $output 'dist'; $scripts = Join-Path $output 'scripts'
[IO.Directory]::CreateDirectory($dist) | Out-Null
[IO.Directory]::CreateDirectory($scripts) | Out-Null
$files = @(); $version = ''
foreach ($name in @('sshhelper.exe', 'sshhelper-tool.exe')) {
    $inputFile = Get-Item -LiteralPath (Join-Path $build $name)
    if ($inputFile.VersionInfo.ProductName -cne 'sshhelper' -or $inputFile.VersionInfo.OriginalFilename -cne $name) { throw 'Wrong executable branding.' }
    if (!$version) { $version = $inputFile.VersionInfo.ProductVersion }
    if ($version -cne $inputFile.VersionInfo.ProductVersion -or $version -notmatch '^\d+\.\d+\.\d+$') { throw 'Mixed executable versions.' }
    Copy-Item -LiteralPath $inputFile.FullName -Destination $dist
    $path = Join-Path $dist $name
    $signature = Get-AuthenticodeSignature -LiteralPath $path
    $status = $signature.Status.ToString()
    if ($status -notin @('NotSigned', 'Valid')) { throw "Invalid signature: $name ($status)" }
    $files += [ordered]@{ name = $name; size_bytes = (Get-Item -LiteralPath $path).Length; sha256 = (Hash $path);
        authenticode = [ordered]@{ status = $status;
            signer_thumbprint = $(if ($signature.SignerCertificate) { $signature.SignerCertificate.Thumbprint } else { $null }) } }
}
if ($ReleaseTag -and $ReleaseTag -cne "v$version") { throw 'Release tag does not match executable version.' }
$ci = $null
if ($env:GITHUB_ACTIONS -eq 'true') {
    if ($dirty) { throw 'CI package source is dirty.' }
    $ci = [ordered]@{ run_id = $env:GITHUB_RUN_ID; run_attempt = $env:GITHUB_RUN_ATTEMPT;
        run_url = "$env:GITHUB_SERVER_URL/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID";
        head_sha = $source; workflow = $env:GITHUB_WORKFLOW }
}
$toolchainLog = Join-Path $build 'logs\toolchain.log'
if (!(Test-Path -LiteralPath $toolchainLog)) { throw 'Record the current toolchain in build/logs/toolchain.log before packaging.' }
$metadata = [ordered]@{ schema_version = 2; product = 'sshhelper'; version = $version; source_commit = $source;
    source_tree_dirty = $dirty; ci = $ci; toolchain = [IO.File]::ReadAllText($toolchainLog).Trim();
    tests = [ordered]@{ total = $cases.Count; failures = 0; skipped = 0; names = @($cases | ForEach-Object { $_.name }) };
    files = $files }
$utf8 = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $dist 'build-info.json'), ($metadata | ConvertTo-Json -Depth 8) + "`n", $utf8)
[IO.File]::WriteAllText((Join-Path $dist 'SHA256SUMS'), (($files | ForEach-Object { "$($_.sha256)  $($_.name)" }) -join "`n") + "`n", $utf8)
Copy-Item -LiteralPath (Join-Path $project 'start.bat') -Destination $output
foreach ($name in @('verify-dist.ps1', 'register-codex-tool.ps1')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $scripts }
$readme = "sshhelper $version`r`nRun start.bat. Data: %LOCALAPPDATA%\sshhelper (override: --data-dir DIR or SSHHELPER_DATA_DIR).`r`nAuthenticode status is recorded per EXE in dist/build-info.json; this release may be unsigned.`r`nVerify: powershell -NoProfile -File scripts/verify-dist.ps1`r`nSource and CI: dist/build-info.json`r`nDocumentation: https://github.com/itsuki689/sshhelper`r`n"
[IO.File]::WriteAllText((Join-Path $output 'README.txt'), $readme, $utf8)
& (Join-Path $PSScriptRoot 'verify-dist.ps1') -Directory $dist
[IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($ArchiveDirectory)) | Out-Null
$zipName = "sshhelper-v$version-windows-x64.zip"
$zip = Join-Path ([IO.Path]::GetFullPath($ArchiveDirectory)) $zipName
if (Test-Path -LiteralPath $zip) { throw 'Archive already exists; choose a fresh archive directory.' }
Compress-Archive -LiteralPath @((Get-ChildItem -LiteralPath $output).FullName) -DestinationPath $zip
[IO.File]::WriteAllText((Join-Path ([IO.Path]::GetFullPath($ArchiveDirectory)) 'SHA256SUMS'), "$(Hash $zip)  $zipName`n", $utf8)
Copy-Item -LiteralPath (Join-Path $dist 'build-info.json') -Destination (Join-Path $ArchiveDirectory 'build-info.json')
Write-Output "Packaged $zip; Authenticode status is recorded per executable."
