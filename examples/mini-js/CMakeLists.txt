# mini-js is the dynamic-language counterpart to mini-go: where that one
# proves the managed, statically-typed recipes against `go run`, this one
# proves Closures, Exceptions, Generators, Maps and Coroutines + Scheduler
# (async/await over a Promise it writes itself) against `node`.
#
# The goldens are `node`'s output, each sample run with samples/prelude.js
# ahead of it, as test/goldens.sh runs it for `node`: the prelude is the
# formatting the samples print through, written in the source language so
# that both sides build their output with the same code (see README.md).
add_front_end(mini-js EXT js LANG js LEAD samples/prelude.js)
