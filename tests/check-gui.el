;;; check-gui.el -*- lexical-binding: t; no-byte-compile: t; -*-
(setq debug-on-error t)
(defconst neo-term-gui-root (expand-file-name ".."
                                              (file-name-directory load-file-name)))
(load (expand-file-name "neo-term.el" neo-term-gui-root) nil t)
(defun neo-term-gui-wait (predicate)
  (let ((deadline (+ (float-time) 15)))
    (while (and (not (funcall predicate))
                (< (float-time) deadline))
      (accept-process-output nil .05)
      (sit-for .02))
    (unless (funcall predicate)
      (error "GUI terminal wait timed out: %s" neo-term--status))))
(condition-case failure
    (progn
      (unless (display-graphic-p)
        (error "GUI Emacs is required"))
      (let ((buffer (generate-new-buffer "*neo-term-width-check*")))
        (unwind-protect
            (progn
              (pop-to-buffer buffer)
              (neo-term-mode)
              (neo-term--screen '((type . "screen")
                                  (v . 1)
                                  (generation . 1)
                                  (cols . 2)
                                  (height . 2)
                                  (x . 1)
                                  (y . 0)
                                  (visible . t)
                                  (alt . nil)
                                  (rows . [[0 [["🫠" 1 -1 -1 0] ["A" 1 -1 -1 0]]]
                                           [1 [[" " 1 -1 -1 0] [" " 1 -1 -1 0]]]])
                                  (history . [])))
              (neo-term--render)
              (unless (string-prefix-p "🫠A"
                                       (buffer-substring-no-properties (point-min)
                                                                       (point-max)))
                (error "Width correction altered copyable Unicode text"))
              (redisplay t)
              (let* ((origin (car (posn-x-y (posn-at-point 1))))
                     (after (car (posn-x-y (posn-at-point 2))))
                     (cell-width (frame-char-width)))
                (unless (<= (abs (- (- after origin) cell-width)) 1)
                  (error "GUI native width mismatch: %s versus %s"
                         (- after origin) cell-width))))
          (kill-buffer buffer)))
      (dolist (backend (append '(system-conpty classic)
                               (when (file-exists-p (expand-file-name
                                                     "build/runtime/conpty.dll"
                                                     neo-term-gui-root)) '(bundled-conpty))))
        (let* ((neo-term-backend backend)
               (neo-term-shell (expand-file-name "build/console-fixture.exe" neo-term-gui-root))
               (neo-term-shell-arguments '("interactive"))
               (buffer (neo-term)))
          (unwind-protect
              (with-current-buffer buffer
                (neo-term-gui-wait (lambda ()
                                     (string-match-p "FIXTURE_READY"
                                                     (buffer-string))))
                (neo-term--send "T日")
                (execute-kbd-macro (kbd "<up>"))
                (neo-term-gui-wait (lambda ()
                                     (and (string-match-p "CHAR=26085"
                                                          (buffer-string))
                                          (string-match-p "KEY=38"
                                                          (buffer-string)))))
                (let ((previous neo-term--cols))
                  (set-frame-size (selected-frame)
                                  (+ (frame-width) 5)
                                  (frame-height))
                  (neo-term-gui-wait (lambda ()
                                       (/= previous neo-term--cols))))
                (execute-kbd-macro (kbd "<backspace> <delete>"))
                (neo-term-gui-wait
                 (lambda ()
                   (and (string-match-p "KEY=8 " (buffer-string))
                        (string-match-p "KEY=46 " (buffer-string)))))
                (execute-kbd-macro (kbd "RET TAB <backtab>"))
                (neo-term-gui-wait
                 (lambda ()
                   (and (string-match-p "KEY=13 CHAR=13" (buffer-string))
                        (string-match-p "KEY=9 CHAR=9" (buffer-string)))))
                (execute-kbd-macro (kbd "C-c C-c"))
                (neo-term-gui-wait (lambda ()
                                     (string-match-p "INTERRUPTED"
                                                     (buffer-string))))
                (execute-kbd-macro (kbd "C-x C-q"))
                (unless neo-term--copy (error "Copy mode did not activate"))
                (unless buffer-read-only (error "Copy mode made the display writable"))
                (condition-case nil
                    (progn
                      (let ((inhibit-read-only t))
                        (delete-region (point-min) (point-max)))
                      (error "Terminal display was editable"))
                  (user-error nil))
                (execute-kbd-macro (kbd "C-l"))
                (when neo-term--copy (error "Clear did not resume input mode"))
                (neo-term-gui-wait
                 (lambda () (string-empty-p (string-trim (buffer-string)))))
                (execute-kbd-macro "z")
                (neo-term-gui-wait
                 (lambda () (string-match-p "CHAR=122" (buffer-string))))
                (redisplay t))
            (when (buffer-live-p buffer)
              (kill-buffer buffer)))))
      (dolist (backend '(system-conpty bundled-conpty))
        (when (or (eq backend 'system-conpty)
                  (file-exists-p (expand-file-name "build/runtime/conpty.dll" neo-term-gui-root)))
          (let* ((neo-term-backend backend)
                 (neo-term-shell (expand-file-name "build/console-fixture.exe" neo-term-gui-root))
                 (neo-term-shell-arguments '("vt"))
                 (buffer (neo-term)))
            (unwind-protect
                (with-current-buffer buffer
                  (neo-term-gui-wait (lambda ()
                                       (and neo-term--alt (string-match-p "ALT_SCREEN"
                                                                          (buffer-string)))))
                  (goto-char (point-min))
                  (search-forward "ALT_SCREEN")
                  (unless (equal (plist-get (get-text-property (1- (point)) 'face)
                                            :foreground) "#12ab34")
                    (error "GUI TUI truecolor was lost"))
                  (redisplay t)
                  (neo-term-gui-wait (lambda ()
                                       (and (not neo-term--alt)
                                            (string-match-p "BURST_DONE"
                                                            (buffer-string))))))
              (when (buffer-live-p buffer)
                (kill-buffer buffer))))))
      (dolist (backend '(system-conpty classic bundled-conpty))
        (when (or (not (eq backend 'bundled-conpty))
                  (file-exists-p (expand-file-name "build/runtime/conpty.dll" neo-term-gui-root)))
          (let* ((neo-term-backend backend)
                 (neo-term-shell "powershell.exe")
                 (neo-term-shell-arguments '("-NoLogo" "-NoProfile"))
                 (buffer (neo-term))
                 (destination (expand-file-name "build" neo-term-gui-root)))
            (unwind-protect
                (with-current-buffer buffer
                  (neo-term-gui-wait
                   (lambda () (string-prefix-p "PowerShell:" neo-term--title)))
                  (neo-term--send (format "TSet-Location '%s'" destination))
                  (neo-term--send "K1,0")
                  (neo-term-gui-wait
                   (lambda () (equal (downcase default-directory)
                                      (downcase (file-name-as-directory destination)))))
                  (neo-term-gui-wait
                   (lambda () (save-excursion
                                (goto-char (neo-term--cursor-position))
                                (get-text-property (line-beginning-position)
                                                   'neo-term-prompt))))
                  (execute-kbd-macro (kbd "C-c C-t RET"))
                  (unless (string-empty-p (current-kill 0))
                    (error "Copy retained PowerShell prompt")))
              (when (buffer-live-p buffer) (kill-buffer buffer))))))
      (when (file-exists-p (expand-file-name "build/runtime/conpty.dll" neo-term-gui-root))
        (let* ((neo-term-backend 'bundled-conpty)
               (neo-term-shell (expand-file-name "build/console-fixture.exe" neo-term-gui-root))
               (neo-term-shell-arguments '("mouse"))
               (buffer (neo-term)))
          (unwind-protect
              (with-current-buffer buffer
                (neo-term-gui-wait
                 (lambda () (and (> neo-term--mouse 0)
                                 (string-match-p "FIXTURE_READY" (buffer-string)))))
                (redisplay t)
                (let ((position (posn-at-point (+ (aref neo-term--row-markers 2) 4))))
                  (unless position (error "Mouse cell was not visible"))
                  (execute-kbd-macro
                   (vector (list 'down-mouse-1 position) (list 'mouse-1 position))))
                (neo-term-gui-wait
                 (lambda () (and (string-match-p "MOUSE=4,2 BUTTONS=1" (buffer-string))
                                 (string-match-p "MOUSE=4,2 BUTTONS=0" (buffer-string))))))
            (when (buffer-live-p buffer) (kill-buffer buffer)))))
      (with-temp-file (expand-file-name "build/gui-check.log" neo-term-gui-root)
        (insert
         "GUI_TESTS=PASS (installed backends; protected input/copy, clear, PowerShell cwd/prompt; ConPTY TUI/colors/alternate screen; bundled mouse; Unicode pixel width)\n"))
      (kill-emacs 0))
  (error
   (with-temp-file (expand-file-name "build/gui-check.log" neo-term-gui-root)
     (insert "GUI_TESTS=FAIL "
             (error-message-string failure) "\n"))
   (kill-emacs 1)))
