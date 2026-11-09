package main

import "fmt"

// How a float prints: the shortest digits that round trip, in Go's
// layout. Plain digits from 1e-4 up to (not including) 1e6, an exponent
// outside that ("1e+06", "1e-05") with a two-digit minimum, never a ".0".
// A float32 shows its own shortest digits. Go has no exponent literals
// here, so the powers of ten are made by a loop, which is exact to 1e22.
func p10(n int64) float64 {
	var x float64 = 1.0
	for n > 0 {
		x = x * 10.0
		n = n - 1
	}
	return x
}

func main() {
	var zero float64 = 0
	var big float64 = p10(308)
	big = big * 10.0
	var tiny float64 = 1.0
	var k int64 = 1074
	for k > 0 {
		tiny = tiny / 2.0
		k = k - 1
	}
	var two53 float64 = 1.0
	k = 53
	for k > 0 {
		two53 = two53 * 2.0
		k = k - 1
	}
	fmt.Println(zero)
	fmt.Println(-zero)
	fmt.Println(0.1)
	fmt.Println(100000.0)
	fmt.Println(p10(6))
	fmt.Println(1234567.0)
	fmt.Println(1.0 / p10(4))
	fmt.Println(1.0 / p10(5))
	fmt.Println(1.0 / p10(7))
	fmt.Println(p10(15))
	fmt.Println(p10(22))
	fmt.Println(12345678901234567.0)
	var a float64 = 0.1
	var b float64 = 0.2
	fmt.Println(a + b)
	fmt.Println(tiny)
	fmt.Println(two53)
	fmt.Println(big)
	fmt.Println(-big)
	fmt.Println(big - big)

	var third float32 = 1.0
	var three float32 = 3.0
	fmt.Println(third / three)
	var m float32 = 100000.0
	fmt.Println(m)
	fmt.Println(m * 10.0)
	var s float32 = 16777216.0
	fmt.Println(s)
	var c float32 = 0.1
	fmt.Println(c)
	fmt.Println(float64(c))
	var tz float32 = 0
	fmt.Println(-tz)
}
