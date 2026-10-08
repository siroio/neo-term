;;; neo-term.el --- Windows native terminal for Emacs -*- lexical-binding: t; -*-
;; Package-Requires: ((emacs "29.1"))
;; Version: 0.2.0
;; SPDX-License-Identifier: GPL-3.0-or-later

(require 'cl-lib)
(require 'json)
(require 'subr-x)
(require 'svg)

(defgroup neo-term nil "Windows native terminals."
  :group 'processes)
(defconst neo-term--directory (file-name-directory (or load-file-name buffer-file-name)))
(defcustom neo-term-host-program (expand-file-name "build/neo-term-host.exe" neo-term--directory)
  "Path to the native helper."
  :type 'file)
(defcustom neo-term-backend 'auto
  "Backend selected before the shell starts."
  :type '(choice (const auto)
                 (const bundled-conpty)
                 (const system-conpty)
                 (const classic)))
(defcustom neo-term-no-conpty nil "Exclude both ConPTY backends."
  :type 'boolean)
(defcustom neo-term-shell (or (executable-find "pwsh.exe") "powershell.exe")
  "Shell executable."
  :type 'string)
(defcustom neo-term-shell-arguments '("-NoLogo" "-NoProfile")
  "Arguments passed directly to the shell."
  :type '(repeat string))
(defcustom neo-term-scrollback-lines 2000 "Maximum retained history lines."
  :type 'natnum)
(defcustom neo-term-shell-integration t
  "Track directories and prompts for ordinary PowerShell sessions."
  :type 'boolean)
(defcustom neo-term-copy-exclude-prompt t
  "Exclude a tracked prompt when copying the current line."
  :type 'boolean)
(defcustom neo-term-clear-scrollback-when-clearing nil
  "Also clear scrollback when clearing the console screen."
  :type 'boolean)
(defcustom neo-term-buffer-name-string nil
  "Optional buffer name format containing %s for the terminal title."
  :type '(choice (const nil) string))
(defcustom neo-term-kill-buffer-on-exit nil
  "Kill the terminal buffer when its process exits."
  :type 'boolean)
(defcustom neo-term-enable-osc52 nil
  "Allow terminal OSC 52 output to copy text into the kill ring and clipboard."
  :type 'boolean)
(defcustom neo-term-enable-mouse t
  "Send mouse input when a ConPTY application requests mouse reporting."
  :type 'boolean)
(defconst neo-term--max-frame (* 4 1024 1024))
(defvar-local neo-term--process nil)
(defvar-local neo-term--session nil)
(defvar-local neo-term--pending (unibyte-string))
(defvar-local neo-term--lines [])
(defvar-local neo-term--history nil)
(defvar-local neo-term--cols 0)
(defvar-local neo-term--height 0)
(defvar-local neo-term--cursor '(0 . 0))
(defvar-local neo-term--visible t)
(defvar-local neo-term--alt nil)
(defvar-local neo-term--copy nil)
(defvar-local neo-term--updating nil)
(defvar-local neo-term--timer nil)
(defvar-local neo-term--requested-size nil)
(defvar-local neo-term--status "starting")
(defvar-local neo-term--glyph-cache nil)
(defvar-local neo-term--screen-start nil)
(defvar-local neo-term--row-markers [])
(defvar-local neo-term--rendered-lines [])
(defvar-local neo-term--rendered-history nil)
(defvar-local neo-term--render-layout nil)
(defvar-local neo-term--title "")
(defvar-local neo-term--raw-title nil)
(defvar-local neo-term--prompt-prefixes nil)
(defvar-local neo-term--mouse 0)
(defvar neo-term-mode-map)
(defvar neo-term-copy-mode-map)

(defun neo-term--frame (text)
  (let* ((bytes (encode-coding-string text 'utf-8 t))
         (size (length bytes)))
    (unless (<= 1 size neo-term--max-frame)
      (error "Invalid terminal frame size"))
    (concat (apply #'unibyte-string (cl-loop for shift from 0 to 24 by 8
                                             collect (logand 255 (ash size (- shift))))) bytes)))

(defun neo-term--send (command)
  (unless (process-live-p neo-term--process)
    (user-error "Terminal has exited"))
  (process-send-string neo-term--process (neo-term--frame command)))

(defun neo-term--consume (bytes)
  (setq neo-term--pending (concat neo-term--pending bytes))
  (let ((more t))
    (while (and more (>= (length neo-term--pending) 4))
      (let ((size (cl-loop for index below 4 sum
                           (ash (aref neo-term--pending index)
                                (* 8 index)))))
        (unless (<= 1 size neo-term--max-frame)
          (error "Invalid terminal frame size: %s" size))
        (if (< (length neo-term--pending)
               (+ 4 size))
            (setq more nil)
          (let ((payload (substring neo-term--pending 4 (+ 4 size))))
            (setq neo-term--pending (substring neo-term--pending (+ 4 size)))
            (neo-term--event (json-parse-string (decode-coding-string payload 'utf-8 t)
                                                :object-type 'alist
                                                :array-type 'array
                                                :null-object nil
                                                :false-object nil))))))))

(defun neo-term--event (event)
  (unless (eql (alist-get 'v event) 1)
    (error "Unsupported terminal protocol"))
  (pcase (alist-get 'type event)
    ("ready"
     (setq neo-term--session event
           neo-term--status (alist-get 'backend event)))
    ("screen"
     (neo-term--screen event))
    ("exit"
     (setq neo-term--status (format "exit %s"
                                    (alist-get 'code event))))
    ("error"
     (setq neo-term--status "error")
     (message "neo-term: %s"
              (alist-get 'message event)))
    (_ (error "Unknown terminal event")))
  (force-mode-line-update))

(defun neo-term--cell-face (cell)
  (let ((fg (aref cell 2))
        (bg (aref cell 3))
        (bits (aref cell 4)) result)
    (when (>= fg 0)
      (setq result (plist-put result
                              :foreground (format "#%06x" fg))))
    (when (>= bg 0)
      (setq result (plist-put result
                              :background (format "#%06x" bg))))
    (when (/= 0 (logand bits 1))
      (setq result (plist-put result
                              :weight 'bold)))
    (when (/= 0 (logand bits 2))
      (setq result (plist-put result
                              :underline t)))
    (when (/= 0 (logand bits 4))
      (setq result (plist-put result
                              :slant 'italic)))
    (when (/= 0 (logand bits 8))
      (setq result (plist-put result
                              :inverse-video t)))
    (when (/= 0 (logand bits 16))
      (setq result (plist-put result
                              :strike-through t)))
    result))

(defun neo-term--validate-line (cells cols)
  (unless (vectorp cells)
    (error "Invalid terminal cells"))
  (let ((width 0))
    (cl-loop for cell across cells do
             (unless (and (vectorp cell)
                          (= (length cell) 5)
                          (stringp (aref cell 0))
                          (<= 1 (length (aref cell 0)) 16)
                          (memq (aref cell 1) '(1 2))
                          (cl-loop for index from 2 to 3 always
                                   (and (integerp (aref cell index))
                                        (<= -1 (aref cell index) #xffffff)))
                          (integerp (aref cell 4))
                          (<= 0 (aref cell 4) 31))
               (error "Invalid terminal cell"))
             (cl-incf width (aref cell 1)))
    (unless (= width cols)
      (error "Terminal cell widths do not match columns"))))

(defun neo-term--screen (event)
  (neo-term--receive-clipboard (alist-get 'clipboard event))
  (when (alist-get 'title event)
    (neo-term--update-title (alist-get 'title event)))
  (when (alist-get 'history_cleared event)
    (setq neo-term--history nil))
  (let ((cols (alist-get 'cols event))
        (height (alist-get 'height event))
        (x (alist-get 'x event))
        (y (alist-get 'y event)))
    (unless (and (integerp cols)
                 (<= 2 cols 300)
                 (integerp height)
                 (<= 2 height 200)
                 (integerp x)
                 (<= 0 x cols)
                 (integerp y)
                 (<= 0 y (1- height)))
      (error "Invalid terminal dimensions or cursor"))
    (unless (and (= cols neo-term--cols)
                 (= height neo-term--height))
      (setq neo-term--lines (make-vector height nil)))
    (setq neo-term--cols cols
          neo-term--height height
          neo-term--cursor (cons x y)
          neo-term--visible (alist-get 'visible event)
          neo-term--alt (alist-get 'alt event))
    (setq neo-term--mouse (or (alist-get 'mouse event) 0))
    (unless (memq neo-term--mouse '(0 1 2 3))
      (error "Invalid terminal mouse mode"))
    (setq-local track-mouse (and neo-term-enable-mouse (> neo-term--mouse 1)))
    (cl-loop for row across (alist-get 'rows event) do
             (unless (and (vectorp row)
                          (= (length row) 2)
                          (integerp (aref row 0))
                          (<= 0 (aref row 0)
                              (1- height)))
               (error "Invalid terminal row"))
             (neo-term--validate-line (aref row 1) cols)
             (aset neo-term--lines (aref row 0)
                   (aref row 1)))
    (cl-loop for line across (alist-get 'history event) do
             (unless (stringp line)
               (error "Invalid terminal history"))
             (setq neo-term--history (nconc neo-term--history (list line))))
    (when (> (length neo-term--history) neo-term-scrollback-lines)
      (setq neo-term--history (nthcdr (- (length neo-term--history) neo-term-scrollback-lines)
                                      neo-term--history)))
    (cond (neo-term--copy nil)
          (noninteractive (neo-term--render))
          ((null neo-term--timer)
           (setq neo-term--timer (run-at-time .02 nil #'neo-term--render-buffer
                                              (current-buffer)))))))

(defun neo-term--render-buffer (buffer)
  (when (buffer-live-p buffer)
    (with-current-buffer buffer
      (setq neo-term--timer nil)
      (unless neo-term--copy (neo-term--render)))))

(defun neo-term--cell-display (text width face)
  (when (display-graphic-p)
    (let* ((pixels (* width (frame-char-width)))
           (key (list text width face (frame-parameter nil 'font)
                      (frame-char-width)
                      (frame-char-height)
                      (face-attribute 'fixed-pitch
                                      :family nil t)
                      (face-foreground 'default nil t)
                      (face-background 'default nil t)))
           (cached (gethash key neo-term--glyph-cache 'missing)))
      (if (not (eq cached 'missing)) cached
        (let* ((base-face (append face '(:inherit fixed-pitch)))
               (actual (string-pixel-width (propertize text 'face base-face)))
               (image nil))
          (when (/= actual pixels)
            (unless (image-type-available-p 'svg)
              (error "SVG support is required to fit terminal glyphs"))
            (let* ((height (frame-char-height))
                   (svg (svg-create pixels height))
                   (fg (or (plist-get face
                                      :foreground)
                           (face-foreground 'default nil t)))
                   (bg (or (plist-get face
                                      :background)
                           (face-background 'default nil t))))
              (when (plist-get face
                               :inverse-video)
                (cl-rotatef fg bg))
              (svg-text svg text
                        :x 0
                        :y (* height .8)
                        :font-size (* height .8)
                        :font-family (face-attribute 'fixed-pitch
                                                     :family nil t)
                        :font-weight (if (eq (plist-get face
                                                        :weight) 'bold) "bold" "normal")
                        :font-style (if (eq (plist-get face
                                                       :slant) 'italic) "italic" "normal")
                        :text-decoration (string-join (delq nil (list (when (plist-get face
                                                                                       :underline) "underline")
                                                                      (when (plist-get face
                                                                                       :strike-through) "line-through"))) " ")
                        :fill fg
                        :transform (format "scale(%s 1)"
                                           (/ (float pixels)
                                              (max 1 actual))))
              (setq image (svg-image svg
                                     :ascent 'center
                                     :scale 1
                                     :background bg))))
          (when (= (hash-table-count neo-term--glyph-cache) 512)
            (clrhash neo-term--glyph-cache))
          (puthash key image neo-term--glyph-cache))))))

(defun neo-term--render ()
  (let ((inhibit-read-only t)
        (neo-term--updating t)
        (layout (list neo-term--cols neo-term--height neo-term--alt
                      (frame-parameter nil 'font) (frame-char-width)
                      (frame-char-height) (face-foreground 'default nil t)
                      (face-background 'default nil t))))
    (if (equal layout neo-term--render-layout)
        (progn
          (neo-term--sync-history)
          (dotimes (row neo-term--height)
            (unless (equal (aref neo-term--lines row)
                           (aref neo-term--rendered-lines row))
              (goto-char (aref neo-term--row-markers row))
              (let ((begin (point)))
                (delete-region begin (line-end-position))
                (neo-term--insert-row row)
                (set-marker (aref neo-term--row-markers row) begin)))))
      (neo-term--rebuild-screen)
      (setq neo-term--render-layout layout))
    (setq neo-term--rendered-lines (copy-sequence neo-term--lines))
    (when (> neo-term--height 0)
      (set-marker neo-term--screen-start (aref neo-term--row-markers 0)))
    (goto-char (neo-term--cursor-position))
    (setq cursor-type (if neo-term--visible 'box nil))
    (dolist (window (get-buffer-window-list (current-buffer) nil t))
      (set-window-start window neo-term--screen-start t)
      (set-window-point window (point)))
    (neo-term--restore-display-protection)
    (set-buffer-modified-p nil)))

(defun neo-term--insert-row (row)
  (let ((begin (point))
        (cells (aref neo-term--lines row)))
    (if cells
        (cl-loop for cell across cells do (neo-term--insert-cell cell))
      (insert (make-string neo-term--cols ?\s)))
    (add-text-properties begin (point)
                         '(read-only t front-sticky (read-only)))
    (neo-term--mark-prompt begin (point))))

(defun neo-term--insert-history (lines)
  (dolist (line lines)
    (let ((begin (point)))
      (insert (propertize (concat line "\n")
                          'read-only t 'front-sticky '(read-only)))
      (neo-term--mark-prompt begin (1- (point))))))

(defun neo-term--mark-prompt (begin end)
  (let ((text (buffer-substring-no-properties begin end)))
    (dolist (prefix neo-term--prompt-prefixes)
      (when (string-prefix-p prefix text)
        (put-text-property begin (+ begin (length prefix)) 'neo-term-prompt t)))))

(defun neo-term--update-title (raw-title)
  (when (and (stringp raw-title) (<= (length raw-title) 16384)
             (not (equal raw-title neo-term--raw-title)))
    (setq neo-term--raw-title raw-title)
    (if (string-prefix-p "neo-term;" raw-title)
        (condition-case nil
            (let* ((json (decode-coding-string
                          (base64-decode-string (substring raw-title 9)) 'utf-8 t))
                   (metadata (json-parse-string json :object-type 'alist))
                   (directory (alist-get 'directory metadata))
                   (prompt (alist-get 'prompt metadata))
                   (title (alist-get 'title metadata)))
              (when (and (stringp directory)
                         (string-match-p "\\`[A-Za-z]:[/\\\\]" directory)
                         (file-directory-p directory))
                (setq default-directory (file-name-as-directory directory)))
              (when (and (stringp prompt) (< 0 (length prompt) 1024)
                         (not (string-match-p "[\r\n]" prompt))
                         (not (member prompt neo-term--prompt-prefixes)))
                (push prompt neo-term--prompt-prefixes)
                (setq neo-term--prompt-prefixes
                      (cl-subseq neo-term--prompt-prefixes
                                 0 (min 64 (length neo-term--prompt-prefixes)))
                      neo-term--render-layout nil))
              (when (stringp title) (neo-term--set-title title)))
          (error nil))
      (neo-term--set-title raw-title))))

(defun neo-term--set-title (title)
  (let ((clean (replace-regexp-in-string "[[:cntrl:]]" "" title)))
    (setq neo-term--title (substring clean 0 (min 256 (length clean)))))
  (when (and neo-term-buffer-name-string (not (string-empty-p neo-term--title)))
    (rename-buffer (format neo-term-buffer-name-string neo-term--title) t)))

(defun neo-term--receive-clipboard (texts)
  (when texts
    (unless (and (vectorp texts) (<= (length texts) 8))
      (error "Invalid terminal clipboard events"))
    (cl-loop for text across texts do
             (unless (and (stringp text) (<= (string-bytes text) 65536))
               (error "Invalid terminal clipboard text"))
             (when neo-term-enable-osc52 (kill-new text)))))

(defun neo-term--rebuild-screen ()
  (cl-loop for marker across neo-term--row-markers do (set-marker marker nil))
  (erase-buffer)
  (setq neo-term--rendered-history
        (unless neo-term--alt (copy-sequence neo-term--history)))
  (neo-term--insert-history neo-term--rendered-history)
  (setq neo-term--screen-start (copy-marker (point) t)
        neo-term--row-markers (make-vector neo-term--height nil))
  (dotimes (row neo-term--height)
    (aset neo-term--row-markers row (copy-marker (point) t))
    (neo-term--insert-row row)
    (set-marker (aref neo-term--row-markers row)
                (save-excursion (beginning-of-line) (point)))
    (unless (= row (1- neo-term--height))
      (insert (propertize "\n" 'read-only t))))
  (set-marker neo-term--screen-start
              (if (> neo-term--height 0)
                  (aref neo-term--row-markers 0)
                (point-max))))

(defun neo-term--history-overlap (current previous)
  (let ((remaining (length current))
        (candidate-length (length previous))
        found)
    (while (and current previous (not found))
      (when (and (eq (car current) (car previous))
                 (<= candidate-length remaining)
                 (cl-loop for old in previous for new in current always (eq old new)))
        (setq found previous))
      (unless found
        (setq previous (cdr previous))
        (cl-decf candidate-length)))
    found))

(defun neo-term--sync-history ()
  (let ((current (unless neo-term--alt neo-term--history)))
    (unless (equal current neo-term--rendered-history)
      (let* ((overlap (neo-term--history-overlap current neo-term--rendered-history))
             (retained (length overlap))
             (removed (- (length neo-term--rendered-history) retained)))
        (goto-char (point-min))
        (forward-line removed)
        (delete-region (point-min) (point))
        (goto-char neo-term--screen-start)
        (neo-term--insert-history (nthcdr retained current))
        (setq neo-term--rendered-history (copy-sequence current))))))

(defun neo-term--cursor-position ()
  (if (= neo-term--height 0)
      (point-min)
    (let* ((row (cdr neo-term--cursor))
           (position (marker-position (aref neo-term--row-markers row)))
           (cells (aref neo-term--lines row))
           (column 0))
      (if cells
          (cl-loop for cell across cells
                   while (< column (car neo-term--cursor))
                   do (cl-incf position (length (aref cell 0)))
                   (cl-incf column (aref cell 1)))
        (cl-incf position (car neo-term--cursor)))
      position)))

(defun neo-term--protect-display (_begin _end)
  (unless neo-term--updating
    (setq buffer-read-only t)
    (user-error "Terminal display cannot be edited; use copy mode to select text")))

(defun neo-term--restore-display-protection ()
  (setq buffer-read-only t)
  (add-hook 'before-change-functions #'neo-term--protect-display nil t))

(defun neo-term--insert-cell (cell)
  (let ((text (aref cell 0))
        (width (aref cell 1)))
    (set-char-table-range char-width-table (aref text 0) width)
    (cl-loop for index from 1 below (length text)
             do (set-char-table-range char-width-table (aref text index) 0))
    (let* ((face (neo-term--cell-face cell))
           (display (neo-term--cell-display text width face)))
      (insert (propertize text 'face face 'display display)))))

(defun neo-term--key-command (event)
  (let* ((basic (event-basic-type event))
         (modifiers (event-modifiers event))
         (bits (+ (if (memq 'shift modifiers) 1 0)
                  (if (memq 'meta modifiers) 2 0)
                  (if (memq 'control modifiers) 4 0)))
         (key (cdr (assq basic '((return . 1)
                                 (tab . 2)
                                 (backtab . 2)
                                 (backspace . 3)
                                 (escape . 4)
                                 (up . 5)
                                 (down . 6)
                                 (left . 7)
                                 (right . 8)
                                 (insert . 9)
                                 (delete . 10)
                                 (deletechar . 10)
                                 (home . 11)
                                 (end . 12)
                                 (prior . 13)
                                 (next . 14))))))
    (when (memq event '(13 9 27))
      (setq basic event
            bits (logand bits 3)))
    (when (eq basic 'backtab)
      (setq bits (logior bits 1)))
    (when (and (symbolp basic)
               (string-match "\\`f\\([0-9]+\\)\\'"
                             (symbol-name basic)))
      (let ((number (string-to-number (match-string 1 (symbol-name basic)))))
        (when (<= 1 number 24)
          (setq key (+ 256 number)))))
    (cond (key (format "K%s,%s" key bits))
          ((integerp basic)
           (pcase basic
             (13 (format "K1,%s"
                         (logand bits 3)))
             (9 (format "K2,%s"
                        (logand bits 3)))
             (127 (format "K3,%s" bits))
             (27 (format "K4,%s" bits))
             (_ (if (= 0 (logand bits 6))
                    (concat "T"
                            (char-to-string event))
                  (format "U%s,%s" basic bits)))))
          (t (user-error "Unsupported terminal key: %s" event)))))

(defun neo-term-send-key ()
  "Send the current key to the terminal."
  (interactive)
  (neo-term--send (neo-term--key-command last-command-event)))

(defun neo-term--resume-input ()
  (when neo-term--copy
    (neo-term-copy-mode)))

(defun neo-term-send-backspace ()
  "Send Backspace to the CLI."
  (interactive)
  (neo-term--send "K3,0"))

(defun neo-term-send-delete ()
  "Send Delete to the CLI."
  (interactive)
  (neo-term--send "K10,0"))

(defun neo-term-send-mouse (event)
  "Send mouse EVENT to a requesting application, or select and browse text."
  (interactive "e")
  (let* ((basic (event-basic-type event))
         (modifiers (event-modifiers event))
         (position (event-end event))
         (window (posn-window position))
         (coordinates (posn-col-row position))
         (button (cdr (assq basic '((mouse-1 . 1) (mouse-2 . 2) (mouse-3 . 3)
                                    (wheel-up . 4) (wheel-down . 5))))))
    (if (and neo-term-enable-mouse (> neo-term--mouse 0) (not neo-term--copy)
             (windowp window) (eq (window-buffer window) (current-buffer)))
        (let ((column (car coordinates))
              (row (cdr coordinates))
              (bits (+ (if (memq 'shift modifiers) 1 0)
                       (if (memq 'meta modifiers) 2 0)
                       (if (memq 'control modifiers) 4 0))))
          (when (and (integerp row) (integerp column)
                     (<= 0 row) (< row neo-term--height)
                     (<= 0 column) (< column neo-term--cols))
            (let ((pressed (or (memq 'down modifiers) (memq basic '(wheel-up wheel-down)))))
              (neo-term--send (format "M%s,%s,%s,%s" row column
                                      (if button (if pressed button (- button)) 0) bits)))))
      (cond ((memq basic '(wheel-up wheel-down))
             (neo-term-copy-mode 1)
             (if (eq basic 'wheel-up) (scroll-down-command 3) (scroll-up-command 3)))
            ((eq basic 'mouse-1)
             (if (memq 'down modifiers) (mouse-drag-region event) (mouse-set-point event)))))))

(defun neo-term-interrupt ()
  "Send Ctrl+C to the CLI."
  (interactive)
  (neo-term--resume-input)
  (neo-term--send "U99,4"))

(defun neo-term-clear (&optional invert)
  "Clear the console screen while keeping the CLI and scrollback."
  (interactive "P")
  (neo-term--resume-input)
  (when (if invert (not neo-term-clear-scrollback-when-clearing)
          neo-term-clear-scrollback-when-clearing)
    (neo-term--send "H"))
  (neo-term--send "L"))

(defun neo-term-clear-scrollback ()
  "Clear scrollback while keeping the current console screen."
  (interactive)
  (neo-term--resume-input)
  (neo-term--send "H"))

(defun neo-term-send-next-key ()
  "Read and send a key, including reserved Emacs keys."
  (interactive)
  (neo-term--send (neo-term--key-command (read-key "Terminal key: "))))
(defun neo-term-paste (text)
  "Paste TEXT into the terminal."
  (interactive (list (current-kill 0)))
  (neo-term--resume-input)
  (neo-term--send (concat "P" text)))
(defun neo-term-copy-mode (&optional argument)
  "Toggle selection and history browsing."
  (interactive "P")
  (setq neo-term--copy (if argument (> (prefix-numeric-value argument) 0)
                        (not neo-term--copy)))
  (setq buffer-read-only t)
  (use-local-map (if neo-term--copy neo-term-copy-mode-map neo-term-mode-map))
  (if neo-term--copy (setq cursor-type 'bar)
    (neo-term--render))
  (force-mode-line-update))

(defun neo-term-copy-mode-done (&optional invert)
  "Copy the selection or current line, then resume terminal input.
INVERT reverses `neo-term-copy-exclude-prompt' for this copy."
  (interactive "P")
  (unless neo-term--copy (user-error "Enable terminal copy mode first"))
  (let ((text
         (if (use-region-p)
             (buffer-substring-no-properties (region-beginning) (region-end))
           (let ((begin (line-beginning-position))
                 (end (line-end-position)))
             (when (and (if invert (not neo-term-copy-exclude-prompt)
                          neo-term-copy-exclude-prompt)
                        (get-text-property begin 'neo-term-prompt))
               (setq begin (next-single-property-change begin 'neo-term-prompt nil end)))
             (string-trim-right (buffer-substring-no-properties begin end))))))
    (kill-new text))
  (deactivate-mark)
  (neo-term-copy-mode -1))
(defun neo-term-close ()
  "Close the terminal and its child processes."
  (interactive)
  (kill-buffer (current-buffer)))
(defun neo-term-describe-session ()
  "Show the backend selected for this session."
  (interactive)
  (message "%s"
           (if neo-term--session
               (format "%s | %s | attempts: %S"
                       (alist-get 'backend neo-term--session)
                       (alist-get 'runtime neo-term--session)
                       (alist-get 'attempts neo-term--session))
             neo-term--status)))

(defvar neo-term-command-map
  (let ((map (make-sparse-keymap)))
    (define-key map (kbd "C-c") #'neo-term-interrupt)
    (define-key map (kbd "C-l") #'neo-term-clear-scrollback)
    (define-key map (kbd "M-l") #'neo-term-clear-scrollback)
    (define-key map (kbd "C-t") #'neo-term-copy-mode)
    (define-key map (kbd "C-v") #'neo-term-paste)
    (define-key map (kbd "C-k") #'neo-term-close)
    (define-key map (kbd "C-d") #'neo-term-describe-session)
    map))

(defvar neo-term-mode-map
  (let ((map (make-keymap))
        (prefix (make-sparse-keymap)))
    (set-char-table-range (nth 1 map) t #'neo-term-send-key)
    (define-key map [t] #'neo-term-send-key)
    (set-keymap-parent prefix ctl-x-map)
    (define-key prefix (kbd "C-t") #'neo-term-copy-mode)
    (define-key prefix (kbd "C-k") #'neo-term-close)
    (define-key prefix (kbd "C-d") #'neo-term-describe-session)
    (define-key prefix (kbd "C-q") #'neo-term-copy-mode)
    (define-key map (kbd "C-x") prefix)
    (define-key map (kbd "C-c") neo-term-command-map)
    (define-key map (kbd "C-l") #'neo-term-clear)
    (define-key map (kbd "DEL") #'neo-term-send-backspace)
    (dolist (key (append '(return tab backtab backspace escape up down left right insert
                                delete deletechar home end prior next)
                        (cl-loop for number from 1 to 24
                                 collect (intern (format "f%s" number)))))
      (dolist (modifiers '(nil (shift) (meta) (control) (shift meta)
                              (shift control) (meta control) (shift meta control)))
        (define-key map (vector (event-convert-list (append modifiers (list key))))
                    #'neo-term-send-key)))
    (define-key map [remap self-insert-command] #'neo-term-send-key)
    (define-key map [remap delete-backward-char] #'neo-term-send-backspace)
    (define-key map [remap backward-delete-char-untabify] #'neo-term-send-backspace)
    (define-key map [remap delete-char] #'neo-term-send-delete)
    (define-key map [remap read-only-mode] #'neo-term-copy-mode)
    (define-key map [down-mouse-1] #'mouse-drag-region)
    (define-key map [mouse-1] #'mouse-set-point)
    (dolist (event '(down-mouse-1 mouse-1 drag-mouse-1 down-mouse-2 mouse-2
                                 down-mouse-3 mouse-3 wheel-up wheel-down mouse-movement))
      (dolist (modifiers '(nil (shift) (meta) (control) (shift meta)
                              (shift control) (meta control) (shift meta control)))
        (define-key map (vector (event-convert-list (append modifiers (list event))))
                    #'neo-term-send-mouse)))
    (define-key map (kbd "C-g") #'keyboard-quit)
    (define-key map (kbd "C-q") #'neo-term-send-next-key)
    (define-key map (kbd "C-y") #'neo-term-paste)
    (define-key map [S-insert] #'neo-term-paste)
    map))
(defvar neo-term-copy-mode-map
  (let ((map (make-sparse-keymap)))
    (set-keymap-parent map special-mode-map)
    (define-key map (kbd "C-x C-t") #'neo-term-copy-mode)
    (define-key map (kbd "C-x C-q") #'neo-term-copy-mode)
    (define-key map [remap read-only-mode] #'neo-term-copy-mode)
    (define-key map (kbd "C-c") neo-term-command-map)
    (define-key map (kbd "M-w") #'kill-ring-save)
    (define-key map (kbd "RET") #'neo-term-copy-mode-done)
    (define-key map [return] #'neo-term-copy-mode-done)
    (define-key map (kbd "C-l") #'neo-term-clear)
    (define-key map "q" #'neo-term-copy-mode)
    map))

(defun neo-term--cleanup ()
  (when (timerp neo-term--timer)
    (cancel-timer neo-term--timer))
  (when (process-live-p neo-term--process)
    (ignore-errors (neo-term--send "C"))
    (delete-process neo-term--process)))

(define-derived-mode neo-term-mode special-mode "Neo-Term"
  "Windows terminal. C-c is the terminal command prefix; C-q sends a reserved key."
  (setq-local truncate-lines t)
  (setq-local buffer-undo-list t)
  (setq-local scroll-margin 0)
  (setq-local line-spacing 0)
  (setq-local char-width-table (copy-sequence char-width-table))
  (setq neo-term--glyph-cache (make-hash-table
                               :test 'equal))
  (setq-local show-trailing-whitespace nil)
  (setq-local mode-line-process '(:eval (format " [%s%s]" neo-term--status (if neo-term--copy
                                                                               " copy" ""))))
  (setq-local buffer-face-mode-face 'fixed-pitch)
  (buffer-face-mode 1)
  (neo-term--restore-display-protection)
  (add-hook 'post-command-hook #'neo-term--restore-display-protection nil t)
  (add-hook 'kill-buffer-hook #'neo-term--cleanup nil t))

(defun neo-term--size (window)
  (cons (max 2 (min 300 (window-body-width window)))
        (max 2 (min 200 (window-body-height window)))))
(defun neo-term--resize (_frame)
  (dolist (window (window-list))
    (with-current-buffer (window-buffer window)
      (when (and (derived-mode-p 'neo-term-mode)
                 (process-live-p neo-term--process))
        (let ((size (neo-term--size window)))
          (unless (equal size neo-term--requested-size)
            (setq neo-term--requested-size size)
            (neo-term--send (format "R%s,%s"
                                    (car size)
                                    (cdr size)))))))))
(add-hook 'window-size-change-functions #'neo-term--resize)

(defun neo-term--shell-arguments ()
  (if (and neo-term-shell-integration
           (member (downcase (file-name-base neo-term-shell)) '("powershell" "pwsh"))
           (cl-every (lambda (argument)
                       (member (downcase argument) '("-nologo" "-noprofile" "-noexit")))
                     neo-term-shell-arguments))
      (append neo-term-shell-arguments
              (unless (member "-noexit" (mapcar #'downcase neo-term-shell-arguments))
                '("-NoExit"))
              (list "-EncodedCommand"
                    (base64-encode-string
                     (encode-coding-string
                      (with-temp-buffer
                        (insert-file-contents (expand-file-name "shell/neo-term.ps1"
                                                               neo-term--directory))
                        (buffer-string))
                      'utf-16le t)
                     t)))
    neo-term-shell-arguments))

(defun neo-term--host-command (size)
  (append
   (list neo-term-host-program
         "--backend"
         (symbol-name neo-term-backend)
         "--cols"
         (number-to-string (car size))
         "--rows"
         (number-to-string (cdr size)))
   (when neo-term-no-conpty '("--no-conpty"))
   (list "--" neo-term-shell)
   (neo-term--shell-arguments)))

(defun neo-term--process-filter (process bytes)
  (when (buffer-live-p (process-buffer process))
    (with-current-buffer (process-buffer process)
      (condition-case failure
          (neo-term--consume bytes)
        (error
         (setq neo-term--status "protocol error")
         (message "neo-term: %s"
                  (error-message-string failure))
         (delete-process process))))))

(defun neo-term--process-sentinel (process event)
  (when (buffer-live-p (process-buffer process))
    (with-current-buffer (process-buffer process)
      (unless (string-prefix-p "exit " neo-term--status)
        (setq neo-term--status (string-trim event)))
      (force-mode-line-update)
      (when (and neo-term-kill-buffer-on-exit
                 (memq (process-status process) '(exit signal)))
        (kill-buffer (current-buffer))))))

;;;###autoload
(defun neo-term-other-window ()
  "Start a terminal in another window."
  (interactive)
  (let ((display-buffer-overriding-action '(display-buffer-pop-up-window)))
    (neo-term)))

;;;###autoload
(defun neo-term ()
  "Start a Windows terminal in a new buffer and return that buffer."
  (interactive)
  (unless (eq system-type 'windows-nt)
    (user-error "neo-term requires Windows"))
  (unless (file-executable-p neo-term-host-program)
    (user-error "Build the neo-term helper with build.ps1 first"))
  (let* ((buffer (generate-new-buffer "*neo-term*"))
         (size (neo-term--size (selected-window)))
         (directory default-directory))
    (with-current-buffer buffer
      (neo-term-mode)
      (setq default-directory directory
            neo-term--requested-size size)
      (condition-case error
          (setq neo-term--process
                (make-process
                 :name (buffer-name buffer)
                 :buffer buffer
                 :noquery t
                 :connection-type 'pipe
                 :coding 'binary
                 :command (neo-term--host-command size)
                 :filter #'neo-term--process-filter
                 :sentinel #'neo-term--process-sentinel))
        (error (kill-buffer buffer)
               (signal (car error)
                       (cdr error)))))
    (pop-to-buffer buffer)
    buffer))

(provide 'neo-term)
;;; neo-term.el ends here
