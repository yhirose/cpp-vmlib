// The sign is part of the constant: -2147483649 does not fit an int32 either.
package main

import "fmt"

func main() {
	var x int32 = -2147483649
	fmt.Println(x)
}
