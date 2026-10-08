@echo off
setlocal DisableDelayedExpansion
if not defined NEO_TERM_PIPE exit /b 1
if "%~1"=="cwd" goto cwd
if not "%~1"=="open" exit /b 1
if "%~2"=="" exit /b 1
set "file=%~f2"
set "file=%file:\=\\%"
set "line=%~3"
set "column=%~4"
if not defined line set "line=1"
if not defined column set "column=1"
setlocal EnableDelayedExpansion
set "message=51;neo-term;{"file":"!file!","line":!line!,"column":!column!}"
goto send
:cwd
set "directory=%CD%"
setlocal EnableDelayedExpansion
set "message=cwd;!directory!"
:send
set /a attempts=0 >nul
:retry
2>nul >"\\.\pipe\!NEO_TERM_PIPE!" (set /p "=!message!" <nul & exit /b 0)
set /a attempts+=1 >nul
if !attempts! lss 10000 goto retry
exit /b 1
