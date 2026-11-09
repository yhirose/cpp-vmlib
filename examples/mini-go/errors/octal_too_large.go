// go says "octal escape value 256 > 255".
package main

import "fmt"

func main() {
	fmt.Println("\400")
}
