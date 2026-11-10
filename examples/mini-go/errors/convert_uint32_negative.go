// go says "cannot convert -1 (untyped int constant) to type uint32".
package main

import "fmt"

func main() {
	fmt.Println(uint32(-1))
}
