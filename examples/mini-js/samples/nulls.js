// null is not undefined. Both are "nothing", but they are two values: the
// literal `null` is the VM's Void, and everything the language itself
// produces as absent (a missing property, an empty return, a missing
// argument) is undefined, the VM's nil.

const u = undefined;
const n = null;
console.log(n === u, n === n, u === u, n !== u);
console.log(typeof n, typeof u);
console.log(String(n), String(u), 'a' + n, 'a' + u);
// An array's join renders both as the empty string.
console.log([n, u].join('-'), [1, n, 2].join('-'));
console.log(!n, !u, n || 1, u || 2, n && 3);
if (n) console.log('null is truthy');
else console.log('null is falsy');
console.log(n ? 'yes' : 'no');
console.log(n === ({}).missing, ({ a: n }).a === n, ({ a: u }).a === u);

function noReturn() {}
function emptyReturn() {
  return;
}
function retNull() {
  return null;
}
console.log(noReturn() === u, emptyReturn() === u, retNull() === n);
console.log(noReturn() === n, retNull() === u);
function second(a, b) {
  return b;
}
console.log(second(1) === u, second(1, null) === n, [1][5] === u);

// What a trap says about null, as V8 words it.
function full(f) {
  try {
    f();
  } catch (e) {
    console.log(e.name + ': ' + e.message);
  }
}
full(() => n.x);
full(() => n[0]);
full(() => { n.x = 1; });
full(() => n());
full(() => null());
full(() => null.x);
full(() => u.x);

// throw null: what catch receives is null itself.
try {
  throw null;
} catch (e) {
  console.log(e === n, e === u, typeof e, String(e));
}

// `new` keeps its instance when the constructor returns null.
class C {
  constructor() {
    this.v = 1;
    return null;
  }
}
console.log(new C().v);

// new Map(null) and new Set(null) are empty, as with undefined.
console.log(new Map(n).size, new Set(n).size, new Map(u).size);
const m = new Map();
m.set(n, 'null key');
m.set(u, 'undefined key');
console.log(m.size, m.get(n), m.get(u));
const s = new Set([n, u, n]);
console.log(s.size, s.has(n), s.has(u));

// A promise carries null as it does any value.
async function awaited() {
  const v = await null;
  console.log(v === n, v === u);
}
awaited();
