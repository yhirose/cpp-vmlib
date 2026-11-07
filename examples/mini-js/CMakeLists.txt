# The goldens are `node`'s output, each sample run with samples/prelude.js
# ahead of it, as gen_golden.sh ran it for `node`: the prelude is the
# formatting the samples print through, written in the source language so
# that both sides build their output with the same code (see README.md).
add_front_end(mini-js EXT js ORACLE node LEAD samples/prelude.js)
