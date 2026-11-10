// go says "cannot convert -3.9 (untyped float constant) to type int32 (truncated)".
package main

import "fmt"

func main() {
	fmt.Println(int32(-3.9))
}
