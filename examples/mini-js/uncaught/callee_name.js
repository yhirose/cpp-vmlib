// The same for a plain name: "TypeError: x is not a function". The position
// is this implementation's own, the `(` of the call (V8 reports the name's
// column), so only the message is node's.
const x = 3;
x();
