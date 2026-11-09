// String escapes. Only length is shown for the control characters (a NUL
// would not read in a transcript); the non-ASCII ones are printed.
console.log("\x41\x4a\x4A1");
console.log("\x41\u0042\u{43}\u{000044}");
console.log("\xe9\u00e9\u20AC\u{1F600}");
console.log("\uD83D\uDE00" === "\u{1F600}");
console.log("\0".length, "a\0b".length);
console.log("\b\f\v\r\n\t".length);
console.log("\q\a\e\s\$\z\-");
console.log('it\'s', "say \"hi\"", '\\');
console.log("a\
b");
// A leading zero with no other digit is 0, in sloppy JavaScript too.
console.log(00, 000);
