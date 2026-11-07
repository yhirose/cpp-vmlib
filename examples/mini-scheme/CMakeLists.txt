# mini-scheme is the front end that shows a *boundary* rather than a
# recipe: the escape half of call/cc is a TryCatch and works, and the
# re-entrant half is the "multi-shot continuations" entry in this README's
# own list of what stays out of reach.
#
# The goldens are `guile`'s output (test/goldens.sh). The smallest
# front end: an s-expression grammar is four rules where the others need
# forty to a hundred and fifty.
add_front_end(mini-scheme EXT scm ORACLE guile)
