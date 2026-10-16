param(
    [string]$Version = '1.0.3',
    [string]$WixDir = "$PSScriptRoot\.tools\wix3",
    [switch]$SkipBuild,
    [switch]$TestPackage
)
$ErrorActionPreference = 'Stop'
function Get-ComponentGuid([string]$identity) {
    $hash = [Security.Cryptography.MD5]::Create()
    try { return (New-Object Guid (,$hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($identity)))).ToString() }
    finally { $hash.Dispose() }
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Version must be major.minor.build (MSI format).' }
$root = $PSScriptRoot
$buildDir = Join-Path $root 'build-vs\Release'
$outputDir = Join-Path $root 'dist'
$productName = 'PuTTY-WebView'
$upgradeCode = '88B6D62D-BFBB-444C-9DC3-F11DFF3B2E33'
if ($TestPackage) {
    $productName = 'PuTTY-WebView-InstallerTest'
    $upgradeCode = '3777FEDD-76D1-4D23-AC8D-1989B6A2909A'
}
if (-not $SkipBuild) {
    & "$root\build.bat" putty_webview
    if ($LASTEXITCODE -ne 0) { throw 'Application build failed.' }
}
foreach ($tool in 'heat.exe','candle.exe','light.exe') {
    if (-not (Test-Path (Join-Path $WixDir $tool))) {
        throw "Missing $tool. Extract WiX 3.14 binaries to $WixDir or pass -WixDir."
    }
}
New-Item -ItemType Directory -Force $outputDir | Out-Null
$workDir = Join-Path $root "build-vs\msi-$productName"
New-Item -ItemType Directory -Force $workDir | Out-Null
$webSource = Join-Path $buildDir 'webview'
if (-not (Test-Path "$webSource\web\index.html")) { throw 'External web resources are missing.' }
$assetFile = Join-Path $workDir 'assets.wxs'
& "$WixDir\heat.exe" dir $webSource -nologo -gg -srd -sreg -dr WEBROOT -cg WebAssets -var var.WebSource -out $assetFile
if ($LASTEXITCODE -ne 0) { throw 'Asset harvesting failed.' }
# Per-user MSI components need HKCU key paths, and directory removal rows.
[xml]$assets = Get-Content -LiteralPath $assetFile
$ns = New-Object System.Xml.XmlNamespaceManager($assets.NameTable)
$ns.AddNamespace('w', 'http://schemas.microsoft.com/wix/2006/wi')
foreach ($component in $assets.SelectNodes('//w:Component', $ns)) {
    $component.SetAttribute('Guid', (Get-ComponentGuid "$productName/$($component.File.Source)"))
    foreach ($file in $component.SelectNodes('w:File', $ns)) { $file.RemoveAttribute('KeyPath') }
    $reg = $assets.CreateElement('RegistryValue', $ns.LookupNamespace('w'))
    $reg.SetAttribute('Root', 'HKCU')
    $reg.SetAttribute('Key', "Software\PuTTY-WebView\$productName\Assets")
    $reg.SetAttribute('Name', $component.GetAttribute('Id'))
    $reg.SetAttribute('Type', 'integer')
    $reg.SetAttribute('Value', '1')
    $reg.SetAttribute('KeyPath', 'yes')
    [void]$component.AppendChild($reg)
    $remove = $assets.CreateElement('RemoveFolder', $ns.LookupNamespace('w'))
    $remove.SetAttribute('Id', 'remove_' + $component.GetAttribute('Id'))
    $remove.SetAttribute('On', 'uninstall')
    [void]$component.AppendChild($remove)
}
# Heat also emits parent directories that contain no files of their own.
# ICE64 requires removal records for those per-user directories too.
$coveredDirectories = @{}
foreach ($component in $assets.SelectNodes('//w:Component', $ns)) {
    $coveredDirectories[$component.GetAttribute('Directory')] = $true
}
$cleanupComponent = $assets.SelectSingleNode('//w:Component', $ns)
foreach ($directory in $assets.SelectNodes('//w:Directory', $ns)) {
    $directoryId = $directory.GetAttribute('Id')
    if (-not $coveredDirectories.ContainsKey($directoryId)) {
        $remove = $assets.CreateElement('RemoveFolder', $ns.LookupNamespace('w'))
        $remove.SetAttribute('Id', 'remove_parent_' + $directoryId)
        $remove.SetAttribute('Directory', $directoryId)
        $remove.SetAttribute('On', 'uninstall')
        [void]$cleanupComponent.AppendChild($remove)
    }
}
$assets.Save($assetFile)
# MSI UI requires RTF. Escape the existing upstream license verbatim.
$licenseText = [IO.File]::ReadAllText((Join-Path $root 'LICENCE'))
$escaped = $licenseText.Replace('\','\\').Replace('{','\{').Replace('}','\}').Replace("`r",'').Replace("`n",'\par ')
$licenseRtf = Join-Path $workDir 'license.rtf'
[IO.File]::WriteAllText($licenseRtf, "{\rtf1\ansi $escaped}")
$defines = @("-dProductName=$productName", "-dUpgradeCode=$upgradeCode", "-dVersion=$Version",
    "-dSourceDir=$root", "-dBuildDir=$buildDir", "-dWebSource=$webSource", "-dLicenseRtf=$licenseRtf",
    "-dAppGuid=$(Get-ComponentGuid "$productName/Application")", "-dLicenseGuid=$(Get-ComponentGuid "$productName/License")")
& "$WixDir\candle.exe" -nologo -arch x64 -ext WixUtilExtension @defines -out "$workDir\" "$root\windows\webview-installer.wxs" $assetFile
if ($LASTEXITCODE -ne 0) { throw 'MSI compilation failed.' }
$msi = Join-Path $outputDir "$productName-$Version-x64.msi"
& "$WixDir\light.exe" -nologo -ext WixUtilExtension -ext WixUIExtension -out $msi "$workDir\webview-installer.wixobj" "$workDir\assets.wixobj"
if ($LASTEXITCODE -ne 0) { throw 'MSI linking or validation failed.' }
if (-not $TestPackage) {
    & "$root\sign_putty.ps1" -Files $msi
    if ($LASTEXITCODE -ne 0) { Write-Warning 'MSI was built but signing failed.' }
}
Write-Host "MSI: $msi"
