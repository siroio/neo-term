# Independent Terminal Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development to implement and review the tasks in this chat. User authorized implementation; do not create commits or worktrees.

**Goal:** Remove libvterm and deliver event-driven ConPTY output through an independently implemented terminal engine and dirty-row notifications.
**Architecture:** Dedicated pipe threads feed a C++ terminal state machine. It records changed rows at mutation time; snapshots and JSON serialization touch those rows only. Emacs renders changed rows without fixed waits.
**Tech Stack:** Windows x64 C++17, ConPTY, Emacs Lisp, official Unicode data.
**Spec:** ../specs/2026-10-09-independent-terminal-design.md

## Global Constraints

- No libvterm source, headers, link objects, or runtime dependencies in the product.
- Preserve Unicode widths, colors, terminal modes, input, mouse, shell integration, history and resize contracts.
- No commits, pushes, external package installation, or new worktrees.
- Use independent code and official Unicode data rather than copying a terminal library.

## Review Focus

- VT/UTF-8 fragmentation must produce the same state as an unsplit stream.
- Malformed and oversized controls must recover without unbounded allocation.
- Idle terminals must wait for events; cursor-only updates must not scan or serialize cells.
- Resize and alternate-screen transitions must preserve cursor and wide-cell boundaries.
- Large output must preserve history tail, remain interruptible and exit without blocking Emacs.

### Task 1: Independent terminal state machine

Files: native/terminal.hpp, native/terminal.cpp, native/unicode-width.hpp, tests/terminal-test.cpp.
Interface: Terminal(int cols,int rows,std::function<void(const std::string&)> output); feed(std::string_view); resize(int cols,int rows); clear_screen(); command(const Command&); shell_notification(const std::string&); bool has_updates() const; Screen snapshot().
- [x] Write unit tests for fragmented input, colors/cursor/editing, Unicode, alternate screen, history/reflow, key replies and dirty rows; confirm initial failure.
- [x] Implement own parser and terminal state; generate Unicode intervals from official data.
- [x] Compile with /W4 /WX and run terminal tests.
- [x] Review correctness and bounded input handling.

### Task 2: Dirty-row encoding

Files: native/backend.hpp, native/screen-encoder.hpp, tests/native-test.cpp.
Interface: Screen::incremental and Screen::changed_rows; unchanged lines remain empty in incremental snapshots.
- [x] Add tests proving initial full screen, changed-row updates and cursor-only frames without unchanged cell serialization.
- [x] Encode only changed rows for incremental snapshots; retain legacy full snapshots used by existing contract tests.
- [x] Run native tests and review resize/reset behavior.

### Task 3: ConPTY integration and distribution

Files: native/conpty.cpp, native/module.cpp, build.ps1, test.ps1, README.md, vendor/.
- [x] Replace libvterm state and callbacks with Terminal while retaining threaded pipe IO and session lifecycle.
- [x] Replace periodic worker wake with output/input/child-exit/shell-notification events.
- [x] Build independent terminal tests and DLL; remove libvterm product sources and link targets.
- [x] Run CLI, ERT, DLL, GUI and real-shell input benchmarks; diagnose failures rather than weaken tests.
- [x] Review complete diff, document precise limitations and confirm no libvterm dependencies.

## Final verification (2026-10-09)

- Final independent DLL rebuild: native and terminal unit tests PASS with /W4 /WX.
- test.ps1 -Gui: CLI 40/40, ERT 48/48, DLL integration 20/20, byte compile and GUI PASS. No Vim process is used. Actual Emacs keymaps drive Unicode input, repeated resize and copy.
- GUI Emacs / PowerShell keymap-to-redisplay benchmark, 26 characters per run: medians 8.42, 8.85, 8.00 ms; maxima 19.98, 17.18, 30.65 ms. Physical keyboard delivery and scanout are excluded.
- Independent review fixes: stop wake synchronization, named-pipe connect event, blank-line reflow, modified-key identity, and 16-codepoint cell bound. Excess combining marks do not advance the cursor; narrow/wide recovery regressions pass.
- libvterm sources, headers, build objects and tests removed from product inputs. Rebuilt distribution DLL uses the independent engine.
- Smart Brain storage deferred: target project has not been specified. No commit or push made.
