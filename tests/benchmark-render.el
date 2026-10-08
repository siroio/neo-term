;;; benchmark-render.el -*- lexical-binding: t; no-byte-compile: t; -*-
(require 'benchmark)
(load (or (getenv "NEO_TERM_BENCH_SOURCE")
          (expand-file-name "../neo-term.el" (file-name-directory load-file-name))) nil t)

(with-temp-buffer
  (neo-term-mode)
  (let* ((line (make-vector 80 ["A" 1 -1 -1 0]))
         (rows (vconcat (cl-loop for row below 24 collect (vector row line))))
         (event `((v . 1) (cols . 80) (height . 24) (x . 0) (y . 0)
                  (visible . t) (rows . ,rows)
                  (history . ,(vconcat (make-list 2000 "history"))))))
    (neo-term--screen event)
    (setf (alist-get 'rows event) [] (alist-get 'history event) [])
    (princ (format "cursor-only/200=%S\n"
                   (benchmark-run 200 (neo-term--screen event))))
    (let ((counter 0))
      (princ (format "one-changed-row/200=%S\n"
                     (benchmark-run 200
                       (let ((changed (copy-sequence line)))
                         (aset changed 0
                               (vector (if (cl-evenp (cl-incf counter)) "B" "C")
                                       1 -1 -1 0))
                         (setf (alist-get 'rows event) (vector (vector 0 changed)))
                         (neo-term--screen event))))))))
