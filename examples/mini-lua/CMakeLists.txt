# mini-lua proves Tail calls, as mini-scheme does -- Lua guarantees them,
# so `lua` is an oracle for the property rather than an implementation
# that happens to agree -- and is the only front end that hands
# coroutines to the program the way CoroCreate/CoroResume/CoroYield were
# named for.
#
# The goldens are `lua`'s output (test/goldens.sh) -- whose language
# specification *requires* the behaviour samples/tailcalls.lua checks.
add_front_end(mini-lua EXT lua LANG lua)
