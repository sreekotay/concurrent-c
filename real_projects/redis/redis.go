// redis.go — same shape as redis_idiomatic.ccs, in Go.
//
// Sharded mutexes, one goroutine per conn, hold covers DB only, encode/write
// after unlock, pipeline window.  Not full Redis.  Errors are values on the
// normal path.
//
// Unlike the CCS server, RESP argv here are owned copies from parse (Go's
// bufio reader does not expose a stable borrow into the fill buffer).  The
// hold / window / shard geometry still mirrors the idiomatic port.
//
// Build: go build -o out/redis_go redis.go
// Run:   ./out/redis_go [addr]   # default 127.0.0.1:6380

package main

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"hash/fnv"
	"io"
	"net"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	readBufCap     = 64 * 1024
	outBufCap      = 8 * 1024
	pipelineWindow = 16
	maxArgs        = 16
	defaultListen  = "127.0.0.1:6380"
)

type holdKind int

const (
	holdNone holdKind = iota
	holdKey
	holdKeys
	holdAll
)

type cmdKind int

const (
	cmdPing cmdKind = iota
	cmdEcho
	cmdGet
	cmdSet
	cmdDel
	cmdExists
	cmdIncr
	cmdMGet
	cmdDBSize
	cmdFlushDB
	cmdUnknown
)

type entry struct {
	val       string
	expiresAt int64 // unix ms; 0 = none
}

type shard struct {
	mu   sync.Mutex
	data map[string]entry
}

type DB struct {
	shards []*shard
	mask   uint32
}

func newDB(ncpu int) *DB {
	n := 1
	for n < ncpu && n < 64 {
		n <<= 1
	}
	s := make([]*shard, n)
	for i := range s {
		s[i] = &shard{data: make(map[string]entry)}
	}
	return &DB{shards: s, mask: uint32(n - 1)}
}

func (db *DB) shardIdx(key string) int {
	h := fnv.New32a()
	_, _ = h.Write([]byte(key))
	return int(h.Sum32() & db.mask)
}

func (db *DB) live(e entry, now int64) bool {
	return e.expiresAt == 0 || e.expiresAt > now
}

// --- hold policy (same idea: name → how many locks) ---

func holdFor(k cmdKind) holdKind {
	switch k {
	case cmdPing, cmdEcho:
		return holdNone
	case cmdGet, cmdSet, cmdIncr:
		return holdKey
	case cmdDel, cmdExists, cmdMGet:
		return holdKeys
	case cmdDBSize, cmdFlushDB:
		return holdAll
	default:
		return holdNone
	}
}

func parseKind(name string) cmdKind {
	switch strings.ToUpper(name) {
	case "PING":
		return cmdPing
	case "ECHO":
		return cmdEcho
	case "GET":
		return cmdGet
	case "SET":
		return cmdSet
	case "DEL":
		return cmdDel
	case "EXISTS":
		return cmdExists
	case "INCR":
		return cmdIncr
	case "MGET":
		return cmdMGet
	case "DBSIZE":
		return cmdDBSize
	case "FLUSHDB":
		return cmdFlushDB
	default:
		return cmdUnknown
	}
}

// --- RESP (minimal, real enough) ---

type cmd struct {
	kind cmdKind
	args [][]byte // owned copies from parse — window-local
}

func readRESPCommand(br *bufio.Reader) (cmd, error) {
	line, err := br.ReadBytes('\n')
	if err != nil {
		return cmd{}, err
	}
	line = bytes.TrimRight(line, "\r\n")
	if len(line) == 0 {
		return cmd{}, errIncomplete
	}

	// Inline: PING\r\n
	if line[0] != '*' {
		parts := bytes.Fields(line)
		if len(parts) == 0 {
			return cmd{}, errIncomplete
		}
		args := make([][]byte, len(parts))
		for i, p := range parts {
			args[i] = append([]byte(nil), p...)
		}
		return cmd{kind: parseKind(string(args[0])), args: args}, nil
	}

	// Array: *N\r\n$len\r\n...\r\n
	n, err := strconv.Atoi(string(line[1:]))
	if err != nil || n < 0 || n > maxArgs {
		return cmd{}, fmt.Errorf("bad argc")
	}
	args := make([][]byte, 0, n)
	for i := 0; i < n; i++ {
		hdr, err := br.ReadBytes('\n')
		if err != nil {
			return cmd{}, err
		}
		hdr = bytes.TrimRight(hdr, "\r\n")
		if len(hdr) == 0 || hdr[0] != '$' {
			return cmd{}, fmt.Errorf("bad bulk hdr")
		}
		blen, err := strconv.Atoi(string(hdr[1:]))
		if err != nil || blen < 0 {
			return cmd{}, fmt.Errorf("bad bulk len")
		}
		buf := make([]byte, blen+2)
		if _, err := io.ReadFull(br, buf); err != nil {
			return cmd{}, err
		}
		args = append(args, buf[:blen]) // drop \r\n; own the slice
	}
	if len(args) == 0 {
		return cmd{}, fmt.Errorf("empty command")
	}
	return cmd{kind: parseKind(string(args[0])), args: args}, nil
}

