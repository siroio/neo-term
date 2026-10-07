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
