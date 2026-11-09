// There is no \q (and no \' in a string): go says "unknown escape".
package main

import "fmt"

func main() {
	fmt.Println("a\qb")
}
