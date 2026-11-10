// A call of a non-function ends the program with the Error V8 throws,
// naming the callee as V8 prints it: "TypeError: o.f is not a function".
// The position is this implementation's own, the `.` of the member call
// (V8 reports the property name's column), so only the message is node's.
const o = {};
o.f();
