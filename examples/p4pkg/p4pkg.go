package p4pkg

type Accumulator struct {
	total int
}

func (a *Accumulator) Add(n int) {
	a.total = a.total + n
}

func (a Accumulator) Total() int {
	return a.total
}
