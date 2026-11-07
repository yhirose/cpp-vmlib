# mini-csharp is statically typed like mini-go, but erases its types where
# mini-go's decide the lowering -- which is the point: it parses types,
# erases them, and then wants nothing the runtime does not already have,
# classes and inheritance included. Its oracle is `dotnet`.
#
# The goldens are `dotnet`'s output (samples/gen_golden.sh).
add_front_end(mini-csharp EXT cs ORACLE dotnet)
