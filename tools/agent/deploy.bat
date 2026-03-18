@echo off
net stop AppSandboxAgent 2>nul
taskkill /f /im appsandbox-clipboard.exe 2>nul
taskkill /f /im appsandbox-clipboard-reader.exe 2>nul
taskkill /f /im appsandbox-input.exe 2>nul
timeout /t 2 /nobreak >nul
copy /y "C:\Users\User\Downloads\agent.exe" "C:\Windows\AppSandbox\agent.exe"
copy /y "C:\Users\User\Downloads\appsandbox-clipboard.exe" "C:\Windows\AppSandbox\appsandbox-clipboard.exe"
copy /y "C:\Users\User\Downloads\appsandbox-clipboard-reader.exe" "C:\Windows\AppSandbox\appsandbox-clipboard-reader.exe"
net start AppSandboxAgent
echo Done.
pause
