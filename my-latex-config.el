(defun my-config-latex ()
  (add-hook 'doc-view-mode-hook 'auto-revert-mode)
  (setq TeX-view-program-selection '((output-pdf "PDF Tools"))
        TeX-view-program-list
        '(("PDF Tools" TeX-pdf-tools-sync-view))
        TeX-source-correlate-start-server
        t))
