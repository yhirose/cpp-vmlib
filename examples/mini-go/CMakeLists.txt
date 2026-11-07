# The goldens are `go run`'s output. Each sample is a real, standalone
# `package main` in a directory of its own, so that no two declare
# `func main` side by side.
add_front_end(mini-go EXT go ORACLE "go run")
