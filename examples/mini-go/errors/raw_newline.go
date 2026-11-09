// An interpreted string literal cannot hold a line break: go says "newline in string".
package main

import "fmt"

func main() {
	fmt.Println("a
b")
}