var errIncomplete = errors.New("incomplete")

func encodeSimple(s string) []byte { return []byte("+" + s + "\r\n") }
func encodeError(s string) []byte  { return []byte("-ERR " + s + "\r\n") }
func encodeInt(n int64) []byte     { return []byte(":" + strconv.FormatInt(n, 10) + "\r\n") }
func encodeBulk(b []byte) []byte {
	if b == nil {
		return []byte("$-1\r\n")
	}
	return []byte("$" + strconv.Itoa(len(b)) + "\r\n" + string(b) + "\r\n")
}
func encodeOK() []byte { return encodeSimple("OK") }

// --- execute under hold (DB only) ---

type reply struct {
	wire []byte
}

func (db *DB) withShards(idxs []int, fn func()) {
	sorted := append([]int(nil), idxs...)
	sort.Ints(sorted)
	u := sorted[:0]
	last := -1
	for _, x := range sorted {
		if x != last {
			u = append(u, x)
			last = x
		}
	}
	for _, i := range u {
		db.shards[i].mu.Lock()
	}
	defer func() {
		for i := len(u) - 1; i >= 0; i-- {
			db.shards[u[i]].mu.Unlock()
		}
	}()
	fn()
}

func (db *DB) execute(c cmd, now int64) reply {
	switch c.kind {
	case cmdPing:
		if len(c.args) > 1 {
			return reply{wire: encodeBulk(c.args[1])}
		}
		return reply{wire: encodeSimple("PONG")}
	case cmdEcho:
		if len(c.args) < 2 {
			return reply{wire: encodeError("wrong number of arguments")}
		}
		return reply{wire: encodeBulk(c.args[1])}
	case cmdUnknown:
		return reply{wire: encodeError("unknown command")}
	}

	h := holdFor(c.kind)
	var out reply

	switch h {
	case holdNone:
		// already handled
	case holdKey:
		if len(c.args) < 2 {
			return reply{wire: encodeError("wrong number of arguments")}
		}
		key := string(c.args[1])
		si := db.shardIdx(key)
		db.withShards([]int{si}, func() {
			out = db.execKey(c, key, si, now)
		})
	case holdKeys:
		if len(c.args) < 2 {
			return reply{wire: encodeError("wrong number of arguments")}
		}
		idxs := make([]int, 0, len(c.args)-1)
		for _, a := range c.args[1:] {
			idxs = append(idxs, db.shardIdx(string(a)))
		}
		db.withShards(idxs, func() {
			out = db.execKeys(c, now)
		})
	case holdAll:
		idxs := make([]int, len(db.shards))
		for i := range idxs {
			idxs[i] = i
		}
		db.withShards(idxs, func() {
			out = db.execAll(c, now)
		})
	}
	return out
}

func (db *DB) execKey(c cmd, key string, si int, now int64) reply {
	sh := db.shards[si]
	switch c.kind {
	case cmdGet:
		e, ok := sh.data[key]
		if !ok || !db.live(e, now) {
			if ok {
				delete(sh.data, key)
			}
			return reply{wire: encodeBulk(nil)}
		}
		return reply{wire: encodeBulk([]byte(e.val))}
	case cmdSet:
		if len(c.args) < 3 {
			return reply{wire: encodeError("wrong number of arguments")}
		}
		sh.data[key] = entry{val: string(c.args[2])}
		return reply{wire: encodeOK()}
	case cmdIncr:
		e, ok := sh.data[key]
		var n int64
		if ok && db.live(e, now) {
			var err error
			n, err = strconv.ParseInt(e.val, 10, 64)
			if err != nil {
				return reply{wire: encodeError("value is not an integer")}
			}
		} else if ok {
			delete(sh.data, key)
			e = entry{}
		}
		n++
		sh.data[key] = entry{val: strconv.FormatInt(n, 10), expiresAt: e.expiresAt}
		return reply{wire: encodeInt(n)}
	}
	return reply{wire: encodeError("internal")}
}

