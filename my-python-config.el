(defun config-my-python ()
  (with-eval-after-load 'python
    (remove-hook 'python-mode-hook #'python-setup-shell)
    (add-hook 'python-mode-hook
              (lambda ()
                (setq python-shell-interpreter "python3")))))
