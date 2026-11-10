// go says "cannot convert 4294967296 (untyped int constant) to type uint32".
package main

import "fmt"

func main() {
	fmt.Println(uint32(4294967296))
}
