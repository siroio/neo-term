@echo off
chcp 65001 >nul
if not defined PROMPT set "PROMPT=$P$G"
set "PROMPT=$E]133;A$E\%PROMPT%$E]133;B$E\$E]7;file://localhost/$P$E\$E]2;neo-term-cmd;$P$E\"
doskey neo-open="%~dp0neo-term-notify.cmd" open $*
doskey neo-cwd="%~dp0neo-term-notify.cmd" cwd
call "%~dp0neo-term-notify.cmd" cwd
