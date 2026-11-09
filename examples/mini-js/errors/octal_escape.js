// A legacy octal escape is legal in a sloppy script (this prints "A") and
// a SyntaxError in strict code; this front end refuses it, as the strict one.
console.log("\101");
