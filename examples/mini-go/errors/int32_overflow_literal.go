// An untyped constant must fit its type: go says "cannot use 2147483648 (untyped int constant) as int32 value in variable declaration (overflows)".
package main

import "fmt"

func main() {
	var x int32 = 2147483648
	fmt.Println(x)
}
