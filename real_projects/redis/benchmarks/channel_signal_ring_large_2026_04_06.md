# Channel Signal Benchmark With Large Ring 2026-04-06

- Requests per case: `200000`
- Clients: `50`
- Idiomatic port: `6393`
- Upstream port: `6394`
- Baseline source: `benchmark_baseline.txt`

## Summary vs Baseline

| Case | Baseline idiomatic rps | New idiomatic rps | Delta | Baseline upstream rps | New upstream rps | New idiomatic/upstream | Baseline idiomatic p50 | New idiomatic p50 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `SET P=1` | 218240.95 | 176127.33 | 0.807x | 211895.10 | 189900.67 | 0.927x | 0.127 | 0.159 |
| `GET P=1` | 220589.90 | 176922.77 | 0.802x | 217867.68 | 191831.62 | 0.922x | 0.127 | 0.159 |
| `SET P=16` | 2873674.42 | 2252351.75 | 0.784x | 2373438.58 | 2063730.38 | 1.091x | 0.226 | 0.284 |
| `GET P=16` | 2958748.58 | 2138008.46 | 0.723x | 2843606.92 | 2433107.50 | 0.879x | 0.151 | 0.271 |
| `INCR P=16` | 1805532.59 | 2034458.17 | 1.127x | 2700274.42 | 2390739.58 | 0.851x | 0.271 | 0.324 |

## Raw Output

## Run 1
### redis_idiomatic
- `SET P=1`: `SET: 159616.92 requests per second, p50=0.159 msec`
- `GET P=1`: `GET: 166112.95 requests per second, p50=0.159 msec`
- `SET P=16`: `SET: 2105263.25 requests per second, p50=0.295 msec`
- `GET P=16`: `GET: 2127659.75 requests per second, p50=0.279 msec`
- `INCR P=16`: `INCR: 2127659.75 requests per second, p50=0.311 msec`

### upstream
- `SET P=1`: `SET: 187265.92 requests per second, p50=0.135 msec`
- `GET P=1`: `GET: 192122.95 requests per second, p50=0.135 msec`
- `SET P=16`: `SET: 2127659.75 requests per second, p50=0.319 msec`
- `GET P=16`: `GET: 2325581.25 requests per second, p50=0.263 msec`
- `INCR P=16`: `INCR: 2352941.25 requests per second, p50=0.271 msec`

## Run 2
### redis_idiomatic
- `SET P=1`: `SET: 187441.42 requests per second, p50=0.159 msec`
- `GET P=1`: `GET: 180831.83 requests per second, p50=0.159 msec`
- `SET P=16`: `SET: 2352941.25 requests per second, p50=0.255 msec`
- `GET P=16`: `GET: 1960784.38 requests per second, p50=0.255 msec`
- `INCR P=16`: `INCR: 2173913.00 requests per second, p50=0.327 msec`

### upstream
- `SET P=1`: `SET: 186741.36 requests per second, p50=0.143 msec`
- `GET P=1`: `GET: 189573.47 requests per second, p50=0.135 msec`
- `SET P=16`: `SET: 1980198.00 requests per second, p50=0.327 msec`
- `GET P=16`: `GET: 2564102.50 requests per second, p50=0.247 msec`
- `INCR P=16`: `INCR: 2409638.75 requests per second, p50=0.271 msec`

## Run 3
### redis_idiomatic
- `SET P=1`: `SET: 181323.66 requests per second, p50=0.159 msec`
- `GET P=1`: `GET: 183823.52 requests per second, p50=0.159 msec`
- `SET P=16`: `SET: 2298850.75 requests per second, p50=0.303 msec`
- `GET P=16`: `GET: 2325581.25 requests per second, p50=0.279 msec`
- `INCR P=16`: `INCR: 1801801.75 requests per second, p50=0.335 msec`

### upstream
- `SET P=1`: `SET: 195694.72 requests per second, p50=0.135 msec`
- `GET P=1`: `GET: 193798.45 requests per second, p50=0.135 msec`
- `SET P=16`: `SET: 2083333.38 requests per second, p50=0.319 msec`
- `GET P=16`: `GET: 2409638.75 requests per second, p50=0.255 msec`
- `INCR P=16`: `INCR: 2409638.75 requests per second, p50=0.271 msec`

