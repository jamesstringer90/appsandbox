@echo off
setlocal
set "ASB_PLATFORM=%~1"
if not defined ASB_PLATFORM set "ASB_PLATFORM=x64"
set "ASB_CONFIGURATION=%~2"
if not defined ASB_CONFIGURATION set "ASB_CONFIGURATION=Release"

msbuild "%~dp0..\..\AppSandbox.sln" /t:agent;appsandbox-input;appsandbox-displays;appsandbox-clipboard;appsandbox-clipboard-reader;appsandbox-audio;appsandbox-accessibility /p:Configuration=%ASB_CONFIGURATION% /p:Platform=%ASB_PLATFORM% /m /nologo
exit /b %ERRORLEVEL%
