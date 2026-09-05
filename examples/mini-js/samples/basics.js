// Scalars, operators, and the three questions this front end answers for
// itself rather than asking the VM: what is true, what is equal, and what
// is a value's type.

show('arith', [1 + 2 * 3, 7 - 2, 7 / 2, 7 % 3, -7 % 3, 2 / 4]);
show('div by zero', [1 / 0, -1 / 0, 0 / 0]);
show('concat', ['a' + 'b', 'n=' + 1, 1 + 'n', 'x' + true]);
show('unary', [-3, -(-3), !true, !!'a']);

// Value::truthy() in the library calls '' and NaN true, and says in its
// own comment that JavaScript disagrees -- so this front end lowers its
// own ToBoolean, and these are the cases that tell the two apart.
show('truthy', [!!0, !!'', !!'0', !!NaN, !![], !!{}]);

// BinOp::Eq refuses two values of different types ("a question a language
// answers, not the VM"); === answers false.
show('strict eq', [1 === 1, 1 === '1', 'a' === 'a', true === 1, NaN === NaN]);
show('strict ne', [1 !== 2, 1 !== '1']);
const a = [1, 2];
const b = [1, 2];
show('identity', [a === a, a === b]);

show('compare', [1 < 2, 2 <= 2, 'a' < 'b', 'b' >= 'c']);
show('typeof', [typeof 1, typeof 'x', typeof true, typeof undefined,
                typeof {}, typeof [], typeof show]);
show('logic', [1 && 2, 0 || 'x', '' || 'y', 'a' && 'b', 0 && 1]);
show('ternary', [1 > 2 ? 'yes' : 'no', 0 ? 'yes' : 'no']);

// Number::toString for the range the samples stay inside: whole numbers
// print without a point, and a double prints its shortest round trip.
show('numstr', [String(3), String(3.5), String(0.1 + 0.2), String(-0),
                String(1000000), String(NaN), String(Infinity)]);
show('strings', ['hello'.length, 'hello'[1], 'hello'.slice(1, 3),
                 'hello'.slice(-2), ''.length]);
