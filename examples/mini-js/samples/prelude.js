// The one formatting function every sample prints through, prepended to
// each of them (for Node by samples/gen_golden.sh, for this front end by
// the CLI's own multi-file form). Why it exists: `console.log` of an array
// or an object is Node's util.inspect, a renderer with its own rules about
// quoting, spacing, depth and colour that has nothing to do with the
// language -- and reproducing it here would be work that proves nothing.
// Formatting in the source language instead means both sides run the *same*
// code to build the string, and what the comparison is left testing is the
// only thing worth testing: whether the two agree about what the program
// computes.
//
// It is also, incidentally, the largest program in this subset, and its
// own first test: closures, recursion, `for` over arrays, string building,
// `typeof`, and mutual recursion between two hoisted declarations.

function fmtIn(v) {
  // Inside a container a string shows its quotes, the way util.inspect and
  // every other JavaScript printer do it.
  if (typeof v === 'string') return "'" + v + "'";
  return fmt(v);
}

function fmt(v) {
  if (v === undefined) return 'undefined';
  const t = typeof v;
  if (t === 'number' || t === 'boolean') return String(v);
  if (t === 'string') return v;
  if (t === 'function') return '[Function]';
  if (Array.isArray(v)) {
    let s = '[';
    for (let i = 0; i < v.length; i++) {
      if (i > 0) s += ', ';
      s += fmtIn(v[i]);
    }
    return s + ']';
  }
  // An Error prints as name: message. Its own keys are not enumerable in
  // real JavaScript, so Object.keys would render `{}` on Node and the two
  // sides would disagree about a value they actually agree on.
  if (v.name !== undefined && v.message !== undefined) {
    return v.name + ': ' + v.message;
  }
  const keys = Object.keys(v);
  if (keys.length === 0) return '{}';
  let out = '{ ';
  for (let i = 0; i < keys.length; i++) {
    if (i > 0) out += ', ';
    out += keys[i] + ': ' + fmtIn(v[keys[i]]);
  }
  return out + ' }';
}

function show(label, v) {
  console.log(label + ': ' + fmt(v));
}

// What a function throws, as a string -- so that a sample can compare the
// *shape* of a failure without an uncaught throw ending the run.
function thrown(f) {
  try {
    f();
    return 'no throw';
  } catch (e) {
    return 'threw ' + fmt(e);
  }
}
