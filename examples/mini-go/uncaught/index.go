package main

import "fmt"

// A subscript past the end of a slice ends the program with go's panic:
// "panic: runtime error: index out of range [5] with length 3" (go then
// prints the goroutine's trace and exits with status 2).
func main() {
	var xs []int64 = []int64{1, 2, 3}
	var i int64 = 5
	fmt.Println(xs[i])
}
