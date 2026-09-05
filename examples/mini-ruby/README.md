# mini-ruby

The eighth front end, and the only one that uses **`FnArity`** and
**`ArgCount`** -- two intrinsics `vmlib.h` carries for a case no other
language here makes visible.

Both introduce themselves in the header:

> **FnArity** -- "A function value's declared parameter count... The one
> fact a front end needs to check 'this callback takes two arguments'
> before calling it."
>
> **ArgCount** -- "How many arguments the running function was called with
> -- the count the caller supplied, not num_params. Only interesting under
> `Func::lenient_arity`, where the two can differ."

Ruby is where both are *program-visible*. `Proc#arity` is FnArity. And
Ruby's own distinction between its two kinds of callable is exactly
`lenient_arity` plus one ArgCount test:

| | Ruby | Here |
|---|---|---|
| a proc | extra arguments dropped, missing ones nil | `Func::lenient_arity` |
| a lambda | a mismatch raises `ArgumentError` | the same flag, plus `if ArgCount != num_params: raise` in the prologue |

`samples/procs.rb` is that table, run against `ruby`.

`ensure` is the third thing: a Defer, reached here from a third direction
after culebra's `defer` and Python's `with` -- which is a reasonable
argument for the tag being where it is rather than being three features.

## Running

```
build/examples/mini-ruby/mini-ruby [--dump-ir] [--dump-bc] PROGRAM.rb
```

## Blocks, and the one convention that carries them

Ruby passes a block *out of band*: it is not an argument, it does not
appear in the parameter list, and `yield` reaches it without naming it.
This IR's calling convention has no out-of-band channel, so this front end
makes one:

> **Every function's parameter 0 is the block it was passed**, nil when
> there was none.

Everything else follows from that with no further machinery:

* `yield a` is a call of parameter 0.
* `block_given?` is parameter 0 not being nil.
* `&blk` in a parameter list *names* parameter 0 rather than declaring a
  new one -- so it can be called, passed on with `f(&blk)`, and asked its
  arity.
* `Proc#arity` is `FnArity` minus one, because FnArity counts it.
* And a lambda's ArgCount test compares against `num_params`, which counts
  it too -- so the one cancels the other and the check is exact.

The cost is one nil per call. It is the same trade examples/mini-culebra
makes for `self` and examples/mini-lua makes for its results array: a
calling convention the IR does not have, written over the one it does.

## A top-level method is a variable; a class's is a table entry

Outside a class, a `def` binds a *name*, exactly the way an assignment
does, and a method that calls another method captures it -- the free-
variable propagation every front end here already has, with no method
table and no lookup, for the same reason the top-level README gives for
why this IR has captures and not static links:

> A static link would assume the defining activation is still on the
> stack, which closures break; a per-function capture list would break on
> self-recursion for the same reason.

`samples/closures.rb` has mutual recursion between two top-level methods,
and nothing special happens for it.

Inside a `class`, there *is* a table, because dispatch has to be dynamic:
`obj.speak` cannot know at compile time which class `obj` turned out to
be. A class is an object holding its base's table and its methods (keyed
`"\x02" + name`, a prefix no Ruby identifier can start with); an instance
is a plain object pointing back at its class. Every question a class
system answers -- an ordinary call, `super`, `is_a?`, `to_s`/`inspect`/
`==` on a plain object -- is the same walk, `$mfind`, climbing `kBaseKey`
links until a key is found or the chain runs out:

```
$mfind(t, key):
  while typeof(t) == "object":
    if t has key: return t[key]
    if not (t has base): break
    t = t[base]
  return nil
```

Every method call in this front end tries this walk *first* -- `$classof`
on the receiver, then `$mfind` for the name -- and only what it does not
claim falls through to the builtin table (`each`, `to_s`, `length`, ...).
That is safe to do unconditionally because a plain array, string or hash
never carries a class of its own, so the walk returns nil for them
immediately and costs one cheap type test. It also means a user's own
`each` or `to_s` on a class instance always wins, with no special-casing
per name the way examples/mini-python needs for `get`/`count` (those
collide with an ordinary object's *own* type; a class instance here is
structurally distinct from every builtin container).

`ClassName.new(args)` is recognized at the one place `ClassName` is used
as a receiver rather than a value: it builds `{"\x01c": <the table>}` and
calls `initialize` through the same `$mfind` walk, so a subclass with no
`initialize` of its own inherits its base's automatically. `super(args)`
looks up the *base* of the class the calling method was declared in
(captured once, at `resolve_def` time, not read off the instance), which
is what makes a three-level override chain resolve correctly regardless
of which subclass actually called it.

`samples/classes.rb` is inheritance, `super`, `attr_accessor`,
`is_a?`/`instance_of?`, and `to_s`/`==` overrides on a plain object, all
checked against `ruby`.

## What Ruby does not share with the VM's defaults

* **Only `nil` and `false` are false.** `0` and `""` are true, where
  `Value::truthy()` calls `0` false -- and its comment names Lua and
  JavaScript disagreeing as the reason it refuses to decide. Ruby is a
  third answer.
* **`/` and `%` on two Integers floor**, where `BinOp::Div` truncates and
  `BinOp::Mod` is C's.
