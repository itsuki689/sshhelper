param([switch]$LeaveRunning)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$root = Join-Path ([IO.Path]::GetTempPath()) ('AirSFTP ' + [char]0x4ea4 + [char]0x4ed8 + ' smoke ' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory((Join-Path $root 'dist')) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $root 'scripts')) | Out-Null
foreach ($name in @('AirSFTP.exe', 'sshhelper-tool.exe', 'build-info.json', 'SHA256SUMS')) {
    Copy-Item -LiteralPath (Join-Path $project "dist\$name") -Destination (Join-Path $root 'dist')
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'verify-dist.ps1') -Destination (Join-Path $root 'scripts')
Copy-Item -LiteralPath (Join-Path $project 'start.bat') -Destination $root
& (Join-Path $root 'scripts\verify-dist.ps1')
$path = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
function StartInfo([string]$Executable, [string]$Arguments) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Executable; $info.Arguments = $Arguments; $info.WorkingDirectory = $root
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.EnvironmentVariables['PATH'] = $path
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    return $info
}
$launch = [Diagnostics.Process]::Start((StartInfo $env:ComSpec ('/d /c ""{0}""' -f (Join-Path $root 'start.bat'))))
if (!$launch.WaitForExit(10000)) { $launch.Kill(); throw 'start.bat timed out.' }
if ($launch.ExitCode -ne 0) { throw ('start.bat failed: ' + $launch.StandardError.ReadToEnd()) }
$gui = $null
$tool = $null
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 100
        $gui = Get-Process AirSFTP -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $root 'dist\AirSFTP.exe') }
    } while (!$gui -and [DateTime]::UtcNow -lt $deadline)
    if (!$gui -or @($gui).Count -ne 1) { throw 'The delivered GUI did not stay running.' }
    Start-Sleep -Seconds 1
    $gui.Refresh()
    if ($gui.HasExited) { throw 'The delivered GUI exited during startup.' }
    $modules = @($gui.Modules | ForEach-Object { $_.FileName })
    if (@($modules | Where-Object { $_ -match '(?i)(mingw|msys|libgcc|libstdc\+\+|libwinpthread)' }).Count) {
        throw 'GUI loaded a development runtime DLL.'
    }
    $info = StartInfo (Join-Path $root 'dist\sshhelper-tool.exe') ('--stdio --credentials "{0}"' -f (Join-Path $root 'missing.credentials'))
    $info.RedirectStandardInput = $true
    $tool = [Diagnostics.Process]::Start($info)
    $tool.StandardInput.WriteLine('{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"portable-smoke","version":"1"}}}')
    $read = $tool.StandardOutput.ReadLineAsync()
    if (!$read.Wait(5000)) { throw 'Delivered MCP initialization timed out.' }
    $reply = $read.Result | ConvertFrom-Json
    if ($reply.result.serverInfo.name -ne 'sshhelper-tool') { throw 'Delivered MCP initialization failed.' }
    $toolModules = @($tool.Modules | ForEach-Object { $_.FileName })
    if (@($toolModules | Where-Object { $_ -match '(?i)(mingw|msys|libgcc|libstdc\+\+|libwinpthread)' }).Count) {
        throw 'MCP loaded a development runtime DLL.'
    }
    $tool.StandardInput.WriteLine('{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"list_servers"}}')
    $read = $tool.StandardOutput.ReadLineAsync()
    if (!$read.Wait(5000)) { throw 'Delivered MCP discovery timed out.' }
    $servers = ($read.Result | ConvertFrom-Json).result.structuredContent.servers
    if (@($servers).Count -ne 1 -or $servers[0].server_id -ne 'local') { throw 'MCP configuration was not isolated.' }
    $tool.StandardInput.Close()
    if (!$tool.WaitForExit(5000) -or $tool.ExitCode -ne 0) { throw 'Delivered MCP did not exit cleanly.' }
    & (Join-Path $root 'scripts\verify-dist.ps1')
    $result = [ordered]@{ fixture = $root; gui_pid = $gui.Id; path = $path; start_bat = 'passed';
        gui_process = 'running'; mcp_initialize = 'passed'; mcp_isolated_discovery = 'passed';
        checksums = 'passed'; gui_modules = $modules; mcp_modules = $toolModules }
    [IO.Directory]::CreateDirectory((Join-Path $project 'build')) | Out-Null
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $project 'build\portable-smoke.json') -Encoding UTF8
    Write-Output ($result | ConvertTo-Json -Depth 5)
} finally {
    if ($tool -and !$tool.HasExited) { $tool.Kill(); $tool.WaitForExit() }
    if (!$LeaveRunning -and $gui -and !$gui.HasExited) { Stop-Process -Id $gui.Id }
    # Keep this isolated fixture and report for inspection; it contains no user profiles.
}
