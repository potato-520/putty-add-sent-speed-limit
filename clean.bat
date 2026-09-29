@echo off
setlocal

cd /d "%~dp0"
set "BUILD_DIR=%~dp0build-vs"

echo ======================================================================
echo  PuTTY-WebView Clean Script
echo ======================================================================

if exist "%BUILD_DIR%" (
    echo [INFO] Removing "%BUILD_DIR%" ...
    rd /s /q "%BUILD_DIR%"
    if exist "%BUILD_DIR%" (
        echo [ERROR] Failed to completely remove "%BUILD_DIR%". Check if files are open.
        exit /b 1
    ) else (
        echo [OK] "%BUILD_DIR%" successfully deleted.
    )
) else (
    echo [INFO] "%BUILD_DIR%" does not exist, nothing to clean.
)

:: Optionally clean temporary pack assets if desired
if "%1"=="--all" (
    if exist "%~dp0windows\webview\webview_assets.zip" (
        echo [INFO] Removing webview_assets.zip ...
        del /f /q "%~dp0windows\webview\webview_assets.zip"
    )
)

echo.
echo ======================================================================
echo  Clean Completed!
echo ======================================================================

exit /b 0
