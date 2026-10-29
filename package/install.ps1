#Requires -RunAsAdministrator
param([int]$LogSeconds = 0, [switch]$NoCertificate)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $NoCertificate) {
    $cer = Join-Path $here 'NabuDrivers.cer'
    foreach ($store in 'Root', 'TrustedPublisher') {
        Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
    }
    Write-Output 'certificate: fork test certificate trusted (Root, TrustedPublisher)'
}
$bcd = bcdedit /enum '{current}' | Out-String
if ($bcd -notmatch 'testsigning\s+Yes') {
    Write-Warning 'Test signing is off, so the driver will not load. Run: bcdedit /set testsigning on, then restart.'
}

$old = Get-CimInstance Win32_SystemDriver -Filter "Name='NabuPrime'"
if ($old) {
    if ($old.State -eq 'Running') { sc.exe stop NabuPrime | Out-Null; Start-Sleep -Seconds 1 }
    if ($old.PathName -notmatch '\\DriverStore\\') {
        sc.exe delete NabuPrime | Out-Null
        Write-Output "old test service removed: $($old.PathName)"
        Start-Sleep -Seconds 1
    }
}

$out = pnputil /add-driver (Join-Path $here 'NabuPrime.inf') /install | Out-String
$code = $LASTEXITCODE
Write-Output $out.Trim()
if ($code -notin 0, 3010, 259) { throw "pnputil failed (exit $code)" }

$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\NabuPrime\Parameters'
if (-not (Test-Path $params)) { throw 'the NabuPrime service was not created' }
Set-ItemProperty -Path $params -Name LogIntervalMs -Value ([int]($LogSeconds * 1000)) -Type DWord
if ($LogSeconds -gt 0) {
    New-Item -ItemType Directory -Force 'C:\nabu\osm' | Out-Null
    Write-Output "log: C:\nabu\osm\prime.log every $LogSeconds s"
}

$start = sc.exe start NabuPrime | Out-String
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='NabuPrime'"
Write-Output ("service: {0}, start {1}, {2}" -f $svc.State, $svc.StartMode, $svc.PathName)
if ($svc.State -ne 'Running') {
    Write-Output $start.Trim()
    Write-Warning 'NabuPrime did not start. Check the sc.exe error above. A device configuration error can mean a disabled, malformed or out-of-range hardware frequency table, or an invalid original level.'
}
