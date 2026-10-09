# GUI Emacs input latency profile

Measured on 2026-10-09 with GUI Emacs 31.1, `-Q`, the current independent-engine DLL, bundled ConPTY, and PowerShell (`-NoLogo -NoProfile`). No Vim process was used. Three runs of 26 characters passed through the real Emacs keymap. `tests/benchmark-input.el` now records the stages and ordinary Emacs controls. Raw results are in `build/profile-powershell-{1,2,3}.log`.

| Stage | Range of three run medians |
| --- | --- |
| Key dispatch before module call | 0.043–0.047 ms |
| Module command submission | 0.010 ms |
| Submission return to Emacs output consumption | 0.744–0.746 ms |
| Frame decoding to screen handler | 0.235–0.238 ms |
| Screen handler to rendering start | 0.027 ms |
| Updating Emacs buffer | 0.692–0.706 ms |
| Explicit GUI redisplay | 6.020–6.105 ms |
| Entire input-to-redisplay path | 7.926–8.855 ms |

Stage medians are calculated independently and are not additive. Entire-path maxima were 19.64, 31.27, and 22.10 ms. These are elapsed wall times; occasional scheduling, garbage collection or GUI waits have not been individually attributed.

The same frame and copied screen contents were tested using ordinary Emacs self insertion. Redisplay medians were 3.098–3.131 ms without terminal text properties, and 4.264–4.340 ms retaining display/face properties (read-only protection removed to allow typing). This comparison suggests terminal properties and terminal-specific display state add cost, but does not establish a universal Emacs minimum: the control changes the first line through self insertion, whereas neo-term replaces the changed terminal row and updates its cursor.

The interactive console fixture was also measured. It writes a new diagnostic line for each character, causing row/history changes rather than PowerShell's same-line editing. Total medians were 18.09–33.19 ms, with substantially more variability in rendering. It is not a lower-bound echo benchmark and should not be used to claim the PowerShell path is slower.

The receive interval includes worker scheduling, ConPTY, child processing, VT parsing/encoding, local socket delivery, Emacs event dispatch and the benchmark's `accept-process-output` loop. Native VT processing was not isolated. Physical keyboard delivery, compositor presentation and display scanout are excluded. Explicit redisplay duration is not physical key-to-pixel latency.

Conclusion: command submission already meets 0.2 ms, but the current measured full GUI path does not. Optimizing only ConPTY IPC would leave roughly 6 ms of redisplay plus decoding and buffer update. The next useful investigation is terminal display properties, changed-row replacement and cursor redisplay. No production behavior was changed during this profile. Smart Brain storage remains deferred because its target project is unspecified.

## Follow-up implementation

The user authorized optimizations after the profile. Two changes are now implemented in `neo-term.el`:

- Keep identical leading and trailing cells when updating a row, replacing only its changed cells. Comparisons include Unicode text, native width, colors and prompt roles. Resize/font/layout changes still rebuild the screen.
- Reconcile derived prompt properties on differing property runs. Unchanged prompts and history are no longer cleared and re-marked. Stale markers are still removed when their source roles disappear.

Regression tests first reproduced unchanged prompt property rewrites and deletion of markers inside unchanged row text. The final implementation passes those regressions and the full `test.ps1 -Gui` suite: native/terminal PASS, CLI 40/40, ERT 50/50, DLL integration 20/20, warning-free byte compile and GUI PASS. The GUI round trip covers actual Emacs keymaps, Japanese/emoji/combining input, repeated resize, copy, colors and native-width pixel placement. No Vim process is used.

Three alternating before/after GUI PowerShell trials, 26 characters each, loading equally byte-compiled sources:

| Trial | Before buffer update | After buffer update | Before whole path | After whole path |
| --- | --- | --- | --- | --- |
| 1 | 0.357 ms | 0.208 ms | 8.29 ms | 8.34 ms |
| 2 | 0.328 ms | 0.193 ms | 7.15 ms | 7.74 ms |
| 3 | 0.346 ms | 0.183 ms | 7.76 ms | 6.90 ms |

The buffer update stage improved consistently by about 42–47%. The whole-path change is within runtime variation; no consistent reduction of its median is established. GUI redisplay remains roughly 6 ms. These are wall times, not isolated CPU times or physical key-to-pixel measurements. Raw logs: `build/compiled-display-{before,after}-{1,2,3}.log`. Interpreted-source trials also show buffer update improving from about 0.67 to 0.35 ms.

Avoiding unchanged window-start and character-width assignments did not establish a benefit; those experiments were not applied. Suppressing mode-line invalidation also did not establish a reliable improvement and was left out. The final change preserves display behavior and reduces update work rather than claiming an unmeasured overall latency gain. No commit or push was made; Smart Brain storage is still deferred.

## Sequential investigation and additional improvements

The user requested the following investigations in order. All measurements use Emacs 31.1 and current byte-compiled Lisp. Diagnostic frame settings are confined to separate `-Q` Emacs processes; user configuration was not edited.

### 1. GUI redisplay

