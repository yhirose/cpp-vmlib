// go says "cannot convert 3000000000.0 (untyped float constant 3e+09) to type int32".
package main

import "fmt"

func main() {
	fmt.Println(int32(3000000000.0))
}
