;;; protocol-driver.el -*- lexical-binding: t; no-byte-compile: t; -*-
(load (expand-file-name "../neo-term.el" (file-name-directory load-file-name)) nil t)
(let* ((port (string-to-number (pop command-line-args-left)))
       (arguments (json-parse-string (decode-coding-string
                                     (base64-decode-string (pop command-line-args-left)) 'utf-8 t)
                                    :array-type 'array))
       (neo-term-module-program (pop command-line-args-left))
       (buffer (generate-new-buffer "*neo-term-protocol-test*"))
       (pending (unibyte-string))
       (control (make-network-process :name "neo-term-test-control" :host "127.0.0.1"
                                      :service port :coding 'binary :noquery t :sentinel #'ignore)))
  (unwind-protect
      (with-current-buffer buffer
        (neo-term-mode)
        (fset 'neo-term--process-filter
              (lambda (_process bytes)
                (when (process-live-p control) (process-send-string control bytes))))
        (set-process-filter
         control
         (lambda (_process bytes)
           (setq pending (concat pending bytes))
           (let ((more t))
             (while (and more (>= (length pending) 4))
               (let ((size (cl-loop for index below 4 sum (ash (aref pending index) (* 8 index)))))
                 (if (< (length pending) (+ size 4))
                     (setq more nil)
                   (with-current-buffer buffer
                     (neo-term--module-command
                      (process-get neo-term--process 'neo-term-module)
                      (decode-coding-string (substring pending 4 (+ size 4)) 'utf-8 t)))
                   (setq pending (substring pending (+ size 4)))))))))
        (condition-case failure
            (progn
              (neo-term--use-module)
              (setq neo-term--process
                    (make-network-process :name "neo-term-test-output" :buffer buffer
                                          :host "127.0.0.1" :family 'ipv4 :service t :server t
                                          :coding 'binary :noquery t
                                          :log #'neo-term--module-connection
                                          :sentinel #'neo-term--process-sentinel))
              (let ((session (neo-term--module-start
                              (process-contact neo-term--process :service) arguments default-directory
                              (expand-file-name "runtime" (file-name-directory neo-term-module-program))
                              (vconcat process-environment))))
                (process-put neo-term--process 'neo-term-module (aref session 0))
                (process-put neo-term--process 'neo-term-token (aref session 1)))
              (while (and (process-live-p control) (process-live-p neo-term--process))
                (accept-process-output nil .01)))
          (error (process-send-string control
                                      (neo-term--frame (json-serialize
                                                        `((type . "error") (v . 1)
                                                          (message . ,(error-message-string failure))))))))
        (accept-process-output control .05))
    (when (buffer-live-p buffer) (kill-buffer buffer))
    (delete-process control)))
