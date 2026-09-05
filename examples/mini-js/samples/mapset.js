// Map and Set. What makes them different from an object in JavaScript is
// what makes them a different thing in the library too: an object's keys
// are strings, and a Map's are values -- IntrinsicId::MapNew.

const m = new Map();
m.set('a', 1);
m.set('b', 2);
show('basic', [m.get('a'), m.get('b'), m.get('missing'), m.size]);
show('has', [m.has('a'), m.has('zz')]);

// Keys that are not strings, which is the whole point: an object would
// have turned each of these into its own text.
m.set(1, 'number one');
m.set(true, 'the boolean');
show('value keys', [m.get(1), m.get(true), m.get('1'), m.size]);

m.set('a', 'replaced');
show('replace', [m.get('a'), m.size]);
show('delete', [m.delete('a'), m.delete('a'), m.size]);

// Insertion order, which Map promises and an object's key order happens
// to share here.
let entries = [];
for (const e of m) { entries.push(fmt(e[0]) + '=' + fmt(e[1])); }
show('iteration', entries);

const s = new Set();
s.add('x');
s.add('y');
s.add('x');
show('set', [s.size, s.has('x'), s.has('z')]);
s.add(1);
s.add(1);
show('set value keys', [s.size, s.has(1), s.has('1')]);
show('set delete', [s.delete('y'), s.size]);

let members = [];
for (const v of s) { members.push(v); }
show('set iteration', members);

// A Map keyed by a reference: two arrays with equal contents are two
// different keys, because a key is a value and these are two values.
const k1 = [1];
const k2 = [1];
const byRef = new Map();
byRef.set(k1, 'first');
byRef.set(k2, 'second');
show('reference keys', [byRef.size, byRef.get(k1), byRef.get(k2)]);

show('typeof', [typeof m, typeof s]);
