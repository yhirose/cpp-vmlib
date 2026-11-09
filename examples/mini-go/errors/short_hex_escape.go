// go says "invalid character 'g' in hexadecimal escape".
package main

import "fmt"

func main() {
	fmt.Println("\x4g")
}
