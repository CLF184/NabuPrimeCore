param(
    [Parameter(Mandatory=$true)][string]$BuildDir,
    [Parameter(Mandatory=$true)][string]$Inf2Cat,
    [Parameter(Mandatory=$true)][string]$SignTool,
    [Parameter(Mandatory=$true)][string]$CertificatePfx,
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$name = 'nabu-prime-1.0.3.0-dynamic-lut'
$package = Join-Path $OutDir $name
New-Item -ItemType Directory -Force -Path $package | Out-Null
Copy-Item -LiteralPath (Join-Path $BuildDir 'NabuPrime.sys') -Destination $package
Copy-Item -LiteralPath (Join-Path $root 'drivers\nabu-prime\NabuPrime.inf') -Destination $package
Copy-Item -LiteralPath (Join-Path $root 'package\install.ps1'),
    (Join-Path $root 'package\uninstall.ps1'),(Join-Path $root 'package\README.txt'),
    (Join-Path $root 'LICENSE') -Destination $package
$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($CertificatePfx)
[IO.File]::WriteAllBytes((Join-Path $package 'NabuDrivers.cer'),
    $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
$driver = Join-Path $package 'NabuPrime.sys'
& $SignTool sign /fd SHA256 /f $CertificatePfx $driver
if ($LASTEXITCODE) { throw "Driver signing failed: $LASTEXITCODE" }
& $Inf2Cat "/driver:$package" /os:10_VB_ARM64,10_CO_ARM64,10_NI_ARM64,10_GE_ARM64 /uselocaltime
if ($LASTEXITCODE) { throw "Catalog generation failed: $LASTEXITCODE" }
$catalog = Join-Path $package 'nabuprime.cat'
& $SignTool sign /fd SHA256 /f $CertificatePfx $catalog
if ($LASTEXITCODE) { throw "Catalog signing failed: $LASTEXITCODE" }
foreach ($file in @($driver,$catalog)) {
    $signature = Get-AuthenticodeSignature -LiteralPath $file
    if ($signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint -or
        $signature.Status -in @('HashMismatch','NotSigned','NotSupported','Incompatible')) {
        throw "Signature check failed for $file : $($signature.Status)"
    }
}
$certificate.Dispose()
$zip = Join-Path $OutDir ($name + '.zip')
Compress-Archive -LiteralPath $package -DestinationPath $zip -Force
Get-FileHash -Algorithm SHA256 -LiteralPath $zip
Write-Output "Test-signed package: $zip"
