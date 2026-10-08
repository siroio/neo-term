@echo off
if not defined PROMPT set "PROMPT=$P$G"
set "PROMPT=$E]133;A$E\%PROMPT%$E]133;B$E\$E]7;file://localhost/$P$E\$E]2;neo-term-cmd;$P$E\"
doskey neo-open="%NEO_TERM_HOST%" --notify open $*
doskey neo-cwd="%NEO_TERM_HOST%" --notify cwd
"%NEO_TERM_HOST%" --notify cwd
