// Closures: the reason a variable becomes a Cell instead of a frame slot,
// and the reason a loop body's `let` gets a new one on every iteration.

function counter() {
  let n = 0;
  return function () { n = n + 1; return n; };
}
const c1 = counter();
const c2 = counter();
show('independent', [c1(), c1(), c2(), c1()]);

// ES2023 14.7.4.4: each iteration of a `let` loop gets its own binding, so
// three closures made here remember three different numbers. `var` would
// not, which is why this subset does not have it.
const fs = [];
for (let i = 0; i < 3; i++) {
  fs.push(function () { return i; });
}
show('per-iteration', [fs[0](), fs[1](), fs[2]()]);

// A block is a scope, and a closure over a block-scoped binding keeps it
// alive after the block is gone.
const held = [];
{
  const secret = 'inner';
  held.push(() => secret);
}
show('block scope', held[0]());

// Two levels of nesting: `x` is a capture in the middle function and a
// capture again in the inner one -- the forwarding table that fills it
// belongs to the site building the closure, not to the function.
function outer(x) {
  return function (y) {
    return function (z) { return x + y + z; };
  };
}
show('nested captures', outer(1)(2)(3));

// Hoisted declarations see each other, including above themselves.
show('mutual recursion', isEven(10));
function isEven(n) { return n === 0 ? true : isOdd(n - 1); }
function isOdd(n) { return n === 0 ? false : isEven(n - 1); }

const twice = (f) => (v) => f(f(v));
show('higher order', twice((n) => n * 3)(2));

// A parameter that a nested function captures is copied out of its slot
// into a cell on entry -- the calling convention stays about locals only.
function adder(n) { return (m) => n + m; }
show('captured parameter', [adder(10)(5), adder(100)(5)]);

let total = 0;
for (const v of [1, 2, 3, 4]) { total += v; }
show('accumulate', total);
