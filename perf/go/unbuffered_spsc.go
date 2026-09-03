package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync"
	"time"
)

func main() {
	iters := 2000000
	if s := os.Getenv("CC_UNBUF_ITERS"); s != "" {
		if v, err := strconv.Atoi(s); err == nil && v > 0 {
			iters = v
		}
	}
	runtime.GOMAXPROCS(runtime.NumCPU())
	ch := make(chan int)
	var wg sync.WaitGroup
	wg.Add(2)
	t0 := time.Now()
	go func() {
		defer wg.Done()
		for i := 0; i < iters; i++ {
			ch <- i
		}
	}()
	go func() {
		defer wg.Done()
		for i := 0; i < iters; i++ {
			<-ch
		}
	}()
	wg.Wait()
	elapsed := float64(time.Since(t0).Nanoseconds()) / 1e6
	pairs := float64(iters) / (elapsed / 1000.0)
	ns := elapsed * 1e6 / float64(iters)
	fmt.Printf("  unbuffered spsc: %.0f pairs/sec (%.3f ms, %.3f ns/pair, n=%d)\n",
		pairs, elapsed, ns, iters)
}
