// Scalar loop, no allocation — Go reference. See scalar.goo for the rationale.
package main

import "fmt"

func main() {
	var x int64 = 1
	var acc int64 = 0
	i := 0
	for i < 1000000000 {
		x = x*6364136223846793005 + 1442695040888963407
		acc = acc + (x >> 33)
		i = i + 1
	}
	fmt.Println(acc)
}
