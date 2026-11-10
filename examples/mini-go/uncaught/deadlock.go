package main

import "fmt"

// The only goroutine is main, blocked receiving from a channel nobody
// sends on: go prints "fatal error: all goroutines are asleep - deadlock!"
// then "goroutine 1 [chan receive]:" and a trace, and a compiled binary
// exits 2. This runtime prints the first line after the file name, exit 1.
func main() {
	var c chan int32 = make(chan int32)
	fmt.Println("waiting")
	<-c
}
