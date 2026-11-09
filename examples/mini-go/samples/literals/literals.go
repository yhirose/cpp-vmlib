package main

import "fmt"

// Integer literals are untyped constants: -9223372036854775808 is one
// constant, valid for an int64 (the minus is applied before the range is
// checked), though 9223372036854775808 alone is not (see errors/).
func main() {
	var hi int64 = 9223372036854775807
	var lo int64 = -9223372036854775808
	fmt.Println(hi)
	fmt.Println(lo)
	fmt.Println(-9223372036854775807 - 1)
	fmt.Println(5 - -3)
	var x int64 = 9223372036854775807
	fmt.Println(x)
	fmt.Println(-x - 1)
	switch lo {
	case -9223372036854775808:
		fmt.Println("lowest")
	default:
		fmt.Println("other")
	}
	// String escapes: \xHH and \ooo are bytes, \u and \U code points.
	fmt.Println("\x41\x4a\x4A\101\060")
	fmt.Println("\u0041\u00e9\u20AC\U0001F600")
	fmt.Println("a\tb\"c\\d")
	fmt.Println(len("\a\b\f\n\r\t\v\\\""))
	fmt.Println(len("\x00\000\xe9\377\u00e9"))
	// A constant must fit its type, at its edges (see errors/ for past them).
	var i32min int32 = -2147483648
	var i32max int32 = 2147483647
	var u32max uint32 = 4294967295
	fmt.Println(i32min)
	fmt.Println(i32max)
	fmt.Println(u32max)
	var seven int32 = 7
	fmt.Println(seven - -2147483648 + 2147483647)
	switch i32min {
	case -2147483648:
		fmt.Println("int32 lowest")
	}
	// A leading zero with no other digit is 0 (not an octal literal).
	fmt.Println(00)
	fmt.Println(000)
}
