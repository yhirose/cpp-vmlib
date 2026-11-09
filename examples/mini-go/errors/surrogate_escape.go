// go says "escape is invalid Unicode code point U+D800".
package main

import "fmt"

func main() {
	fmt.Println("\uD800")
}
