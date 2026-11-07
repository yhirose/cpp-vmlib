# mini-culebra is the front end whose oracle is culebra itself. It is the
# only one that exercises the mechanisms vmlib.h describes in culebra's own
# terms -- the owned stack, a Scope's explicit release order and
# RunOptions::entry_frame_drops -- and its standard library is the Host
# functions recipe, vm::RunOptions::natives.
#
# The goldens are `culebra`'s own output (test/goldens.sh): the
# language this front end is a slice of.
add_front_end(mini-culebra EXT cul LANG culebra)
