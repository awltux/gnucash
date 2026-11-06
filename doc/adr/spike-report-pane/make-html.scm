;; Produce an HTML document with the GnuCash report engine, for the pane spike.
;; This is the real thing, not a hand-written string: gnc:make-html-document
;; builds the document object and gnc:html-document-render turns it into HTML.
(use-modules (gnucash report))

(define doc (gnc:make-html-document))
(gnc:html-document-set-title! doc "GnuCash report pane spike")
(gnc:html-document-add-object!
 doc (gnc:make-html-text "Rendered by the GnuCash report engine, not by hand."))

(display (gnc:html-document-render doc))
