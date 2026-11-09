// go says "cannot use -1 (untyped int constant) as uint32 value in variable declaration (overflows)".
package main

import "fmt"

func main() {
	var x uint32 = -1
	fmt.Println(x)
}
