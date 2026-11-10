package main

import "fmt"

// main blocks on a channel nobody sends on: go's "fatal error: all
// goroutines are asleep - deadlock!" (then the goroutine's trace, exit 2).
func main() {
	var c chan int32 = make(chan int32)
	fmt.Println("waiting")
	<-c
}
