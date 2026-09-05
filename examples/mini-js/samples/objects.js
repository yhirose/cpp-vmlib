// Objects and arrays: string-keyed properties, indexing, and the
// JavaScript rules the library deliberately leaves to a front end --
// a missing key or an out-of-range index is `undefined`, not a trap.

const p = { x: 1, y: 2, label: 'origin' };
show('read', [p.x, p['y'], p.label]);
show('missing', [p.z, typeof p.z]);
p.x = 10;
p['y'] = 20;
p.extra = true;
show('write', p);
show('keys', Object.keys(p));

const nested = { a: { b: { c: 'deep' } }, list: [1, [2, 3]] };
show('nested', [nested.a.b.c, nested.list[1][0]]);
nested.a.b.c = 'changed';
show('write through', nested.a.b.c);

const xs = [10, 20, 30];
show('array', [xs.length, xs[0], xs[2], xs[3], xs[-1]]);
xs[1] = 21;
xs.push(40);
show('mutated', xs);
show('popped', [xs.pop(), xs]);
show('slice', [xs.slice(1), xs.slice(0, 2), xs.slice(-2), xs.slice(5)]);
show('join', [xs.join('-'), xs.join(), [].join('-')]);

// Arrays and objects are references; a copy is something a program writes.
const alias = xs;
alias.push(99);
show('reference', [xs.length, alias === xs]);

let sum = 0;
for (const v of [1, 2, 3, 4, 5]) { sum += v; }
show('for-of array', sum);

let letters = '';
for (const ch of 'abc') { letters += ch + '.'; }
show('for-of string', letters);

let seen = [];
for (const k of Object.keys(p)) { seen.push(k + '=' + fmt(p[k])); }
show('for-of keys', seen);

// break and continue, by depth: the inner loop is the one they name.
let pairs = [];
for (let i = 0; i < 3; i++) {
  for (let j = 0; j < 3; j++) {
    if (j === 1) continue;
    if (j === 2) break;
    pairs.push(String(i) + String(j));
  }
}
show('break/continue', pairs);

const obj = { greet: (who) => 'hi ' + who };
show('function property', obj.greet('there'));
