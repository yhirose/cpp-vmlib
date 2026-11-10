// go says "cannot convert 2147483648 (untyped int constant) to type int32".
package main

import "fmt"

func main() {
	fmt.Println(int32(2147483648))
}
