// The minus is part of the constant, so -9223372036854775809 is out of
// range too (go: "... (overflows)").
package main

import "fmt"

func main() {
	var b int64 = -9223372036854775809
	fmt.Println(b)
}
