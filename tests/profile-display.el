;;; profile-display.el --- Isolate GUI redisplay work -*- lexical-binding: t; -*-
;; Diagnostic only: no shell, no user configuration, no persistent frame settings.
(require 'profiler)
(defconst neo-term-display-profile-root
  (expand-file-name "../" (file-name-directory load-file-name)))
(load (or (getenv "NEO_TERM_PROFILE_SOURCE")
          (expand-file-name "neo-term" neo-term-display-profile-root)) nil t)
(defvar neo-term-display-profile-results nil)

(defun neo-term-display-profile-style ()
  "Apply one diagnostic style reduction in this throwaway buffer."
  (let ((style (getenv "NEO_TERM_PROFILE_STYLE"))
        (inhibit-read-only t) (neo-term--updating t))
    (pcase style
      ("left-to-right" (setq-local bidi-paragraph-direction 'left-to-right))
      ("stretch-spaces"
       (save-excursion
         (goto-char (point-min))
         (while (search-forward " " nil t)
           ;; Use separate objects for distinct cell display boundaries.
           (unless (get-text-property (1- (point)) 'display)
             (put-text-property (1- (point)) (point) 'display
                                `(space :width (,(frame-char-width))))))))
      ("no-properties" (set-text-properties (point-min) (point-max) nil))
      ("no-face" (buffer-face-mode -1))
      ("no-nil-properties"
       (remove-text-properties (point-min) (point-max)
                               '(face nil display nil neo-term-prompt-role nil)))
      ("no-protection"
       (remove-text-properties (point-min) (point-max)
                               '(read-only nil front-sticky nil))))))

(defun neo-term-display-profile-case (kind)
  (let ((buffer (generate-new-buffer " *neo-term display profile*")) samples
        (count (if (getenv "NEO_TERM_PROFILE_NATIVE") 1200 200))
        cpu-start wall-start gc-start profile-error profile)
    (unwind-protect
        (progn
          (switch-to-buffer buffer)
          (neo-term-mode)
          (when (timerp neo-term--cursor-timer) (cancel-timer neo-term--cursor-timer))
          (setq neo-term--cursor-blink nil)
          (let* ((cols (window-body-width)) (height (window-body-height))
                 (cells (vconcat (cl-loop repeat cols collect [" " 1 -1 -1 0])))
                 (rows (vconcat (cl-loop for row below height collect (vector row cells)))))
            (neo-term--screen `((cols . ,cols) (height . ,height) (x . 0) (y . 0)
                                (visible . t) (cursor_blink . nil) (rows . ,rows)))
            (neo-term--render)
            (when (timerp neo-term--timer) (cancel-timer neo-term--timer))
            (setq neo-term--timer nil)
            (neo-term-display-profile-style)
            (redisplay t)
            (if (getenv "NEO_TERM_PROFILE_NATIVE")
                (with-temp-file (expand-file-name "build/native-profile.ready"
                                                   neo-term-display-profile-root)
                  (insert (format "%s" (emacs-pid))))
              (condition-case failure (profiler-cpu-start 1000000)
                (error (setq profile-error failure))))
            (setq cpu-start (current-cpu-time) wall-start (float-time) gc-start gcs-done)
            (dotimes (index count)
              (pcase kind
                ('unchanged nil)
                ('cursor (goto-char (+ (point-min) (% index 20))))
                ('row
                 (let ((new (copy-sequence cells)))
                   (aset new 10 (vector (if (cl-evenp index) "a" "b") 1 -1 -1 0))
                   (aset neo-term--lines 0 new)
                   (neo-term--render)
                   (neo-term-display-profile-style))))
              (let ((start (float-time)))
                (redisplay t)
                (push (* 1000 (- (float-time) start)) samples)))
            (let* ((elapsed (- (float-time) wall-start)) (cpu-end (current-cpu-time))
                   (cpu (/ (float (- (car cpu-end) (car cpu-start))) (cdr cpu-end)))
                   (sorted (sort samples #'<)))
              (when (profiler-cpu-running-p) (profiler-cpu-stop))
              (setq profile (profiler-cpu-log))
              (push (format "CASE=%s double-buffer-disabled=%S size=%sx%s median=%.4fms min=%.4fms max=%.4fms wall=%.4fs process-cpu=%.4fs cpu-resolution=%s gc=%s profiler=%S\n"
                            kind w32-disable-double-buffering cols height
                            (/ (+ (nth 99 sorted) (nth 100 sorted)) 2)
                            (car sorted) (car (last sorted)) elapsed cpu (cdr cpu-end)
                            (- gcs-done gc-start)
                            (if (getenv "NEO_TERM_PROFILE_NATIVE") 'external-sampler
                              (or profile-error 'available)))
                    neo-term-display-profile-results)
              (when profile
                (let (entries)
                  (maphash (lambda (trace hits)
                             (push (cons hits (profiler-fixup-backtrace trace)) entries)) profile)
                  (setq entries (sort entries (lambda (a b) (> (car a) (car b)))))
                  (dolist (entry (seq-take entries 8))
                    (push (format "PROFILE=%s hits=%s trace=%S\n" kind (car entry) (cdr entry))
                          neo-term-display-profile-results)))))))
      (when (profiler-cpu-running-p) (profiler-cpu-stop))
      (kill-buffer buffer))))

(condition-case failure
    (progn
      (unless (display-graphic-p) (error "GUI Emacs is required"))
      (when (getenv "NEO_TERM_PROFILE_NO_DOUBLE_BUFFER")
        (setq w32-disable-double-buffering t))
      (when (getenv "NEO_TERM_PROFILE_NO_MENU") (menu-bar-mode -1))
      (when (getenv "NEO_TERM_PROFILE_NO_DWRITE")
        (setq w32-inhibit-dwrite t)
        ;; Reopen fonts in this disposable frame after selecting the backend.
        (clear-font-cache))
      (when (getenv "NEO_TERM_PROFILE_SMALL") (split-window-below))
      (dolist (kind (if (getenv "NEO_TERM_PROFILE_NATIVE") '(row)
                     '(unchanged cursor row)))
        (neo-term-display-profile-case kind))
      (with-temp-file (expand-file-name "build/display-profile.log" neo-term-display-profile-root)
        (insert (format "EMACS=%s\n" emacs-version))
        (dolist (result (nreverse neo-term-display-profile-results)) (insert result)))
      (kill-emacs 0))
  (error
   (with-temp-file (expand-file-name "build/display-profile.log" neo-term-display-profile-root)
     (insert (format "FAIL=%S\n" failure)))
   (kill-emacs 1)))
