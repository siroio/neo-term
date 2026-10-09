;;; benchmark-input.el --- Measure the normal GUI input path -*- lexical-binding: t; -*-
;; Run with emacs -Q -l tests/benchmark-input.el.
;; NEO_TERM_BENCH_SHELL optionally selects ordinary shell typing.
;; Drives the real Emacs keymap; excludes physical keyboard delivery and scanout.
(defconst neo-term-input-benchmark-root
  (expand-file-name "../" (file-name-directory load-file-name)))
(load (or (getenv "NEO_TERM_BENCH_SOURCE")
          (expand-file-name "neo-term.el" neo-term-input-benchmark-root)) nil t)
(setq neo-term--directory neo-term-input-benchmark-root
      neo-term-module-program (expand-file-name "build/neo-term-module.dll" neo-term-input-benchmark-root))
(when (getenv "NEO_TERM_BENCH_NO_DWRITE")
  ;; Diagnostic only: this global setting belongs to this disposable -Q process.
  (setq w32-inhibit-dwrite t)
  (clear-font-cache))
(defvar neo-term-input-benchmark-received nil)
(defvar neo-term-input-benchmark-rendered nil)
(defvar neo-term-input-benchmark-render-ms nil)
(defvar neo-term-input-benchmark-send-start nil)
(defvar neo-term-input-benchmark-send-end nil)
(defvar neo-term-input-benchmark-consume nil)
(defvar neo-term-input-benchmark-render-start nil)
(advice-add 'neo-term--send :around
            (lambda (function &rest args)
              (setq neo-term-input-benchmark-send-start (float-time))
              (prog1 (apply function args)
                (setq neo-term-input-benchmark-send-end (float-time)))))
(advice-add 'neo-term--consume :before
            (lambda (&rest _) (setq neo-term-input-benchmark-consume (float-time))))
(advice-add 'neo-term--screen :before
            (lambda (&rest _) (setq neo-term-input-benchmark-received (float-time))))
(advice-add 'neo-term--render :around
            (lambda (function &rest args)
              (let ((start (float-time)))
                (setq neo-term-input-benchmark-render-start start)
                (prog1 (apply function args)
                  (setq neo-term-input-benchmark-rendered (float-time)
                        neo-term-input-benchmark-render-ms
                        (* 1000 (- (float-time) start)))))))

(defun neo-term-input-benchmark-wait (predicate)
  (let ((deadline (+ (float-time) 15)))
    (while (and (not (funcall predicate)) (< (float-time) deadline))
      (accept-process-output nil .001))
    (unless (funcall predicate)
      (error "Input benchmark timed out: %s history=%s tail=%S"
             neo-term--status neo-term--history-revision
             (buffer-substring-no-properties (max (point-min) (- (point-max) 2000)) (point-max))))))

