;;; module-test.el -*- lexical-binding: t; no-byte-compile: t; -*-
(setq debug-on-error t)
(load (expand-file-name "../neo-term.el" (file-name-directory load-file-name)) nil t)
(require 'ert)

(defun neo-term-module-test-wait (buffer predicate &optional timeout)
  (let ((deadline (+ (float-time) (or timeout 12))))
    (while (and (buffer-live-p buffer)
                (not (with-current-buffer buffer (funcall predicate)))
                (< (float-time) deadline))
      (accept-process-output nil .01))
    (should (buffer-live-p buffer))
    (with-current-buffer buffer
      (unless (funcall predicate)
        (ert-fail (list neo-term--status neo-term--session
                        (process-status neo-term--process)
                        neo-term--cols neo-term--height
                        (substring-no-properties (buffer-string) 0
                                                 (min 300 (buffer-size)))))))))

(defun neo-term-module-test-start (mode &optional directory environment)
  (let ((neo-term-transport 'dll)
        (neo-term-backend 'bundled-conpty)
        (neo-term-shell (expand-file-name "build/console-fixture.exe" neo-term--directory))
        (neo-term-shell-arguments (list mode))
        (default-directory (or directory default-directory))
        (process-environment (or environment process-environment)))
    (neo-term)))

(defun neo-term-module-test-wait-for-exit (pids)
  (let ((deadline (+ (float-time) 5)))
    (while (and (seq-intersection pids (list-system-processes))
                (< (float-time) deadline))
      (accept-process-output nil .01)))
  (should-not (seq-intersection pids (list-system-processes))))

(ert-deftest neo-term-module-unicode-output-and-exit-preserve-their-values ()
  (let ((buffer (neo-term-module-test-start "unicode")))
    (unwind-protect
        (progn
          (neo-term-module-test-wait buffer (lambda () (equal neo-term--status "exit 7")))
          (with-current-buffer buffer
            (should (process-get neo-term--process 'neo-term-module))
            (neo-term--render)
            (should (string-match-p "日本語😀" (buffer-string)))))
      (when (buffer-live-p buffer) (kill-buffer buffer)))))

(ert-deftest neo-term-module-unicode-input-survives-repeated-resizes ()
  (let ((buffer (neo-term-module-test-start "echo-unicode")))
    (unwind-protect
        (progn
          (neo-term-module-test-wait
           buffer (lambda () (neo-term--render) (string-match-p "ECHO_READY" (buffer-string))))
          (with-current-buffer buffer
            (dotimes (_ 10)
              (dolist (cols '(2 3 4 7))
                (neo-term--send (format "R%d,24" cols))
                (neo-term-module-test-wait buffer (lambda () (= neo-term--cols cols)))))
            (neo-term--send "R80,24")
            (dolist (character (string-to-list "日本語😀é"))
              (neo-term--send (format "U%d,0" character)))
            (neo-term--send "K1,0"))
          (neo-term-module-test-wait buffer (lambda () (equal neo-term--status "exit 9")))
          (with-current-buffer buffer
            (neo-term--render)
            (should (string-match-p "ECHO=日本語😀é" (buffer-string)))))
      (when (buffer-live-p buffer) (kill-buffer buffer)))))

(ert-deftest neo-term-module-burst-output-retains-the-history-tail-before-exit ()
  (let ((buffer (neo-term-module-test-start "vt")))
    (unwind-protect
        (progn
          (neo-term-module-test-wait buffer (lambda () (equal neo-term--status "exit 0")))
          (with-current-buffer buffer
            (neo-term--render)
            (let ((text (buffer-string)))
              (dotimes (offset 512)
                (should (string-match-p (format "burst %d\\b" (+ 2488 offset)) text)))
              (should (string-match-p "BURST_DONE" text)))))
      (when (buffer-live-p buffer) (kill-buffer buffer)))))

(ert-deftest neo-term-module-interrupt-finishes-continuous-unicode-output ()
  (let ((buffer (neo-term-module-test-start "flood")))
    (unwind-protect
        (progn
          (neo-term-module-test-wait buffer (lambda () (> (length neo-term--history) 0)))
          (with-current-buffer buffer (neo-term--send "U99,4"))
          (neo-term-module-test-wait buffer (lambda () (equal neo-term--status "exit 13")) 5)
          (with-current-buffer buffer
            (neo-term--render)
            (should (string-match-p "FLOOD_STOPPED" (buffer-string)))))
      (when (buffer-live-p buffer) (kill-buffer buffer)))))

(ert-deftest neo-term-module-killing-a-buffer-ends-its-cli-without-waiting-for-output ()
  (let ((buffer (neo-term-module-test-start "flood")) pid)
    (unwind-protect
        (progn
          (neo-term-module-test-wait buffer (lambda () neo-term--session))
          (setq pid (with-current-buffer buffer (alist-get 'pid neo-term--session)))
          (should (memq pid (list-system-processes)))
          (sleep-for .1)
          (let ((start (float-time)))
            (kill-buffer buffer)
            (should (< (- (float-time) start) .1)))
          (let ((deadline (+ (float-time) 5)))
            (while (and (memq pid (list-system-processes)) (< (float-time) deadline))
              (accept-process-output nil .01)))
          (should-not (memq pid (list-system-processes)))
          (garbage-collect))
      (when (buffer-live-p buffer) (kill-buffer buffer)))))

(ert-deftest neo-term-module-powershell-integration-works-without-the-helper-exe ()
  (let* ((neo-term-transport 'dll)
         (neo-term-backend 'bundled-conpty)
         (neo-term-shell "powershell.exe")
         (neo-term-shell-arguments '("-NoLogo" "-NoProfile"))
         (buffer (neo-term)))
    (unwind-protect
        (progn
          (neo-term-module-test-wait buffer (lambda () neo-term--exact-prompts))
          (with-current-buffer buffer
            (neo-term--send "P[Console]::Write('DLL_日本語😀'); exit 23")
            (neo-term--send "K1,0"))
          (neo-term-module-test-wait buffer (lambda () (equal neo-term--status "exit 23")))
          (with-current-buffer buffer
            (neo-term--render)
            (should (string-match-p "DLL_日本語😀" (buffer-string)))))
      (when (buffer-live-p buffer) (kill-buffer buffer)))))

(ert-deftest neo-term-module-explicit-dll-reports-an-unavailable-module ()
  (let ((neo-term-transport 'dll)
        (neo-term-module-program (expand-file-name "build/absent-module.dll" neo-term--directory)))
    (should-error (neo-term) :type 'user-error)))

(ert-deftest neo-term-module-exe-transport-is-rejected ()
  (let ((neo-term-transport 'exe)) (should-error (neo-term) :type 'user-error)))

(ert-deftest neo-term-module-sessions-inherit-their-own-directory-and-environment ()
  (let* ((first-directory (make-temp-file (expand-file-name "build/module-first-" neo-term--directory) t))
         (second-directory (make-temp-file (expand-file-name "build/module-second-" neo-term--directory) t))
         (original-pipe (getenv "NEO_TERM_PIPE"))
         (first (neo-term-module-test-start
                 "environment" (file-name-as-directory first-directory)
                 (cons "NEO_TERM_TEST_MARKER=日本語😀" process-environment)))
         (second (neo-term-module-test-start
                  "environment" (file-name-as-directory second-directory)
                  (cons "NEO_TERM_TEST_MARKER=second" process-environment)))
         pids)
    (unwind-protect
        (progn
          (dolist (item (list (list first "日本語😀" first-directory)
                             (list second "second" second-directory)))
            (neo-term-module-test-wait (car item) (lambda () (string-prefix-p "ENV=" neo-term--title)))
            (with-current-buffer (car item)
              (push (alist-get 'pid neo-term--session) pids)
              (should (equal (downcase (subst-char-in-string ?\\ ?/ neo-term--title))
                             (downcase (format "ENV=%s|%s|%s" (nth 1 item) (nth 2 item) (emacs-pid)))))))
          (should (equal original-pipe (getenv "NEO_TERM_PIPE"))))
      (dolist (buffer (list first second))
        (when (buffer-live-p buffer) (kill-buffer buffer)))
      (neo-term-module-test-wait-for-exit pids)
      (delete-directory first-directory)
      (delete-directory second-directory))))

(ert-deftest neo-term-module-repeated-start-and-garbage-collection-release-cli-processes ()
  (dotimes (_ 30)
    (let ((buffer (neo-term-module-test-start "sleeper")) pid)
      (unwind-protect
          (progn
            (neo-term-module-test-wait buffer (lambda () neo-term--session))
            (setq pid (with-current-buffer buffer (alist-get 'pid neo-term--session)))
            (kill-buffer buffer)
            (garbage-collect)
            (let ((deadline (+ (float-time) 3)))
              (while (and (memq pid (list-system-processes)) (< (float-time) deadline))
                (accept-process-output nil .01)))
            (should-not (memq pid (list-system-processes))))
        (when (buffer-live-p buffer) (kill-buffer buffer))))))

(ert-deftest neo-term-module-normal-exit-honors-kill-buffer-on-exit ()
  (let* ((neo-term-kill-buffer-on-exit t)
         (buffer (neo-term-module-test-start "unicode")))
    (unwind-protect
        (let ((deadline (+ (float-time) 5)))
          (while (and (buffer-live-p buffer) (< (float-time) deadline))
            (accept-process-output nil .01))
          (should-not (buffer-live-p buffer)))
      (when (buffer-live-p buffer) (kill-buffer buffer)))))

(ert-deftest neo-term-module-resolves-the-cli-using-the-session-path-and-directory ()
  (let ((directory (make-temp-file (expand-file-name "build/module-path-" neo-term--directory) t)))
    (unwind-protect
        (progn
          (copy-file (expand-file-name "build/console-fixture.exe" neo-term--directory)
                     (expand-file-name "module-cli.exe" directory))
          (dolist (program '("module-cli.exe" "./module-cli.exe"))
            (let* ((neo-term-transport 'dll)
                   (neo-term-backend 'bundled-conpty)
                   (neo-term-shell program)
                   (neo-term-shell-arguments '("unicode"))
                   (default-directory (file-name-as-directory directory))
                   (process-environment (cons (concat "PATH=" directory) process-environment))
                   (buffer (neo-term)))
              (unwind-protect
                  (neo-term-module-test-wait buffer (lambda () (equal neo-term--status "exit 7")))
                (when (buffer-live-p buffer) (kill-buffer buffer))))))
      (delete-file (expand-file-name "module-cli.exe" directory))
      (delete-directory directory))))

(ert-deftest neo-term-module-startup-failure-does-not-start-an-exe ()
  (let* ((directory (make-temp-file (expand-file-name "build/module-unavailable-" neo-term--directory) t))
         (file (expand-file-name "module.dll" directory))
         (neo-term-module-program file)
         (neo-term-backend 'bundled-conpty)
         (neo-term-shell (expand-file-name "build/console-fixture.exe" neo-term--directory))
         (neo-term-shell-arguments '("unicode"))
         (buffer (progn (write-region "" nil file nil 'silent) (neo-term))))
    (unwind-protect
        (progn
          (neo-term-module-test-wait buffer (lambda () (equal neo-term--status "error")))
          (with-current-buffer buffer (should-not neo-term--session)))
      (when (buffer-live-p buffer) (kill-buffer buffer))
      (delete-file file)
      (delete-directory directory))))

(ert-deftest neo-term-module-vim-saves-unicode-after-repeated-resizes ()
  (let ((vim (or (getenv "NEO_TERM_TEST_VIM") (executable-find "vim.exe"))))
    (unless vim (ert-skip "Set NEO_TERM_TEST_VIM to run real Vim integration"))
    (let* ((file (make-temp-file (expand-file-name "build/module-vim-" neo-term--directory) nil ".txt"))
           (neo-term-transport 'dll)
           (neo-term-backend 'bundled-conpty)
           (neo-term-shell vim)
           (neo-term-shell-arguments (list "-Nu" "NONE" "-n" "-i" "NONE" "--cmd" "set encoding=utf-8" file))
           (buffer (neo-term)))
      (unwind-protect
          (progn
            (neo-term-module-test-wait buffer (lambda () (neo-term--render) (string-match-p "module-vim-" (buffer-string))))
            (with-current-buffer buffer (neo-term--send "TiNEO_VIM_OK 日本語😀é"))
            (neo-term-module-test-wait buffer (lambda () (neo-term--render) (string-match-p "NEO_VIM_OK" (buffer-string))))
            (with-current-buffer buffer
              (dotimes (_ 3)
                (dolist (size '((40 . 10) (120 . 40) (80 . 24)))
                  (neo-term--send (format "R%d,%d" (car size) (cdr size)))
                  (neo-term-module-test-wait buffer (lambda () (and (= neo-term--cols (car size)) (= neo-term--height (cdr size)))))))
              (neo-term--send "K4,0")
              (neo-term--send "T:wq")
              (neo-term--send "K1,0"))
            (neo-term-module-test-wait buffer (lambda () (equal neo-term--status "exit 0")))
            (with-temp-buffer
              (insert-file-contents file)
              (should (equal (string-trim (buffer-string)) "NEO_VIM_OK 日本語😀é"))))
        (when (buffer-live-p buffer) (kill-buffer buffer))
        (delete-file file)))))

(ert-deftest neo-term-module-rejects-an-unauthenticated-notification-connection ()
  (let* ((buffer (generate-new-buffer "*neo-term-auth-test*"))
         (server (make-network-process :name "neo-term-auth-test" :server t
                                       :host "127.0.0.1" :family 'ipv4 :service t :buffer buffer :noquery t
                                       :sentinel #'ignore))
         (client (make-network-process :name "neo-term-invalid-client" :host "127.0.0.1"
                                       :service (process-contact server :service) :buffer buffer :noquery t)))
    (unwind-protect
        (progn
          (with-current-buffer buffer (neo-term-mode) (setq neo-term--process server))
          (process-put server 'neo-term-token "expected-token")
          (neo-term--module-connection server client "open")
          (neo-term--module-auth-filter client (neo-term--frame "{\"type\":\"auth\",\"token\":\"wrong-token\"}"))
          (should-not (process-live-p client))
          (should (process-live-p server))
          (with-current-buffer buffer (should (eq neo-term--process server))))
      (when (process-live-p client) (delete-process client))
      (when (buffer-live-p buffer) (kill-buffer buffer)))))

(ert-deftest neo-term-module-screen-clear-retains-history-and-accepts-more-input ()
  (let ((buffer (neo-term-module-test-start "history")) history)
    (unwind-protect
        (progn
          (neo-term-module-test-wait buffer (lambda () (neo-term--render) (string-match-p "FIXTURE_READY" (buffer-string))))
          (with-current-buffer buffer
            (setq history (copy-sequence neo-term--history))
            (should history)
            (neo-term--send "L"))
          (neo-term-module-test-wait buffer (lambda () (seq-every-p (lambda (row) (string-empty-p (string-trim (mapconcat (lambda (cell) (aref cell 0)) row "")))) neo-term--lines)))
          (with-current-buffer buffer
            (should (equal history (seq-take neo-term--history (length history))))
            (neo-term--send "U122,0"))
          (neo-term-module-test-wait buffer (lambda () (neo-term--render) (string-match-p "CHAR=122" (buffer-string)))))
      (when (buffer-live-p buffer) (kill-buffer buffer)))))

(ert-deftest neo-term-distribution-does-not-contain-a-native-helper-exe ()
  (should-not (file-exists-p (expand-file-name "build/neo-term-host.exe" neo-term--directory))))

(ert-deftest neo-term-cmd-notification-preserves-a-directory-with-unicode-and-symbols ()
  (let* ((directory (make-temp-file (expand-file-name "build/日本語 & ! " neo-term--directory) t))
         (default-directory (file-name-as-directory directory))
         (buffer (neo-term-cmd)) pid)
    (unwind-protect
        (progn
          (neo-term-module-test-wait buffer (lambda () neo-term--session))
          (setq pid (with-current-buffer buffer (alist-get 'pid neo-term--session)))
          (neo-term-module-test-wait buffer (lambda () (neo-term--render) (string-match-p ">" (buffer-string))))
          (with-current-buffer buffer
            (should (equal default-directory (file-name-as-directory directory)))
            (should-not (equal neo-term--status "protocol error"))
            (neo-term--send "Pecho SYMBOLS_OK")
            (neo-term--send "K1,0"))
          (neo-term-module-test-wait buffer (lambda () (neo-term--render) (string-match-p "SYMBOLS_OK" (buffer-string)))))
      (when (buffer-live-p buffer) (kill-buffer buffer))
      (when pid (neo-term-module-test-wait-for-exit (list pid)))
      (delete-directory directory))))

(ert-deftest neo-term-unavailable-dll-never-falls-back-to-an-exe ()
  (let ((neo-term-transport 'auto)
        (neo-term-module-program (expand-file-name "build/missing-module.dll" neo-term--directory)))
    (should-error (neo-term) :type 'user-error)))

(ert-deftest neo-term-unsupported-screen-clear-does-not-end-the-cli ()
  (let* ((neo-term-backend 'system-conpty)
         (neo-term-shell (expand-file-name "build/console-fixture.exe" neo-term--directory))
         (neo-term-shell-arguments '("interactive"))
         (buffer (neo-term)))
    (unwind-protect
        (progn
          (neo-term-module-test-wait buffer (lambda () (neo-term--render) (string-match-p "FIXTURE_READY" (buffer-string))))
          (with-current-buffer buffer
            (when (alist-get 'clear neo-term--session) (ert-skip "This runtime supports screen clear"))
            (should-error (neo-term-clear) :type 'user-error)
            (should-error (neo-term--send "L"))
            (should (process-live-p neo-term--process))
            (neo-term--send "U122,0"))
          (neo-term-module-test-wait buffer (lambda () (neo-term--render) (string-match-p "CHAR=122" (buffer-string)))))
      (when (buffer-live-p buffer) (kill-buffer buffer)))))

(ert-run-tests-batch-and-exit (or (getenv "NEO_TERM_MODULE_TEST_SELECTOR") t))
