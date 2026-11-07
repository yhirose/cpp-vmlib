# mini-go is a second example, proving the managed-language recipes
# (Fixed-width integers, `float`, Static calls, Struct fields, Switch)
# against real Go rather than culebra's dynamic PL/0.
#
# The goldens are Go's output: test/languages.sh builds each sample with
# `go build` and runs the binary. Each sample is a real, standalone
# `package main` in a directory of its own, so that no two declare
# `func main` side by side.
add_front_end(mini-go EXT go LANG go)
