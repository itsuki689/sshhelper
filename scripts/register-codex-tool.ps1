param(
    [string]$ExecutablePath = (Join-Path $PSScriptRoot '..\dist\sshhelper-tool.exe'),
    [string]$CredentialsPath = ''
)
$ErrorActionPreference = 'Stop'
$toolPath = (Resolve-Path -LiteralPath $ExecutablePath).Path
if (-not $CredentialsPath) { $CredentialsPath = Join-Path (Split-Path -Parent $toolPath) 'AirSFTP.credentials' }
$credentialFile = [IO.Path]::GetFullPath($CredentialsPath)
if (-not (Get-Command codex -ErrorAction SilentlyContinue)) { throw 'Codex CLI was not found on PATH.' }
& codex mcp add sshhelper-tool -- $toolPath --stdio --credentials $credentialFile
if ($LASTEXITCODE -ne 0) { throw 'Could not register sshhelper-tool in Codex.' }
Write-Host 'sshhelper-tool registered. Reopen the Codex chat or reload MCP connections to load it.'
