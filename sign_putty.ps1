# PowerShell Code Signing Script for PuTTY-WebView
param (
    [string[]]$Files
)

$thumbprint = "4765252782FFCE4024630152F54FF11963F94FE2"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

$cert = Get-Item "Cert:\CurrentUser\My\$thumbprint" -ErrorAction SilentlyContinue
if (-not $cert) {
    Write-Host "[ERROR] Code signing certificate ($thumbprint) not found in CurrentUser\My." -ForegroundColor Red
    exit 1
}

$targets = @()
if ($Files -and $Files.Count -gt 0) {
    foreach ($f in $Files) {
        if (Test-Path $f) {
            $targets += (Resolve-Path $f).Path
        }
    }
} else {
    $candidates = @(
        (Join-Path $scriptDir "build-vs\Release\putty_webview.exe"),
        (Join-Path $scriptDir "build-vs\Release\putty.exe"),
        (Join-Path $scriptDir "build-vs\Release\plink.exe"),
        (Join-Path $scriptDir "putty-src\build-vs\Release\putty_webview.exe"),
        (Join-Path $scriptDir "putty_webview.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            $targets += (Resolve-Path $c).Path
        }
    }
}

if ($targets.Count -eq 0) {
    Write-Host "[ERROR] Target executable not found for signing." -ForegroundColor Red
    exit 1
}

foreach ($target in $targets) {
    $sig = Set-AuthenticodeSignature -FilePath $target -Certificate $cert -HashAlgorithm SHA256
    Write-Host "[OK] Signed: $($sig.Path)" -ForegroundColor Green
    Write-Host "     Status: $($sig.Status)"
}
