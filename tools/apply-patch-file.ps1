param([Parameter(Mandatory = $true)][string]$PatchPath)

$codexPath = (Get-ChildItem (Join-Path $env:LOCALAPPDATA 'OpenAI\Codex\bin') -Recurse -Filter codex.exe |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
if (-not $codexPath) { throw 'Unable to locate codex.exe.' }
$patchText = Get-Content -LiteralPath $PatchPath -Raw

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $codexPath
$startInfo.ArgumentList.Add('--codex-run-as-apply-patch')
$startInfo.ArgumentList.Add($patchText.TrimEnd())
$startInfo.UseShellExecute = $false
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true

$process = [System.Diagnostics.Process]::Start($startInfo)
$stdout = $process.StandardOutput.ReadToEnd()
$stderr = $process.StandardError.ReadToEnd()
$process.WaitForExit()
if ($stdout) { Write-Output $stdout.TrimEnd() }
if ($stderr) { Write-Error $stderr.TrimEnd() }
exit $process.ExitCode
