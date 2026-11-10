// go says "cannot use 3.5 (untyped float constant) as int32 value in variable declaration (truncated)".
package main

import "fmt"

func main() {
	var x int32 = 3.5
	fmt.Println(x)
}
