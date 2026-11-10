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

	// `%` at each width. The int32 line is the edge the Fixed-width
	// integers recipe is about: Go's remainder keeps the dividend's sign,
	// and a normalized operand gets that for free -- as does a uint32 past
	// int32's range, which needs no unsigned form of the operator because
	// nothing about it has a sign bit to reinterpret.
	var neg int32 = -2147483648
	fmt.Println(neg % 3)
	fmt.Println(neg % -1)
	fmt.Println(u % 7)
	fmt.Println(big % 1000000007)

	// The least int64 over -1, which the spec answers with itself and a
	// remainder of 0: the quotient overflows, and wraps.
	var least int64 = -big - 1
	var minus int64 = -1
	fmt.Println(least / minus)
	fmt.Println(least % minus)

	// A constant converted to, or assigned to, an integer type must be
	// exactly representable (see errors/ for the ones that are not); a
	// float constant with no fraction is. A variable converts at run time,
	// truncating toward zero.
	var three int32 = 3.0
	fmt.Println(three)
	fmt.Println(int64(2.0))
	fmt.Println(uint32(4294967295.0))
	fmt.Println(int64(9223372036854775807.0))
	fmt.Println(int64(9007199254740993.0))
	fmt.Println(int64(2) * 3.0)
	var f float64 = 3.9
	fmt.Println(int64(f))
	var nf float64 = -3.9
	fmt.Println(int64(nf))
}
