package main

import "fmt"

// An integer divided by zero: "panic: runtime error: integer divide by
// zero" in go.
func main() {
	var a int64 = 1
	var z int64 = 0
	fmt.Println(a / z)
}
