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
                (neo-term--send "K5,0")
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
                (neo-term--send "U99,4")
                (neo-term-gui-wait (lambda ()
                                     (string-match-p "INTERRUPTED"
                                                     (buffer-string))))
                (neo-term-copy-mode)
                (unless neo-term--copy (error "Copy mode did not activate"))
                (neo-term-copy-mode)
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
      (with-temp-file (expand-file-name "build/gui-check.log" neo-term-gui-root)
        (insert
         "GUI_TESTS=PASS (installed backends; input, resize, interrupt, copy; ConPTY TUI/truecolor/alternate screen; Unicode pixel width)\n"))
      (kill-emacs 0))
  (error
   (with-temp-file (expand-file-name "build/gui-check.log" neo-term-gui-root)
     (insert "GUI_TESTS=FAIL "
             (error-message-string failure) "\n"))
   (kill-emacs 1)))
