package main

import "fmt"

func square(x int32) int32 {
	return x * x
}

func main() {
	var a int32 = 2000000000
	var b int32 = 2000000000
	var sum int32 = a + b
	fmt.Println(sum)

	var u uint32 = 4000000000
	var v uint32 = 1000000000
	var usum uint32 = u + v
	fmt.Println(usum)

	fmt.Println(square(50000))
	fmt.Println(square(3))

	var big int64 = 9223372036854775807
	fmt.Println(big)

	var truncated int32 = int32(big)
	fmt.Println(truncated)

	var widened float64 = float64(a)
	fmt.Println(widened)
}
