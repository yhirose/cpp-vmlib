// A case constant must fit the subject type too: go says "cannot use 4294967296 (untyped int constant) as uint32 value in switch case (overflows)".
package main

import "fmt"

func main() {
	var x uint32 = 1
	switch x {
	case 4294967296:
		fmt.Println("big")
	}
}
