// null is not undefined. Both are "nothing", but they are two values: the
// literal `null` is the VM's Void, and everything the language itself
// produces as absent (a missing property, an empty return, a missing
// argument) is undefined, the VM's nil.

const u = undefined;
const n = null;

// undefined: what the language itself produces as absent, as before.
function noReturn() {}
function second(a, b) {
  return b;
}
console.log(noReturn() === u, second(1) === u, ({}).missing === u);
console.log(typeof u, String(u), 'a' + u, !u, u || 2);

// null: its own value.
function retNull() {
  return null;
}
console.log(n === u, n !== u, n === n, retNull() === n, retNull() === u);
console.log(n === noReturn(), n === ({}).missing, second(1, null) === n);
console.log(typeof n, String(n), 'a' + n);
// An array's join renders both as the empty string.
console.log([n, u].join('-'), [1, n, 2].join('-'));
console.log(!n, n || 1, n && 3, n ? 'yes' : 'no');
if (n) console.log('null is truthy');
else console.log('null is falsy');
console.log(({ a: n }).a === n, ({ a: u }).a === u);

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
