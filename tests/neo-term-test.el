;;; neo-term-test.el -*- lexical-binding: t; no-byte-compile: t; -*-
(require 'ert)
(require 'json)
(defconst neo-term-test-root (expand-file-name ".."
                                               (file-name-directory load-file-name)))
(let ((source (expand-file-name "../neo-term.el"
                                (file-name-directory load-file-name))))
  (when (file-exists-p source)
    (load source nil t)))

(ert-deftest neo-term-frames-survive-split-and-coalesced-receives ()
  (should (fboundp 'neo-term--consume))
  (with-temp-buffer
    (neo-term-mode)
    (let* ((json
            "{\"type\":\"ready\",\"v\":1,\"backend\":\"classic\",\"runtime\":\"test\",\"pid\":1,\"attempts\":[]}")
           (frame (neo-term--frame json)))
      (neo-term--consume (substring frame 0 2))
      (should-not neo-term--session)
      (neo-term--consume (concat (substring frame 2) frame))
      (should (equal (alist-get 'backend neo-term--session) "classic"))
      (should (= (length neo-term--pending) 0)))))

(ert-deftest neo-term-oversized-input-is-rejected-before-allocation ()
  (should (fboundp 'neo-term--consume))
  (with-temp-buffer
    (neo-term-mode)
    (should-error (neo-term--consume (unibyte-string 0 0 64 1)))))

(ert-deftest neo-term-unicode-wide-cells-and-colors-are-rendered ()
  (should (fboundp 'neo-term--screen))
  (with-temp-buffer
    (neo-term-mode)
    (neo-term--screen
     '((type . "screen")
       (v . 1)
       (generation . 1)
       (cols . 5)
       (height . 2)
       (x . 2)
       (y . 0)
       (visible . t)
       (alt . nil)
       (rows . [[0 [["日" 2 16711680 -1 1] ["A" 1 -1 -1 0] [" " 1 -1 -1 0] [" " 1 -1 -1 0]]]
                [1 [["😀" 2 -1 -1 0] [" " 1 -1 -1 0] [" " 1 -1 -1 0] [" " 1 -1 -1 0]]]])
       (history . [])))
    (should (string-prefix-p "日A  \n😀"
                             (buffer-string)))
    (should (= (point) 2))
    (should (equal (plist-get (get-text-property 1 'face)
                              :foreground) "#ff0000"))
    (should (eq (plist-get (get-text-property 1 'face)
                           :weight) 'bold))))

(ert-deftest neo-term-key-encoding-distinguishes-characters-and-control-keys ()
  (should (fboundp 'neo-term--key-command))
  (should (equal (neo-term--key-command 'up) "K5,0"))
  (should (equal (neo-term--key-command ?日) "T日"))
  (should (equal (neo-term--key-command ?\C-c) "U99,4"))
  (should (equal (neo-term--key-command 'C-left) "K7,4"))
  (should (equal (neo-term--key-command 'return) "K1,0")))

(ert-deftest neo-term-ascii-terminal-keys-and-backtab-preserve-key-identity ()
  (should (equal (neo-term--key-command ?\r) "K1,0"))
  (should (equal (neo-term--key-command ?\t) "K2,0"))
  (should (equal (neo-term--key-command ?\e) "K4,0"))
  (should (equal (neo-term--key-command 'backtab) "K2,1")))

(ert-deftest neo-term-native-cell-width-overrides-emacs-unicode-width ()
  (let ((original (char-width #x1fae0)))
    (with-temp-buffer
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
      (should (= (current-column) 1))
      (should (= (string-width "🫠A") 2)))
    (should (= (char-width #x1fae0) original))))

(ert-deftest neo-term-output-history-is-copyable-and-bounded ()
  (should (fboundp 'neo-term--screen))
  (with-temp-buffer
    (neo-term-mode)
    (let ((neo-term-scrollback-lines 2))
      (neo-term--screen '((type . "screen")
                          (v . 1)
                          (generation . 1)
                          (cols . 2)
                          (height . 2)
                          (x . 0)
                          (y . 0)
                          (visible . t)
                          (alt . nil)
                          (rows . [[0 [["X" 1 -1 -1 0] [" " 1 -1 -1 0]]] [1 [[" " 1 -1 -1 0]
                                                                             [" " 1 -1 -1 0]]]])
                          (history . ["old1" "old2" "old3"])))
      (should (string-prefix-p "old2\nold3\nX"
                               (buffer-string))))))

(ert-deftest neo-term-real-cmd-output-reaches-emacs-buffer ()
  (should (fboundp 'neo-term))
  (let* ((neo-term-host-program (expand-file-name "build/neo-term-host.exe" neo-term-test-root))
         (neo-term-backend 'system-conpty)
         (neo-term-shell "cmd.exe")
         (neo-term-shell-arguments '("/Q"))
         (buffer (neo-term)))
    (unwind-protect
        (with-current-buffer buffer
          (let ((deadline (+ (float-time) 10)))
            (while (and (not neo-term--session)
                        (< (float-time) deadline))
              (accept-process-output nil .05))
            (should neo-term--session))
          (neo-term--send "Techo NEO^_EMACS_OK")
          (neo-term--send "K1,0")
          (let ((deadline (+ (float-time) 10)))
            (while (and (not (string-match-p "NEO_EMACS_OK"
                                             (buffer-string)))
                        (< (float-time) deadline))
              (accept-process-output nil .05))
            (should (string-match-p "NEO_EMACS_OK"
                                    (buffer-string)))))
      (when (buffer-live-p buffer)
        (kill-buffer buffer)))))

(defun neo-term-test-screen ()
  (neo-term--screen
   '((type . "screen") (v . 1) (cols . 2) (height . 2)
     (x . 1) (y . 0) (visible . t) (alt . nil)
     (rows . [[0 [["X" 1 -1 -1 0] [" " 1 -1 -1 0]]]
              [1 [[" " 1 -1 -1 0] [" " 1 -1 -1 0]]]])
     (history . ["old"]))))

(ert-deftest neo-term-display-cannot-be-edited-after-read-only-is-disabled ()
  (with-temp-buffer
    (neo-term-mode)
    (neo-term-test-screen)
    (read-only-mode -1)
    (let ((before (buffer-string)))
      (should-error (insert "local edit"))
      (read-only-mode -1)
      (should-error (delete-region (point-min) (point-max)))
      (should (equal before (buffer-string))))
    (neo-term-test-screen)
    (should buffer-read-only)))

(ert-deftest neo-term-command-loop-restores-protection-after-a-rejected-edit ()
  (with-temp-buffer
    (neo-term-mode)
    (dotimes (_ 2)
      (read-only-mode -1)
      (should-error (insert "local edit"))
      (run-hooks 'post-command-hook)
      (should buffer-read-only)
      (should (memq #'neo-term--protect-display before-change-functions)))
    (should (string-empty-p (buffer-string)))))

(ert-deftest neo-term-editing-keys-send-input-without-changing-the-display ()
  (with-temp-buffer
    (neo-term-mode)
    (neo-term-test-screen)
    (let ((before (buffer-string)) sent)
      (cl-letf (((symbol-function 'neo-term--send)
                 (lambda (command) (push command sent))))
        (let ((last-command-event 'backspace))
          (call-interactively (key-binding [backspace])))
        (let ((last-command-event 'delete))
          (call-interactively (key-binding [delete])))
        (should (equal (reverse sent) '("K3,0" "K10,0")))
        (should (equal before (buffer-string)))))))

(ert-deftest neo-term-typing-with-a-selection-does-not-delete-terminal-output ()
  (with-temp-buffer
    (neo-term-mode)
    (neo-term-test-screen)
    (delete-selection-mode 1)
    (unwind-protect
        (let ((before (buffer-string))
              (transient-mark-mode t)
              (last-command-event ?a)
              (this-command 'neo-term-send-key)
              sent)
          (goto-char (point-min))
          (push-mark (point-max) t t)
          (cl-letf (((symbol-function 'neo-term--send)
                     (lambda (command) (setq sent command))))
            (delete-selection-pre-hook)
            (call-interactively (key-binding "a")))
          (should (equal sent "Ta"))
          (should (equal before (buffer-string))))
      (delete-selection-mode -1))))

(ert-deftest neo-term-terminal-prefix-reserves-clear-copy-and-interrupt ()
  (with-temp-buffer
    (neo-term-mode)
    (should (eq (key-binding (kbd "C-c C-c")) #'neo-term-interrupt))
    (should (eq (key-binding (kbd "C-c C-l")) #'neo-term-clear-scrollback))
    (should (eq (key-binding (kbd "C-l")) #'neo-term-clear))
    (should (eq (key-binding (kbd "C-c M-l")) #'neo-term-clear-scrollback))
    (should (eq (key-binding (kbd "C-x C-q")) #'neo-term-copy-mode))
    (call-interactively (key-binding (kbd "C-x C-q")))
    (should neo-term--copy)
    (should buffer-read-only)))

(ert-deftest neo-term-special-keys-and-modifiers-reach-the-cli ()
  (with-temp-buffer
    (neo-term-mode)
    (dolist (key '(left right up down home end prior next return tab backtab escape insert
                       backspace delete deletechar f1 f12 f24))
      (dolist (modifiers '(nil (control) (meta) (shift) (control shift)
                              (control meta) (meta shift) (control meta shift)))
        (let ((event (event-convert-list (append modifiers (list key)))) sent)
          (if (eq event 'S-insert)
              (should (eq (key-binding (vector event)) #'neo-term-paste))
            (cl-letf (((symbol-function 'neo-term--send)
                       (lambda (command) (setq sent command))))
              (let ((last-command-event event))
                (call-interactively (key-binding (vector event)))))
            (should (equal sent (neo-term--key-command event)))))))))

(ert-deftest neo-term-clear-requests-do-not-inject-shell-commands ()
  (with-temp-buffer
    (neo-term-mode)
    (neo-term-test-screen)
    (neo-term-copy-mode)
    (let (sent)
      (cl-letf (((symbol-function 'neo-term--send)
                 (lambda (command) (push command sent))))
        (neo-term-clear)
        (should-not neo-term--copy)
        (neo-term-clear-scrollback)
        (neo-term-interrupt)
        (should (equal (reverse sent) '("L" "H" "U99,4")))))))

(ert-deftest neo-term-history-clear-acknowledgment-retains-the-current-screen ()
  (with-temp-buffer
    (neo-term-mode)
    (neo-term-test-screen)
    (neo-term--screen
     '((type . "screen") (v . 1) (cols . 2) (height . 2)
       (x . 1) (y . 0) (visible . t) (alt . nil)
       (rows . []) (history . []) (history_cleared . t)))
    (should-not neo-term--history)
    (should (string-prefix-p "X " (buffer-string)))))

(ert-deftest neo-term-cursor-only-updates-do-not-rewrite-buffer-text ()
  (with-temp-buffer
    (neo-term-mode)
    (neo-term-test-screen)
    (let ((tick (buffer-chars-modified-tick)))
      (neo-term--screen
       '((v . 1) (cols . 2) (height . 2) (x . 0) (y . 1)
         (visible . t) (rows . []) (history . [])))
      (should (= tick (buffer-chars-modified-tick)))
      (should (= (line-number-at-pos) 3)))))

(ert-deftest neo-term-row-updates-preserve-unchanged-history-and-screen-markers ()
  (with-temp-buffer
    (neo-term-mode)
    (neo-term-test-screen)
    (let ((history-marker (copy-marker 2))
          (row-marker (copy-marker 8)))
      (neo-term--screen
       '((v . 1) (cols . 2) (height . 2) (x . 0) (y . 0)
         (visible . t) (rows . [[0 [["日" 2 -1 -1 0]]]]) (history . [])))
      (should (= history-marker 2))
      (should (= row-marker 7))
      (should (equal (buffer-substring-no-properties (point-min) (point-max))
                     "old\n日\n  ")))))

(ert-deftest neo-term-history-appends-before-a-rewritten-first-screen-row ()
  (with-temp-buffer
    (neo-term-mode)
    (neo-term-test-screen)
    (neo-term--screen
     '((v . 1) (cols . 2) (height . 2) (x . 0) (y . 0) (visible . t)
       (rows . [[0 [["A" 1 -1 -1 0] ["B" 1 -1 -1 0]]]]) (history . [])))
    (neo-term--screen
     '((v . 1) (cols . 2) (height . 2) (x . 0) (y . 0) (visible . t)
       (rows . []) (history . ["new"])))
    (should (equal (buffer-substring-no-properties (point-min) (point-max))
                   "old\nnew\nAB\n  "))
    (should (= neo-term--screen-start (aref neo-term--row-markers 0)))))

(ert-deftest neo-term-history-growth-and-trimming-retain-the-live-screen ()
  (with-temp-buffer
    (neo-term-mode)
    (neo-term-test-screen)
    (let ((neo-term-scrollback-lines 2))
      (neo-term--screen
       '((v . 1) (cols . 2) (height . 2) (x . 0) (y . 0)
         (visible . t) (rows . []) (history . ["new1" "new2"])))
      (should (equal (buffer-substring-no-properties (point-min) (point-max))
                     "new1\nnew2\nX \n  "))
      (let ((neo-term-scrollback-lines 0))
        (neo-term--screen
         '((v . 1) (cols . 2) (height . 2) (x . 0) (y . 0)
           (visible . t) (rows . []) (history . ["new3"])))
        (should (equal (buffer-substring-no-properties (point-min) (point-max))
                       "X \n  "))))))

(ert-deftest neo-term-copy-return-copies-selection-and-resumes-input ()
  (with-temp-buffer
    (neo-term-mode)
    (neo-term-test-screen)
    (neo-term-copy-mode)
    (goto-char (point-min))
    (let ((transient-mark-mode t) (kill-ring nil))
      (push-mark (+ (point) 3) t t)
      (call-interactively (key-binding (kbd "RET")))
      (should (equal (current-kill 0) "old"))
      (should-not neo-term--copy)
      (should buffer-read-only))))

(ert-deftest neo-term-shell-metadata-tracks-local-directory-and-excludes-prompt ()
  (with-temp-buffer
    (neo-term-mode)
    (let* ((directory (expand-file-name ".." neo-term-test-root))
           (metadata (json-serialize `((directory . ,directory)
                                       (prompt . "PS> ") (title . "PowerShell"))))
           (title (concat "neo-term;" (base64-encode-string
                                       (encode-coding-string metadata 'utf-8) t))))
      (neo-term--screen
       `((v . 1) (cols . 6) (height . 2) (x . 5) (y . 0) (visible . t)
         (title . ,title)
         (rows . [[0 [["P" 1 -1 -1 0] ["S" 1 -1 -1 0] [">" 1 -1 -1 0]
                      [" " 1 -1 -1 0] ["X" 1 -1 -1 0] [" " 1 -1 -1 0]]]])
         (history . [])))
      (should (equal default-directory (file-name-as-directory directory)))
      (should (equal neo-term--title "PowerShell"))
      (neo-term-copy-mode)
      (goto-char (point-min))
      (let ((kill-ring nil))
        (neo-term-copy-mode-done)
        (should (equal (current-kill 0) "X"))))))

(ert-deftest neo-term-invalid-shell-metadata-cannot-change-directory ()
  (with-temp-buffer
    (neo-term-mode)
    (let ((directory default-directory))
      (neo-term--update-title "neo-term;not-base64-json")
      (should (equal default-directory directory))
      (let ((metadata (json-serialize '((directory . "/ssh:remote:/tmp")
                                        (prompt . "X> ") (title . "remote")))))
        (neo-term--update-title
         (concat "neo-term;" (base64-encode-string metadata t)))
        (should (equal default-directory directory))))))

(ert-deftest neo-term-custom-powershell-command-is-not-replaced-by-integration ()
  (let ((neo-term-shell "powershell.exe")
        (neo-term-shell-arguments '("-Command" "'custom'")))
    (should (equal (neo-term--shell-arguments) neo-term-shell-arguments)))
  (let ((neo-term-shell "powershell.exe")
        (neo-term-shell-arguments '("-NoLogo" "-NoProfile")))
    (should (member "-NoExit" (neo-term--shell-arguments)))
    (should (member "-EncodedCommand" (neo-term--shell-arguments)))))

(ert-deftest neo-term-title-control-characters-are-removed ()
  (with-temp-buffer
    (neo-term-mode)
    (neo-term--update-title "title\n\t")
    (should (equal neo-term--title "title"))))

(ert-deftest neo-term-osc52-copy-requires-explicit-opt-in ()
  (with-temp-buffer
    (neo-term-mode)
    (let ((kill-ring '("unchanged")))
      (neo-term--receive-clipboard ["日本語😀"])
      (should (equal kill-ring '("unchanged")))
      (let ((neo-term-enable-osc52 t))
        (neo-term--receive-clipboard ["日本語😀"])
        (should (equal (current-kill 0) "日本語😀")))
      (should-error (neo-term--receive-clipboard [1])))))

(ert-deftest neo-term-requested-mouse-input-preserves-button-release-and-modifiers ()
  (let ((buffer (generate-new-buffer "*neo-term-mouse-test*")))
    (unwind-protect
        (progn
          (switch-to-buffer buffer)
          (neo-term-mode)
          (neo-term-test-screen)
          (setq neo-term--mouse 1)
          (let ((position (list (selected-window) 1 '(0 . 0) 0 nil 1 '(0 . 0))) sent)
            (cl-letf (((symbol-function 'neo-term--send)
                       (lambda (command) (push command sent))))
              (neo-term-send-mouse (list 'C-down-mouse-1 position))
              (let ((end (list (selected-window) 2 '(1 . 0) 0 nil 2 '(1 . 0))))
                (neo-term-send-mouse (list 'C-drag-mouse-1 position end))))
            (should (equal (reverse sent) '("M0,0,1,4" "M0,1,-1,4")))))
      (kill-buffer buffer))))

(ert-deftest neo-term-resuming-copy-mode-catches-up-after-resize-and-alternate-screen ()
  (with-temp-buffer
    (neo-term-mode)
    (neo-term-test-screen)
    (neo-term-copy-mode 1)
    (let ((before (buffer-string)))
      (neo-term--screen
       '((v . 1) (cols . 3) (height . 2) (x . 1) (y . 0) (visible . t)
         (alt . t) (rows . [[0 [["Z" 1 -1 -1 0] [" " 1 -1 -1 0]
                                [" " 1 -1 -1 0]]]]) (history . ["later"])))
      (should (equal before (buffer-string))))
    (neo-term-copy-mode -1)
    (should (equal (buffer-substring-no-properties (point-min) (point-max)) "Z  \n   "))
    (neo-term--screen
     '((v . 1) (cols . 3) (height . 2) (x . 1) (y . 0) (visible . t)
       (alt . nil) (rows . []) (history . [])))
    (should (string-prefix-p "old\nlater\nZ" (buffer-string)))))
