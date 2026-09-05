// throw / catch / finally. `finally` is a Defer inside the Scope wrapping
// the try, so it runs however the block is left -- and the two cases that
// prove that are a `return` out of a try and a throw nothing catches
// locally.

show('caught', thrown(() => { throw new Error('boom'); }));
show('thrown value', thrown(() => { throw 'a string'; }));
show('no throw', thrown(() => 1));

let order = [];
try {
  order.push('try');
  throw new TypeError('bad');
} catch (e) {
  order.push('catch ' + e.name);
} finally {
  order.push('finally');
}
order.push('after');
show('order', order);

// finally on the way out through `return`: the value is already decided
// when the deferred block runs, and the deferred block does not change it.
let trace = [];
function f() {
  try {
    trace.push('body');
    return 'returned';
  } finally {
    trace.push('finally on return');
  }
}
show('return through finally', [f(), trace]);

// finally on the way out through a throw, with no catch of its own.
let unwound = [];
function g() {
  try {
    throw new RangeError('deep');
  } finally {
    unwound.push('finally on throw');
  }
}
show('throw through finally', [thrown(g), unwound]);

// Nesting: the inner handler runs first, and rethrowing reaches the outer.
let nested = [];
try {
  try {
    throw new Error('inner');
  } catch (e) {
    nested.push('inner saw ' + e.message);
    throw new Error('rethrown');
  } finally {
    nested.push('inner finally');
  }
} catch (e) {
  nested.push('outer saw ' + e.message);
}
show('nested', nested);

// finally on the way out of a loop, through break.
let loop = [];
for (let i = 0; i < 3; i++) {
  try {
    if (i === 1) break;
    loop.push('body ' + String(i));
  } finally {
    loop.push('finally ' + String(i));
  }
}
show('break through finally', loop);

// The error object itself is just an object here.
try { throw new Error('has fields'); } catch (e) {
  show('fields', [e.name, e.message, typeof e]);
}