`tests/profile-display.el` separates no-change redisplay, cursor motion and one-cell row edits. At 80x33, the row case's median redisplay was about 10.2 ms. Across 200 updates, wall time was 2.16 s and Emacs process CPU time was 2.03 s (the CPU counter resolution is 1 ms). Native CPU samples were concentrated in `redisplay_internal (C function)`. Unchanged and cursor-only cases were about 0.30 and 0.38 ms. At a fixed 80x16 window, the row case was 5.92 ms. This shows CPU work associated with redisplay growing with visible content, rather than merely a notification delay.

Disabling Windows double buffering in the diagnostic process did not help: about 10.37 ms versus about 9.98 ms in that comparison. Window-start and width-table guards also did not help. They are not enabled in the product. The [Emacs Windows backend source](https://raw.githubusercontent.com/emacs-mirror/emacs/master/src/w32term.c) was inspected to select the double-buffer experiment; its master version is a reference, not proof of the installed binary's exact implementation.

The Lisp profiler does not expose individual C font/layout/GDI call costs. Their exact subdivision remains unmeasured. Process CPU includes all Emacs threads and profiler overhead. The initial small-window experiment accidentally split once per case and changed dimensions; that run was discarded and rerun with one fixed split (`display-profile-small-fixed.log`).

### 2. Display properties

The row diagnostic produced about 8.89 ms after stripping nil `face`, `display` and `neo-term-prompt-role` entries, versus about 10.23 ms normally. Removing the fixed-pitch face did not improve it. Diagnostic removal of all properties or protection properties was only used to isolate costs; protection and actual display features remain enabled.

`neo-term--cell-string` now emits only attributes with actual values. This preserves actual colors, SVG width correction, prompt roles and display protection. Ordinary Emacs insertion was checked to avoid introducing color or role inheritance into default cells.

Three alternating compiled GUI PowerShell comparisons before bounded prompt scans:

| Trial | Before whole path | Sparse properties | Before redisplay | Sparse redisplay |
| --- | --- | --- | --- | --- |
| 1 | 6.77 ms | 5.37 ms | 5.88 ms | 4.50 ms |
| 2 | 6.83 ms | 5.40 ms | 5.84 ms | 4.52 ms |
| 3 | 7.40 ms | 5.23 ms | 6.04 ms | 4.57 ms |

Logs: `build/property-{pre-sparse-properties,sparse-property-experiment}-{1,2,3}.log`.

### 3. Prompt search with retained history

`tests/profile-prompts.el` varies retained prompt history while changing one live input cell. At 2000 lines, exact-role prompt scans took a median 3.28 ms, and prefix matching took 8.21 ms. At zero history they took about 0.005 and 0.035 ms.

When history, layout and prompt configuration remain unchanged, scans now start at the live screen's logical-line context. Wrapped prefixes and an unfinished prompt beginning in history are included. New history, clear, resize, layout changes and prompt configuration changes retain a full scan. At 2000 lines, scan medians became 0.005 ms for exact roles and 0.037 ms for prefixes. The benchmark is a controlled retained-history workload, not physical input latency.

An actual GUI PowerShell workload emitted 2000 representative OSC 133 prompt records through ConPTY, then typed the 26-character benchmark through Emacs's real keymap. Three alternating trials, compared to the sparse-property code before bounded scans:

| Trial | Before whole path | Bounded scans | Before buffer update | Bounded update |
| --- | --- | --- | --- | --- |
| 1 | 13.68 ms | 8.11 ms | 3.743 ms | 0.162 ms |
| 2 | 12.88 ms | 8.17 ms | 3.866 ms | 0.162 ms |
| 3 | 12.27 ms | 8.13 ms | 3.788 ms | 0.156 ms |

Logs: `build/history-input-{before,after}-{1,2,3}.log`. This generated prompt workload does not represent 2000 separately executed user commands. Initial generation runs omitted OSC terminators and had an incorrect history-only wait count; those failed measurement runs were excluded and the generator and wait condition were corrected.

### Final verification and limits

With both improvements, three alternating normal GUI PowerShell trials compared to the code before sparse properties: whole-path medians 7.74/7.63/7.93 ms before and 6.03/6.65/6.16 ms after. Runtime variation prevents treating the earlier isolated 5.2–5.4 ms values as a guaranteed final latency. Final normal buffer-update medians are approximately 0.09–0.10 ms; remaining GUI redisplay is still about 5 ms. Physical keyboard delivery, compositor presentation and scanout are excluded.

The final product code passes `test.ps1 -Gui`: native and terminal tests, CLI 40/40, ERT 53/53, DLL integration 20/20, warning-free byte compile and GUI PASS. New regression coverage checks preservation of historical prompt properties, a prompt starting in history, and a wrapped prefix crossing the history/live boundary. The original historical-property regression failed before bounded scans and passes afterward. Actual Emacs Unicode input, copy, colors, reflow and repeated resize pass; no Vim process is used.

New diagnostic files pass Emacs `check-parens`; scoped source reread and `git diff --check` pass. No commit or push made. Smart Brain saving remains deferred because its project is unspecified.

## Native redisplay investigation

The user authorized the next investigation. The installed Emacs executable contains
DWARF symbols. `tests/profile-native-display.py` starts its own hidden `-Q` GUI
Emacs, samples the busiest owned thread's AMD64 instruction pointer, and resolves
Emacs addresses using the installed binary's symbols via MinGW `addr2line`.
It suspends only that owned thread and always resumes it in `finally`.
`objdump` resolves addresses inside short exported `win32u.dll` syscall stubs.
No installed Emacs executable or user configuration was modified.

For 1200 one-cell edits on an 80x33 screen, the resolved run collected 6655
elapsed-time samples: Emacs 1839, DirectWrite 1642, win32u 2052, and ntdll 822.
Emacs samples include glyph production, text extents, bidi processing and garbage
collection. win32u samples include NtUserMessageCall (1121), NtUserSetMenu (676),
and NtGdiBitBlt (166). These are instruction-pointer samples across the render
loop, not exclusive CPU percentages or caller-stack attribution. A syscall
sample can include waiting time. Suspension perturbs the workload, so native
sampling timings are not used as the improvement benchmark. Raw results:
`build/native-display-{baseline,ltr,resolved}.json`.

Diagnostic comparisons, with no production change:

| Experiment | Redisplay median | Decision |
| --- | --- | --- |
| Current 80x33 one-cell edit | 6.49 ms | Baseline |
| Fixed left-to-right paragraph direction, sampled workload | 6.37 ms vs sampled baseline 6.42 ms | No established gain |
| One stretch-space display object per blank cell, guarded against repeated property writes | 23.62 ms | Slower; rejected |
| Hide menu bar in the disposable frame | 5.60 ms | Changes UI; not applied |
| Disable DirectWrite in the disposable process and clear its font cache | 2.95 ms | Promising alternate rendering path |

The unguarded first stretch-space trial rewrote display properties every update;
its measurement was discarded as a confounded comparison. The guarded trial also
loses decisively. No cursor/selection equivalence claim is made for that prototype.

`NEO_TERM_BENCH_NO_DWRITE=1` now selects the alternate font path only inside the
disposable benchmark process. Three alternating compiled GUI PowerShell trials
typed 26 characters through Emacs's real keymap:

| Trial | Current whole path | No DirectWrite | Current redisplay | No DirectWrite redisplay |
| --- | --- | --- | --- | --- |
| 1 | 5.84 ms | 3.42 ms | 4.59 ms | 2.64 ms |
| 2 | 5.36 ms | 3.40 ms | 4.58 ms | 2.69 ms |
| 3 | 5.30 ms | 3.25 ms | 4.46 ms | 2.52 ms |

Logs: `build/native-font-{before,no-dwrite}-{1,2,3}.log`. This consistently reduces
the measured whole-path median by about 36–41%; physical keyboard delivery and
compositor/scanout remain excluded. It does not achieve a 0.2 ms whole path.

The alternate path passes the actual Emacs GUI suite (protected input/copy,
Unicode keyboard round trip, Unicode pixel placement, colors, history/reflow,
repeated resize, cursor, alternate screen and mouse). `build/gui-no-dwrite.log`
records PASS. An initial command-line `--eval` launch was incorrectly quoted by
Start-Process and timed out; it was replaced by a saved Lisp driver and rerun
successfully. That timeout is not a font-backend compatibility failure.

`w32-inhibit-dwrite` is an Emacs-wide rendering switch. The suite does not establish
identical color-emoji appearance, antialiasing or rendering in other user buffers.
Consequently no production toggle, global setting or buffer-local workaround was
applied. The investigation identifies an alternate rendering path with measured
gain, while preserving the existing product behavior. Smart Brain saving remains
deferred because the target project is unspecified.

## Commit checkpoint

The user authorized committing and pushing this work. Fresh validation initially
reproduced an intermittent bundled-ConPTY descendant-flood exit timeout. Temporary
timing instrumentation measured snapshot mutex acquisition waits of 2.485, 4.937
and 6.485 seconds; console close and child termination were short. The continuous
reader repeatedly reacquired the state mutex, starving the worker's snapshot and
therefore its next parent-exit check. An initial EOF-event hypothesis was rejected
after inspection and repeat failures; its attempted change was removed.

The reader now yields the state mutex through a condition variable when a snapshot
is waiting. Snapshot completion uses a scope guard to notify the reader, including
when snapshot construction throws. Stop changes its predicate under both affected
mutexes and wakes both writer and reader conditions. All temporary trace code was
removed. The previously failing parent/descendant exit test passed ten consecutive
runs across both ConPTY backends (approximately 0.75–0.96 seconds per two-backend
test, versus a three-second timeout per backend).

Official Unicode source text is pinned to LF in `.gitattributes`, preserving its
raw-byte digests after a Windows checkout. `generate-unicode.py --check` passes.
After the final rebuild, `test.ps1 -Gui` passes native/terminal tests, CLI 40/40,
ERT 53/53, DLL integration 20/20, warning-free byte compilation and actual GUI
Emacs verification. The commit includes the rebuilt independent-engine DLL;
temporary logs, native build outputs and diagnostic launch drivers are excluded.
