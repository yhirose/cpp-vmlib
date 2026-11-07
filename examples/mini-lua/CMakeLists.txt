# The goldens are `lua`'s output (samples/gen_golden.sh) -- whose language
# specification *requires* the behaviour samples/tailcalls.lua checks.
add_front_end(mini-lua EXT lua ORACLE lua)