* **`+` concatenates** strings and arrays.
* **A whole Float prints with a point** -- `to_display`'s output plus the
  one rule its comment leaves to a front end.
* **An assignment is an expression** whose value is what was assigned,
  which matters because a method answers its body's last value:
  `lambda { n += 1 }` returns the new `n`. `Tag::Assign` yields nothing,
  so the binder reads the variable back.

## Where the grammar fought back

Two bugs are worth recording, because both surfaced as a type error pages
away from their cause and both have the same root: **a newline in Ruby is a
terminator, not whitespace.**

```ruby
puts i
(1..3).each { |k| ... }     # read as i(1..3) -> "cannot call int"

x = "outer"
[1].each { |x| ... }        # read as "outer"[1] -> "string index must be an int"
```

The fix is that every token which can *finish* an expression -- a literal,
an identifier, a closing `)`, `]`, `}` or `end` -- skips horizontal space
only, and newlines are consumed at the statement level and inside brackets
instead. That is what Ruby's own grammar does, and the two failures above
are exactly why it does it. The other front ends here get away with
treating a newline as whitespace because their expressions cannot be
continued by a bare `(` or `[`.

A third, smaller one: the pre-scan that gathers the names a body assigns
runs before any block's parameters exist, so `[1].each { |x| x = "inner" }`
recorded the *outer* `x` as the target. Resolution knew better, so the
emitter asks resolution rather than the pre-scan.

## The ternary, and the ambiguity that looked fatal but wasn't

The original cut of this grammar left the ternary out because a Ruby
method name may end in `?` or `!`, and `x ? a : b` looked impossible to
tell from a predicate named `x?` without the whitespace rule Ruby itself
resorts to. It turns out `ident`'s own greediness already resolves it:
`ident <- ... [a-zA-Z0-9_]* [?!]? > hs` binds the `?` into the token only
when it is *adjacent* -- `x?` lexes as one identifier, but `x ? a : b`
lexes `x`, then leaves the `?` for `ternary` to find, because a space
before it stops the identifier's own optional suffix from matching. Ruby
draws exactly the same line for exactly the same reason.

## case/when, and what `===` means here

Ruby's `case` tests each `when` value against the subject with `===`,
which means something different per value: a `Range` asks inclusion, a
class asks `is_a?`, anything else falls back to `==`. `$caseeq` is that
three-way branch, so `when 4..10`, `when SomeClass`, and `when 1, 2, 3`
all work from the one helper -- see `samples/control.rb`.

## Symbols are strings, quietly

A symbol has no `Value` tag of its own, so `:name` is a string literal
the same as examples/mini-scheme's own symbols are, and `sym.class` would
say `"String"` here against Ruby's `"Symbol"` -- the one place
`samples/control.rb` does not ask. Everywhere a program actually uses a
symbol (a hash key, a `case`/`when` tag, `attr_accessor :x`) the two
behave identically, since Ruby hashes and compares a symbol by its name.

## A default is evaluated in the method's own scope

Unlike examples/mini-python's, whose default closes over the *enclosing*
scope once at `def` time, Ruby re-evaluates a default at every call that
needs it, in the method's *own* scope -- so `def f(a, b = a + 1)` can read
an earlier parameter. Nothing about the calling convention has to change
for this: every call already supplies the block as argument 0, so
`ArgCount` lines up with a parameter's position in the declared list
exactly, and the prologue can ask "was parameter `i` supplied?" as
`ArgCount > i` -- one check per default, no capture, no cell.

## What is absent, and what quietly differs

Absent: modules, splats (`*args`) and keyword arguments (both would need
a call-site convention change like examples/mini-python's, which would
also blur `FnArity`/`ArgCount` back into meaninglessness -- the one thing
this front end exists to keep sharp), `method_missing`, operator
overloading beyond `==`, multiple inheritance, string methods beyond
`upcase`/`downcase`/`length`, `Struct`, `require`, and a real exception
hierarchy -- `rescue` here catches everything by default, matching Ruby's
own bare `rescue`.

Quietly different:

* **`rescue` has no class filter**, so `rescue ValueError` is not a thing
  here; every `rescue` catches every raise.
* **`raise` always makes a RuntimeError**, and `e.message` is the string
  it was given.
* **Bignums are absent**: an Integer that leaves int64 wraps.
  examples/mini-python is where that recipe lives.
* **`sort` is an insertion sort** over the array, with no block form. The
  collections a sample sorts are small, and a comparison sort is not what
  this front end is here to show.
* **`obj.attr += 1` is not supported** through an attr_accessor-style
  setter method (only plain `obj.attr = v`), because augmenting would
  need to call the getter first and this front end does not thread that
  through -- write directly to `@attr` from inside the class instead.

## Testing

`ctest -R mini-ruby-samples` runs the eight samples and requires each one's
output to match a golden file captured from `ruby` -- see PL/0's own README
for why an external, independent oracle is what "passing" means here.

`samples/gen_golden.sh` regenerates the goldens, by hand and never from the
build.

All eight samples are clean under ASan/UBSan/LeakSanitizer and under
`COREIR_GC_STRESS=1`.
