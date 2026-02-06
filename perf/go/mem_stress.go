package main

import (
	"fmt"
	"runtime"
	"time"
)

func main() {
	const count = 100000
	fmt.Printf("Spawning %d goroutines...\n", count)
	
	start := time.Now()
	done := make(chan bool)
	for i := 0; i < count; i++ {
		go func() {
			time.Sleep(10 * time.Second)
			done <- true
		}()
	}
	
	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	fmt.Printf("Goroutines spawned in %v\n", time.Since(start))
	fmt.Printf("Alloc = %v MiB\n", m.Alloc / 1024 / 1024)
	fmt.Printf("Sys = %v MiB\n", m.Sys / 1024 / 1024)
	fmt.Printf("NumGoroutine = %v\n", runtime.NumGoroutine())
	
	// Keep alive for RSS measurement from outside
	time.Sleep(5 * time.Second)
}
