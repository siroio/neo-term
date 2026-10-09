;;; profile-prompts.el --- Measure prompt scans with retained history -*- lexical-binding: t; -*-
(defconst neo-term-prompt-profile-root
  (expand-file-name "../" (file-name-directory load-file-name)))
(load (or (getenv "NEO_TERM_PROFILE_SOURCE")
          (expand-file-name "neo-term" neo-term-prompt-profile-root)) nil t)
(defvar neo-term-prompt-profile-samples nil)
(advice-add 'neo-term--mark-logical-prompts :around
            (lambda (function &rest args)
              (let ((start (float-time)))
                (prog1 (apply function args)
                  (push (* 1000 (- (float-time) start)) neo-term-prompt-profile-samples)))))

(defun neo-term-prompt-profile-cells (text exact)
  (vconcat (cl-loop for char across text for index from 0
                    collect (vector (char-to-string char) 1 -1 -1 0
                                    (if exact (cond ((= index 0) 1) ((= index 3) 2) (t 0)) 0)))))

(dolist (exact '(t nil))
  (dolist (history-count '(0 200 2000))
    (with-temp-buffer
      (neo-term-mode)
      (setq neo-term-scrollback-lines 2000)
      (unless exact (setq neo-term--prompt-prefixes '("P> ")))
      (let* ((line (neo-term-prompt-profile-cells "P> old command" exact))
             (history (vconcat (cl-loop repeat history-count collect (vector line nil))))
             (screen (vconcat (append line (make-list (- 80 (length line)) [" " 1 -1 -1 0]))))
             (event `((cols . 80) (height . 24) (x . 5) (y . 0) (visible . t)
                      (rows . ,(vector (vector 0 screen))) (history . ,history)))
             totals)
        (neo-term--screen event)
        (setf (alist-get 'history event) [])
        (setq neo-term-prompt-profile-samples nil)
        (dotimes (index 100)
          (let ((new (copy-sequence screen)) (start (float-time)))
            (aset new 5 (vector (if (cl-evenp index) "a" "b") 1 -1 -1 0 0))
            (setf (alist-get 'rows event) (vector (vector 0 new)))
            (neo-term--screen event)
            (push (* 1000 (- (float-time) start)) totals)))
        (setq totals (sort totals #'<)
              neo-term-prompt-profile-samples (sort neo-term-prompt-profile-samples #'<))
        (princ (format "PROMPTS=%s history=%s count=%s update-median=%.4fms prompt-median=%.4fms\n"
                       (if exact 'exact 'prefix) history-count (length totals)
                       (/ (+ (nth 49 totals) (nth 50 totals)) 2)
                       (/ (+ (nth 49 neo-term-prompt-profile-samples)
                             (nth 50 neo-term-prompt-profile-samples)) 2)))))))
