;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;;  report-gui.scm -- the view side of the report engine
;;;
;;;  The report engine has no toolkit. A view that can show dialogs
;;;  installs itself here at startup; until it does, errors and warnings
;;;  go to the log file only, which is what a headless host wants.
;;;
;;;  Copyright 2026 The GnuCash contributors
;;;
;;;  This program is free software; you can redistribute it and/or
;;;  modify it under the terms of the GNU General Public License as
;;;  published by the Free Software Foundation; either version 2 of
;;;  the License, or (at your option) any later version.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

(define-module (gnucash report report-gui))

(export gnc:report-gui-running?
        gnc:report-gui-error
        gnc:report-gui-warning
        gnc:report-gui-progress
        gnc:report-set-gui-hooks!)

;; Each is a parameter holding a procedure. A view replaces them; the
;; defaults mean "no view attached".
(define gnc:report-gui-running? (make-parameter (lambda () #f)))
(define gnc:report-gui-error   (make-parameter (lambda (parent str) #f)))
(define gnc:report-gui-warning (make-parameter (lambda (parent str) #f)))
(define gnc:report-gui-progress (make-parameter (lambda (message percent) #f)))

(define (gnc:report-set-gui-hooks! running? error warning progress)
  (gnc:report-gui-running? running?)
  (gnc:report-gui-error error)
  (gnc:report-gui-warning warning)
  (gnc:report-gui-progress progress))
