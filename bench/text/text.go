// String and slice heavy text processing — Go reference. See text.goo for the
// rationale and for why Index is used in place of Contains.
package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
)

func main() {
	lines := 200000
	if len(os.Args) > 1 {
		n, err := strconv.Atoi(os.Args[1])
		if err == nil {
			lines = n
		}
	}
	hits := 0
	total := 0
	for i := 0; i < lines; i++ {
		line := "alpha beta gamma " + strconv.Itoa(i) + " delta epsilon zeta"
		words := strings.Fields(line)
		kept := []string{}
		for j := 0; j < len(words); j++ {
			if strings.Index(words[j], "a") >= 0 {
				hits = hits + 1
				kept = append(kept, strings.ToUpper(words[j]))
			}
			total = total + len(words[j])
		}
		joined := strings.Join(kept, ",")
		total = total + strings.Count(joined, ",")
	}
	fmt.Println(total + hits)
}
