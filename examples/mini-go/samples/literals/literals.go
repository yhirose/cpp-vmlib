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
}
