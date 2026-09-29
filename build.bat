@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0"
set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%build-vs"

echo ======================================================================
echo  PuTTY-WebView Windows Build ^& Auto-Signing Script
echo ======================================================================

:: 1. Locate CMake
set "CMAKE_EXE="
where cmake.exe >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set "CMAKE_EXE=cmake.exe"
) else (
    if exist "C:\Program Files\CMake\bin\cmake.exe" (
        set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"
    ) else (
        echo [ERROR] cmake.exe not found in PATH or standard installation directory.
        echo Please install CMake or add it to PATH.
        exit /b 1
    )
)

:: 2. Parse Targets (default: putty_webview putty plink)
set "TARGETS=%*"
if "%TARGETS%"=="" (
    set "TARGETS=putty_webview putty plink"
)

echo [INFO] Build targets: %TARGETS%
echo [INFO] Output directory: %BUILD_DIR%\Release

:: 3. Pack Web Assets if putty_webview is targeted
echo "%TARGETS%" | findstr /i "putty_webview putty_webkit" >nul
if %ERRORLEVEL% equ 0 (
    echo [INFO] Packing Web assets...
    set "PYTHON_CMD="
    python --version >nul 2>nul
    if !ERRORLEVEL! equ 0 (
        set "PYTHON_CMD=python"
    ) else (
        py --version >nul 2>nul
        if !ERRORLEVEL! equ 0 (
            set "PYTHON_CMD=py"
        )
    )

    if defined PYTHON_CMD (
        !PYTHON_CMD! "%SCRIPT_DIR%windows\webview\pack_assets.py" "%SCRIPT_DIR%windows\webview\web" "%SCRIPT_DIR%windows\webview\webview_assets.zip"
        if !ERRORLEVEL! neq 0 (
            echo [ERROR] Failed to pack webview assets.
            exit /b 1
        )
    ) else (
        echo [WARNING] python not found. Checking existing webview_assets.zip...
        if not exist "%SCRIPT_DIR%windows\webview\webview_assets.zip" (
            echo [ERROR] webview_assets.zip is missing and python is not available to create it.
            exit /b 1
        )
    )
)

:: 4. CMake Configure
echo [INFO] Configuring CMake with Visual Studio 17 2022 (x64)...
"%CMAKE_EXE%" -S "%SCRIPT_DIR%." -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configuration failed.
    exit /b %ERRORLEVEL%
)

:: 5. CMake Build
echo [INFO] Building Release configuration...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --target %TARGETS%
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Compilation failed.
    exit /b %ERRORLEVEL%
)

:: 6. Code Signing
echo.
echo ======================================================================
echo  Auto-Signing Binaries
echo ======================================================================
if exist "%SCRIPT_DIR%sign_putty.ps1" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%sign_putty.ps1"
    if %ERRORLEVEL% neq 0 (
        echo [WARNING] Code signing encountered an issue. See details above.
    ) else (
        echo [OK] Code signing completed successfully.
    )
) else (
    echo [WARNING] sign_putty.ps1 not found, skipping signing step.
)

echo.
echo ======================================================================
echo  Build and Packaging Successfully Completed!
echo  Binaries: %BUILD_DIR%\Release
echo ======================================================================

exit /b 0
