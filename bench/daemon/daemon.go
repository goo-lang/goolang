// The daemon shape — Go reference. A faithful translation of daemon.goo:
// same splits, same map, same appends, same join. Go reclaims what it builds,
// which is exactly the property under measurement.
package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
)

func handle(req string) string {
	fields := strings.Split(req, ",")
	counts := map[string]int{}
	total := 0
	for i := 0; i < len(fields); i++ {
		f := strings.TrimSpace(fields[i])
		n, err := strconv.Atoi(f)
		if err == nil {
			total = total + n
			counts[f] = counts[f] + 1
		} else {
			counts[strings.ToUpper(f)] = 1
		}
	}
	parts := []string{}
	for i := 0; i < len(fields); i++ {
		parts = append(parts, strings.TrimSpace(fields[i]))
	}
	return strconv.Itoa(total) + ":" + strconv.Itoa(len(counts)) + ":" + strings.Join(parts, "|")
}

func main() {
	requests := 50000
	if len(os.Args) > 1 {
		n, err := strconv.Atoi(os.Args[1])
		if err == nil {
			requests = n
		}
	}
	last := ""
	for i := 0; i < requests; i++ {
		last = handle("1, 2, 3, four, 5, six, 7")
	}
	fmt.Println(len(last))
}
