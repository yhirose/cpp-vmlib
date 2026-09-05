// function* and yield. The library's GenResume answers a fresh
// {value, done} object -- which is already exactly what `g.next()`
// evaluates to in JavaScript, so the iterator protocol needs no
// translation at all here, only a name.

function* count(n) {
  let i = 1;
  while (i <= n) {
    yield i;
    i = i + 1;
  }
  return 'done';
}

const g = count(2);
show('protocol', [g.next(), g.next(), g.next(), g.next()]);

let collected = [];
for (const v of count(4)) { collected.push(v); }
show('for-of', collected);

// A generator is lazy: nothing in the body runs until the first resume,
// and it stops again at every yield.
let effects = [];
function* noisy() {
  effects.push('started');
  yield 'a';
  effects.push('resumed');
  yield 'b';
  effects.push('finished');
}
const n = noisy();
show('before first next', effects);
n.next();
show('after first next', effects);
n.next();
n.next();
show('drained', effects);

// yield is an expression: it evaluates to whatever the *next* resume sent.
function* echo() {
  const first = yield 'ready';
  const second = yield 'got ' + fmt(first);
  return 'got ' + fmt(second);
}
const e = echo();
show('sent values', [e.next().value, e.next('one').value, e.next('two')]);

// Closing early runs the body's pending finally, then reports done.
let cleanup = [];
function* withCleanup() {
  try {
    yield 1;
    yield 2;
  } finally {
    cleanup.push('cleaned');
  }
}
const w = withCleanup();
show('first', w.next());
show('returned', w.return('early'));
show('cleanup ran', cleanup);
show('after return', w.next());

// A throw delivered at the yield point is catchable by the body itself.
function* catcher() {
  try {
    yield 'a';
  } catch (err) {
    yield 'caught ' + err.message;
  }
  yield 'still going';
}
const c = catcher();
show('throw into', [c.next().value, c.throw(new Error('in')).value,
                    c.next().value]);

// Generators compose: one drives another with an ordinary for-of.
function* doubled(src) {
  for (const v of src) { yield v * 2; }
}
let out = [];
for (const v of doubled(count(3))) { out.push(v); }
show('composed', out);
show('typeof', [typeof count, typeof g]);
