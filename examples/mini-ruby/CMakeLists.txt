# mini-ruby is the only front end that uses FnArity and ArgCount: Ruby
# makes both program-visible, as `Proc#arity` and as the proc/lambda
# distinction (lenient_arity plus one ArgCount test).
#
# The goldens are `ruby`'s output (test/goldens.sh).
add_front_end(mini-ruby EXT rb LANG ruby)
