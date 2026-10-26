$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$files = & git -c "safe.directory=$($project.Replace('\','/'))" -C $project ls-files -- '*.md'
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate public documentation.' }
foreach ($file in $files) {
    $text = [IO.File]::ReadAllText((Join-Path $project $file))
    foreach ($match in [regex]::Matches($text, '(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])')) {
        # Examples use loopback or RFC 5737 documentation ranges, not deployed hosts.
        if ($match.Value -notmatch '^(127\.|192\.0\.2\.|198\.51\.100\.|203\.0\.113\.)') {
            throw "Use an anonymized host or documentation address in $file (literal address omitted)."
        }
    }
}
Write-Output 'Public documentation uses anonymized host examples.'
