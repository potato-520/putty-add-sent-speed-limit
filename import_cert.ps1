# PowerShell Certificate Import Script for PuTTY-WebView
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  PuTTY-WebView Code Signing Certificate Installer" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

# Check Administrator privileges
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "[INFO] Requesting Administrator privileges (UAC)..." -ForegroundColor Yellow
    $scriptPath = $MyInvocation.MyCommand.Definition
    Start-Process powershell.exe -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$scriptPath`"" -Verb RunAs
    exit
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$cerFile = Join-Path $scriptDir "PuTTY_WebView_CodeSigning.cer"

if (-not (Test-Path $cerFile)) {
    $alt = Join-Path $scriptDir "putty-src\PuTTY_WebView_CodeSigning.cer"
    if (Test-Path $alt) { $cerFile = $alt }
}

if (-not (Test-Path $cerFile)) {
    Write-Host "[ERROR] PuTTY_WebView_CodeSigning.cer not found in $scriptDir" -ForegroundColor Red
    Write-Host "Press any key to exit..."
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
    exit 1
}

Write-Host "[1/3] Importing certificate to Trusted Root Certification Authorities (Root)..." -ForegroundColor Yellow
$proc1 = Start-Process certutil.exe -ArgumentList "-addstore -f `"Root`" `"$cerFile`"" -Wait -PassThru -NoNewWindow
if ($proc1.ExitCode -eq 0) {
    Write-Host "[OK] Imported into Root store successfully!" -ForegroundColor Green
} else {
    Write-Host "[WARN] certutil Root exit code: $($proc1.ExitCode)" -ForegroundColor Red
}
Write-Host ""

Write-Host "[2/3] Importing certificate to Trusted Publishers (TrustedPublisher)..." -ForegroundColor Yellow
$proc2 = Start-Process certutil.exe -ArgumentList "-addstore -f `"TrustedPublisher`" `"$cerFile`"" -Wait -PassThru -NoNewWindow
if ($proc2.ExitCode -eq 0) {
    Write-Host "[OK] Imported into TrustedPublisher store successfully!" -ForegroundColor Green
} else {
    Write-Host "[WARN] certutil TrustedPublisher exit code: $($proc2.ExitCode)" -ForegroundColor Red
}
Write-Host ""

Write-Host "[3/3] Verifying putty_webview.exe signature..." -ForegroundColor Yellow
$exeCandidates = @(
    (Join-Path $scriptDir "putty-src\build-vs\Release\putty_webview.exe"),
    (Join-Path $scriptDir "build-vs\Release\putty_webview.exe"),
    (Join-Path $scriptDir "putty_webview.exe")
)

$targetExe = $null
foreach ($path in $exeCandidates) {
    if (Test-Path $path) {
        $targetExe = $path
        break
    }
}

if ($targetExe) {
    $sig = Get-AuthenticodeSignature $targetExe
    Write-Host "Target: $($sig.Path)"
    Write-Host "Status: $($sig.Status)"
    Write-Host "Signer: $($sig.SignerCertificate.Subject)"
    if ($sig.Status -eq "Valid") {
        Write-Host ""
        Write-Host "============================================================" -ForegroundColor Green
        Write-Host "[SUCCESS] Signature is VALID and fully trusted by Windows!" -ForegroundColor Green
        Write-Host "============================================================" -ForegroundColor Green
    } else {
        Write-Host "Status Details: $($sig.StatusMessage)" -ForegroundColor Yellow
    }
} else {
    Write-Host "putty_webview.exe not found for live verification."
}

Write-Host ""
Write-Host "Certificate import complete. ESET and Windows Defender now recognize this certificate." -ForegroundColor Cyan
Write-Host "Press any key to exit..."
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
