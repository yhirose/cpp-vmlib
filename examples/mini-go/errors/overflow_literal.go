// go refuses an untyped constant that does not fit an int64: "cannot use
// 9223372036854775808 (untyped int constant) as int64 value in variable
// declaration (overflows)". The wording varies by Go version.
package main

import "fmt"

func main() {
	var b int64 = 9223372036854775808
	fmt.Println(b)
}
