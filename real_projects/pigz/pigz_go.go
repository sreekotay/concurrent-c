package main

/*
#cgo LDFLAGS: -lz
#include <zlib.h>
#include <stdlib.h>

// Helper to perform deflate in one go, matching the C version's logic
int compress_block_zlib(unsigned char* in, size_t in_len, unsigned char* out, size_t* out_len, int level) {
    z_stream strm = {0};
    if (deflateInit2(&strm, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return Z_ERRNO;
    }
    strm.next_in = in;
    strm.avail_in = (uInt)in_len;
    strm.next_out = out;
    strm.avail_out = (uInt)*out_len;
    
    int ret = deflate(&strm, Z_FINISH);
    *out_len = strm.total_out;
    deflateEnd(&strm);
    return ret;
}
*/
import "C"

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"sync"
	"sync/atomic"
	"unsafe"
)

const (
	BlockSize = 128 * 1024
	ChanCap   = 16
)

type Block struct {
	seq    int64
	data   []byte
	isLast bool
}

type CompressedResult struct {
	seq         int64
	data        []byte
	crc         uint32
	originalLen int
	isLast      bool
}

var (
	gBytesIn     int64
	gBytesOut    int64
	gBlocksDone  int32
)

func compressBlockCGO(blk *Block, level int) (*CompressedResult, error) {
	// Pre-allocate buffer with some overhead
	maxOut := len(blk.data) + (len(blk.data) >> 3) + 256
	outBuf := make([]byte, maxOut+10+8)

	// Gzip header
	outBuf[0] = 0x1f
	outBuf[1] = 0x8b
	outBuf[2] = 0x08
	outBuf[3] = 0x00
	outBuf[4] = 0
	outBuf[5] = 0
	outBuf[6] = 0
	outBuf[7] = 0
	outBuf[8] = 0x00
	outBuf[9] = 0x03

	crc := crc32.ChecksumIEEE(blk.data)

	cIn := (*C.uchar)(unsafe.Pointer(&blk.data[0]))
	cInLen := C.size_t(len(blk.data))
	cOut := (*C.uchar)(unsafe.Pointer(&outBuf[10]))
	cOutLen := C.size_t(maxOut)

	ret := C.compress_block_zlib(cIn, cInLen, cOut, &cOutLen, C.int(level))
	if ret != C.Z_STREAM_END {
		return nil, fmt.Errorf("zlib deflate failed with %d", ret)
	}

	pos := 10 + int(cOutLen)

	// Footer: CRC32 and ISIZE
	binary.LittleEndian.PutUint32(outBuf[pos:pos+4], crc)
	binary.LittleEndian.PutUint32(outBuf[pos+4:pos+8], uint32(len(blk.data)))
	pos += 8

	finalBuf := outBuf[:pos]

	atomic.AddInt64(&gBytesIn, int64(len(blk.data)))
	atomic.AddInt64(&gBytesOut, int64(len(finalBuf)))
	atomic.AddInt32(&gBlocksDone, 1)

	return &CompressedResult{
		seq:         blk.seq,
		data:        finalBuf,
		crc:         crc,
		originalLen: len(blk.data),
		isLast:      blk.isLast,
	}, nil
}

func compressFile(inPath, outPath string, level int) error {
	inFile, err := os.Open(inPath)
	if err != nil {
		return err
	}
	defer inFile.Close()

	outFile, err := os.Create(outPath)
	if err != nil {
		return err
	}
	defer outFile.Close()

	results := make(chan chan *CompressedResult, ChanCap)

	var wg sync.WaitGroup
	wg.Add(1)

	// Consumer
	go func() {
		defer wg.Done()
		for resChan := range results {
			res := <-resChan
			if res != nil {
				outFile.Write(res.data)
			}
		}
	}()

	// Producer
	seq := int64(0)
	for {
		buf := make([]byte, BlockSize)
		n, err := inFile.Read(buf)
		if n > 0 {
			data := buf[:n]
			isLast := (n < BlockSize)
			blk := &Block{
				seq:    seq,
				data:   data,
				isLast: isLast,
			}
			seq++

			resChan := make(chan *CompressedResult, 1)
			results <- resChan

			go func(b *Block, rc chan *CompressedResult) {
				res, err := compressBlockCGO(b, level)
				if err != nil {
					fmt.Fprintf(os.Stderr, "Error compressing block %d: %v\n", b.seq, err)
					rc <- nil
				} else {
					rc <- res
				}
			}(blk, resChan)

			if isLast {
				break
			}
		}
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
	}

	close(results)
	wg.Wait()
	return outFile.Sync()
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr, "Usage: %s <file>\n", os.Args[0])
		os.Exit(1)
	}

	inPath := os.Args[1]
	outPath := inPath + ".gz"

	err := compressFile(inPath, outPath, 6)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Compressed %s -> %s\n", inPath, outPath)
	fmt.Printf("Stats: %d bytes in, %d bytes out, %d blocks\n",
		atomic.LoadInt64(&gBytesIn),
		atomic.LoadInt64(&gBytesOut),
		atomic.LoadInt32(&gBlocksDone))
}
