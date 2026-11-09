// A legacy octal literal: 010 is 8 in a sloppy script (node prints 8) and a
// SyntaxError in strict code; this front end refuses it as not supported.
console.log(010);
