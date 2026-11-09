// 010 is octal 8 in Go (it prints 8); this front end has no octal literals.
package main

import "fmt"

func main() {
	var x int64 = 010
	fmt.Println(x)
}
