// go says "cannot convert 3.9 (untyped float constant) to type int64 (truncated)": a constant conversion must be exact.
package main

import "fmt"

func main() {
	fmt.Println(int64(3.9))
}