func (db *DB) execKeys(c cmd, now int64) reply {
	switch c.kind {
	case cmdDel:
		var n int64
		for _, a := range c.args[1:] {
			key := string(a)
			sh := db.shards[db.shardIdx(key)]
			if e, ok := sh.data[key]; ok {
				if db.live(e, now) {
					n++
				}
				delete(sh.data, key)
			}
		}
		return reply{wire: encodeInt(n)}
	case cmdExists:
		var n int64
		for _, a := range c.args[1:] {
			key := string(a)
			sh := db.shards[db.shardIdx(key)]
			if e, ok := sh.data[key]; ok && db.live(e, now) {
				n++
			} else if ok {
				delete(sh.data, key)
			}
		}
		return reply{wire: encodeInt(n)}
	case cmdMGet:
		var buf bytes.Buffer
		keys := c.args[1:]
		buf.WriteString("*" + strconv.Itoa(len(keys)) + "\r\n")
		for _, a := range keys {
			key := string(a)
			sh := db.shards[db.shardIdx(key)]
			e, ok := sh.data[key]
			if !ok || !db.live(e, now) {
				if ok {
					delete(sh.data, key)
				}
				buf.WriteString("$-1\r\n")
				continue
			}
			buf.WriteString("$" + strconv.Itoa(len(e.val)) + "\r\n" + e.val + "\r\n")
		}
		return reply{wire: buf.Bytes()}
	}
	return reply{wire: encodeError("internal")}
}

func (db *DB) execAll(c cmd, now int64) reply {
	switch c.kind {
	case cmdDBSize:
		var n int64
		for _, sh := range db.shards {
			for k, e := range sh.data {
				if db.live(e, now) {
					n++
				} else {
					delete(sh.data, k)
				}
			}
		}
		return reply{wire: encodeInt(n)}
	case cmdFlushDB:
		for _, sh := range db.shards {
			sh.data = make(map[string]entry)
		}
		return reply{wire: encodeOK()}
	}
	return reply{wire: encodeError("internal")}
}

// --- connection: parse → window → acquire/exec → write ---

func handleClient(conn net.Conn, db *DB) {
	defer conn.Close()

	br := bufio.NewReaderSize(conn, readBufCap)
	out := make([]byte, 0, outBufCap)

	flush := func() error {
		if len(out) == 0 {
			return nil
		}
		_, err := conn.Write(out)
		out = out[:0]
		return err
	}

	for {
		// Pipeline window: block for the first command, then drain up to
		// pipelineWindow-1 more that are already in the read buffer
		// (same idea as CCS rr_cmd_next over a filled reader).
		for i := 0; i < pipelineWindow; i++ {
			if i > 0 && br.Buffered() == 0 {
				break
			}
			if i == 0 {
				_ = conn.SetReadDeadline(time.Now().Add(30 * time.Second))
			}
			c, err := readRESPCommand(br)
			if err != nil {
				if i == 0 && (errors.Is(err, io.EOF) || errors.Is(err, errIncomplete)) {
					_ = flush()
					return
				}
				if i > 0 {
					// Partial trailing frame: flush what we have and close.
					_ = flush()
					return
				}
				_, _ = conn.Write(encodeError(err.Error()))
				return
			}
			now := time.Now().UnixMilli()
			r := db.execute(c, now) // locks inside; wire already encoded
			if len(out)+len(r.wire) > outBufCap {
				if err := flush(); err != nil {
					return
				}
			}
			out = append(out, r.wire...)
		}
		if err := flush(); err != nil {
			return
		}
	}
}

func main() {
	addr := defaultListen
	if len(os.Args) > 1 {
		addr = os.Args[1]
	}
	db := newDB(runtime.NumCPU())
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "listen: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("redis-go idiomatic on %s shards=%d\n", addr, len(db.shards))
	for {
		c, err := ln.Accept()
		if err != nil {
			continue
		}
		go handleClient(c, db)
	}
}
