param(
    [string]$ExecutablePath = '',
    [string]$CredentialsPath = ''
)
$ErrorActionPreference = 'Stop'
if (!$ExecutablePath) { $ExecutablePath = Join-Path $PSScriptRoot '..\dist\sshhelper-tool.exe' }
$toolPath = (Resolve-Path -LiteralPath $ExecutablePath).Path
if (-not (Get-Command codex -ErrorAction SilentlyContinue)) { throw 'Codex CLI was not found on PATH.' }
if ($CredentialsPath) {
    & codex mcp add sshhelper-tool -- $toolPath --stdio --credentials ([IO.Path]::GetFullPath($CredentialsPath))
} else { & codex mcp add sshhelper-tool -- $toolPath --stdio }
if ($LASTEXITCODE -ne 0) { throw 'Could not register sshhelper-tool in Codex.' }
Write-Host 'sshhelper-tool registered. Reopen the Codex chat or reload MCP connections to load it.'
