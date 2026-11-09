// An octal escape has exactly three digits, so \0 is an error: go says "invalid character '\"' in octal escape".
package main

import "fmt"

func main() {
	fmt.Println("\0")
}
