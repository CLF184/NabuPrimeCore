#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='NabuPrime'"
if ($svc -and $svc.State -eq 'Running') {
    sc.exe stop NabuPrime | Out-Null
    Start-Sleep -Seconds 1
    Write-Output 'service: stopped (CPU7 restored)'
}

$listing = (pnputil /enum-drivers | Out-String) -split "(\r?\n){2,}"
foreach ($block in $listing | Where-Object { $_ -match 'Original Name:\s+nabuprime\.inf' }) {
    $published = [regex]::Match($block, 'Published Name:\s+(\S+)').Groups[1].Value
    $out = pnputil /delete-driver $published /uninstall | Out-String
    Write-Output $out.Trim()
    if ($LASTEXITCODE -notin 0, 3010) { throw "pnputil could not remove $published, exit $LASTEXITCODE" }
}

if (Get-Service -Name NabuPrime -ErrorAction SilentlyContinue) {
    sc.exe delete NabuPrime | Out-Null
    Write-Output 'service: deleted'
}
Write-Output 'NabuPrime removed. No restart needed.'
