@echo off
setlocal
set "ASB_SOURCE=%~1"
if not defined ASB_SOURCE (
    echo Usage: deploy.bat path-to-built-helpers
    exit /b 1
)
set "ASB_HELPERS=appsandbox-agent appsandbox-input appsandbox-displays appsandbox-clipboard appsandbox-clipboard-reader appsandbox-audio appsandbox-accessibility"
for %%F in (%ASB_HELPERS%) do (
    if not exist "%ASB_SOURCE%\%%F.exe" (
        echo Missing "%ASB_SOURCE%\%%F.exe"
        exit /b 1
    )
)
net stop AppSandboxAgent 2>nul
taskkill /f /im appsandbox-clipboard.exe 2>nul
taskkill /f /im appsandbox-clipboard-reader.exe 2>nul
taskkill /f /im appsandbox-input.exe 2>nul
taskkill /f /im appsandbox-audio.exe 2>nul
taskkill /f /im appsandbox-displays.exe 2>nul
taskkill /f /im appsandbox-accessibility.exe 2>nul
timeout /t 2 /nobreak >nul
for %%F in (%ASB_HELPERS%) do (
    copy /y "%ASB_SOURCE%\%%F.exe" "%SystemRoot%\AppSandbox\%%F.exe" >nul
    if errorlevel 1 (
        net start AppSandboxAgent
        exit /b 1
    )
)
net start AppSandboxAgent
if errorlevel 1 exit /b 1
echo Done.