(defun neo-term-input-benchmark-emacs-control (content label)
  "Measure ordinary Emacs editing and redisplay in the same frame."
  (let ((original (current-buffer))
        (control (generate-new-buffer " *neo-term redisplay control*")) times)
    (unwind-protect
        (progn
          (switch-to-buffer control)
          (insert content)
          (let ((inhibit-read-only t))
            (remove-text-properties (point-min) (point-max) '(read-only nil front-sticky nil)))
          (goto-char (point-min))
          (redisplay t)
          (dolist (character (string-to-list "abcdefghijklmnopqrstuvwxyz"))
            (execute-kbd-macro (vector character))
            (let ((start (float-time)))
              (redisplay t)
              (push (* 1000 (- (float-time) start)) times)))
          (setq times (sort times #'<))
          (format "CONTROL=%s median=%.4fms min=%.4fms max=%.4fms\n"
                  label (/ (+ (nth 13 times) (nth 12 times)) 2)
                  (car times) (car (last times))))
      (switch-to-buffer original)
      (kill-buffer control))))

(condition-case failure
    (let* ((neo-term-backend 'bundled-conpty)
           (shell (getenv "NEO_TERM_BENCH_SHELL"))
           (neo-term-shell (or shell (expand-file-name "build/console-fixture.exe" neo-term--directory)))
           (neo-term-shell-arguments (if shell '("-NoLogo" "-NoProfile") '("interactive")))
           (buffer (neo-term))
           (typed "")
           samples)
      (unwind-protect
          (with-current-buffer buffer
            (neo-term-input-benchmark-wait
             (lambda () (if shell neo-term--exact-prompts
                          (string-match-p "FIXTURE_READY" (buffer-string)))))
            (when (and shell (getenv "NEO_TERM_BENCH_HISTORY"))
              (let ((count (string-to-number (getenv "NEO_TERM_BENCH_HISTORY"))))
                (unless (<= 1 count 2000) (error "History profile count must be 1..2000"))
                ;; Emit representative OSC 133 prompt records through the real shell/ConPTY.
                (execute-kbd-macro
                 (vconcat (format "& { $e=[char]27; $b=[char]7; for($i=0;$i -lt %d;$i++){ [Console]::Write(\"$e]133;A${b}P> $e]133;B${b}history-$i`r`n\") } }; Write-Output 'NEOTERM_HISTORY_DONE'" count)
                          [return]))
                (neo-term-input-benchmark-wait
                 (lambda () (and (>= neo-term--history-revision (max 1 (- count neo-term--height)))
                                 (string-match-p "^NEOTERM_HISTORY_DONE *$" (buffer-string)))))
                (redisplay t)))
            (when shell
              ;; A unique prefix avoids matching letters already in the prompt.
              (setq typed "NEOTERMBENCH")
              (execute-kbd-macro (vconcat typed))
              (neo-term-input-benchmark-wait
               (lambda () (string-match-p typed (neo-term--copy-text (point-min) (point-max))))))
            (dolist (character (string-to-list "abcdefghijklmnopqrstuvwxyz"))
              (let ((start (float-time)))
                (setq typed (concat typed (char-to-string character)))
                (execute-kbd-macro (vector character))
                (neo-term-input-benchmark-wait
                 (lambda () (string-match-p (if shell (regexp-quote typed)
                                              (format "CHAR=%d\\b" character))
                                            (if shell (neo-term--copy-text (point-min) (point-max))
                                              (buffer-string)))))
                (let ((before-redisplay (float-time)))
                  (redisplay t)
                  (push (list :total (* 1000 (- (float-time) start))
                              :key-dispatch (* 1000 (- neo-term-input-benchmark-send-start start))
                              :send (* 1000 (- neo-term-input-benchmark-send-end neo-term-input-benchmark-send-start))
                              :roundtrip (* 1000 (- neo-term-input-benchmark-consume neo-term-input-benchmark-send-end))
                              :decode (* 1000 (- neo-term-input-benchmark-received neo-term-input-benchmark-consume))
                              :schedule (* 1000 (- neo-term-input-benchmark-render-start neo-term-input-benchmark-received))
                              :receive (* 1000 (- neo-term-input-benchmark-received start))
                              :render-complete (* 1000 (- neo-term-input-benchmark-rendered start))
                              :render neo-term-input-benchmark-render-ms
                              :redisplay (* 1000 (- (float-time) before-redisplay))) samples))))
            (let ((display-images
                   (cl-loop for position from (point-min) below (point-max)
                            count (eq (car-safe (get-text-property position 'display)) 'image)))
                  (control-result
                   (concat
                    (neo-term-input-benchmark-emacs-control
                     (buffer-substring-no-properties (point-min) (point-max)) "plain-emacs-redisplay")
                    (neo-term-input-benchmark-emacs-control
                     (buffer-substring (point-min) (point-max)) "styled-emacs-redisplay"))))
              (with-temp-file (expand-file-name "build/input-benchmark.log" neo-term--directory)
                (setq samples (nreverse samples))
                (let* ((times (sort (mapcar (lambda (sample) (plist-get sample :total)) samples) #'<))
                       (count (length times)))
                  (insert (format "SHELL=%s GUI_INPUT_REDISPLAY median=%.2fms min=%.2fms max=%.2fms\n"
                                  (or shell "fixture") (/ (+ (nth (/ count 2) times)
                                                             (nth (1- (/ count 2)) times)) 2)
                                  (car times) (car (last times)))))
                (insert (format "DWRITE_INHIBITED=%S\n" w32-inhibit-dwrite))
                (dolist (stage '(:key-dispatch :send :roundtrip :decode :schedule :render :redisplay :total))
                  (let* ((values (sort (mapcar (lambda (sample) (plist-get sample stage)) samples) #'<))
                         (count (length values)))
                    (insert (format "STAGE=%s median=%.4fms min=%.4fms max=%.4fms\n"
                                    stage (/ (+ (nth (/ count 2) values)
                                                (nth (1- (/ count 2)) values)) 2)
                                    (car values) (car (last values))))))
                (insert control-result)
                (insert (format "DISPLAY_IMAGES=%s FRAME_CELL=%sx%s FIXED_PITCH=%S DEFAULT=%S\n"
                                display-images (frame-char-width) (frame-char-height)
                                (face-attribute 'fixed-pitch :family nil t)
                                (face-attribute 'default :family nil t)))
                (insert (format "SAMPLES=%S\n" samples)))))
        (when (buffer-live-p buffer) (kill-buffer buffer)))
      (kill-emacs 0))
  (error
   (with-temp-file (expand-file-name "build/input-benchmark.log" neo-term--directory)
     (insert (format "FAIL=%S\n" failure)))
   (kill-emacs 1)))
