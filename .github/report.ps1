param([string]$Log, [string]$Title)
$lines = Get-Content $Log
$errs = $lines | Where-Object { $_ -match '(error|fatal error|CMake Error)' -and $_ -notmatch 'warning' } | Select-Object -Unique -First 10
foreach ($e in $errs) {
  $t = ($e.Trim() -replace '%','%25' -replace "`r",'' -replace "`n",'%0A')
  if ($t.Length -gt 900) { $t = $t.Substring(0,900) }
  Write-Host "::error title=$Title error::$t"
}
$tail = ($lines | Select-Object -Last 25) -join "`n"
$tail = $tail -replace '%','%25' -replace "`r",'' -replace "`n",'%0A'
if ($tail.Length -gt 3500) { $tail = $tail.Substring($tail.Length - 3500) }
Write-Host "::error title=$Title tail::$tail"
