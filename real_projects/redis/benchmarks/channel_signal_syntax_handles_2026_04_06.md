# Channel Reply Benchmark After Syntax Cleanup 2026-04-06

- Requests per case: `200000`
- Clients: `50`
- Runs per case: `5`
- Idiomatic port: `6411`
- Upstream port: `6410`

## Summary vs Upstream

| Case | Idiomatic mean rps | Idiomatic median rps | Upstream mean rps | Upstream median rps | Idiomatic/upstream mean | Idiomatic mean p50 | Upstream mean p50 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `SET P=1` | 202127.86 | 202839.75 | 200518.60 | 202020.20 | 1.008x | 0.133 | 0.130 |
| `GET P=1` | 186872.98 | 198019.80 | 203318.95 | 203665.98 | 0.919x | 0.140 | 0.129 |
| `SET P=16` | 2453904.10 | 2500000.00 | 2204886.18 | 2222222.25 | 1.113x | 0.263 | 0.297 |
| `GET P=16` | 2713750.70 | 2739726.00 | 2623037.30 | 2631579.00 | 1.035x | 0.217 | 0.239 |
| `INCR P=16` | 2072670.17 | 2325581.25 | 2504064.90 | 2564102.50 | 0.828x | 0.330 | 0.255 |

## Raw Output

## Run 1
### redis_idiomatic
- `SET P=1`: `SET: 204290.09 requests per second, p50=0.127 msec`
- `GET P=1`: `GET: 213675.22 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2500000.00 requests per second, p50=0.263 msec`
- `GET P=16`: `GET: 2816901.25 requests per second, p50=0.199 msec`
- `INCR P=16`: `INCR: 2352941.25 requests per second, p50=0.231 msec`

### upstream
- `SET P=1`: `SET: 206825.23 requests per second, p50=0.127 msec`
- `GET P=1`: `GET: 212539.86 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2298850.75 requests per second, p50=0.287 msec`
- `GET P=16`: `GET: 2777778.00 requests per second, p50=0.223 msec`
- `INCR P=16`: `INCR: 2597402.50 requests per second, p50=0.239 msec`

## Run 2
### redis_idiomatic
- `SET P=1`: `SET: 209205.03 requests per second, p50=0.127 msec`
- `GET P=1`: `GET: 131578.95 requests per second, p50=0.159 msec`
- `SET P=16`: `SET: 2531645.50 requests per second, p50=0.247 msec`
- `GET P=16`: `GET: 2777778.00 requests per second, p50=0.215 msec`
- `INCR P=16`: `INCR: 2380952.50 requests per second, p50=0.287 msec`

### upstream
- `SET P=1`: `SET: 206185.56 requests per second, p50=0.127 msec`
- `GET P=1`: `GET: 204498.98 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2222222.25 requests per second, p50=0.295 msec`
- `GET P=16`: `GET: 2702702.75 requests per second, p50=0.239 msec`
- `INCR P=16`: `INCR: 2564102.50 requests per second, p50=0.255 msec`

## Run 3
### redis_idiomatic
- `SET P=1`: `SET: 202839.75 requests per second, p50=0.135 msec`
- `GET P=1`: `GET: 198019.80 requests per second, p50=0.135 msec`
- `SET P=16`: `SET: 2500000.00 requests per second, p50=0.231 msec`
- `GET P=16`: `GET: 2739726.00 requests per second, p50=0.215 msec`
- `INCR P=16`: `INCR: 2325581.25 requests per second, p50=0.311 msec`

### upstream
- `SET P=1`: `SET: 202020.20 requests per second, p50=0.127 msec`
- `GET P=1`: `GET: 202839.75 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2222222.25 requests per second, p50=0.303 msec`
- `GET P=16`: `GET: 2631579.00 requests per second, p50=0.239 msec`
- `INCR P=16`: `INCR: 2564102.50 requests per second, p50=0.255 msec`

## Run 4
### redis_idiomatic
- `SET P=1`: `SET: 198609.73 requests per second, p50=0.135 msec`
- `GET P=1`: `GET: 198412.69 requests per second, p50=0.135 msec`
- `SET P=16`: `SET: 2439024.25 requests per second, p50=0.295 msec`
- `GET P=16`: `GET: 2702702.75 requests per second, p50=0.223 msec`
- `INCR P=16`: `INCR: 2298850.75 requests per second, p50=0.255 msec`

### upstream
- `SET P=1`: `SET: 197628.47 requests per second, p50=0.135 msec`
- `GET P=1`: `GET: 203665.98 requests per second, p50=0.127 msec`
- `SET P=16`: `SET: 2197802.25 requests per second, p50=0.295 msec`
- `GET P=16`: `GET: 2564102.50 requests per second, p50=0.247 msec`
- `INCR P=16`: `INCR: 2469135.75 requests per second, p50=0.263 msec`

## Run 5
### redis_idiomatic
- `SET P=1`: `SET: 195694.72 requests per second, p50=0.143 msec`
- `GET P=1`: `GET: 192678.23 requests per second, p50=0.143 msec`
- `SET P=16`: `SET: 2298850.75 requests per second, p50=0.279 msec`
- `GET P=16`: `GET: 2531645.50 requests per second, p50=0.231 msec`
- `INCR P=16`: `INCR: 1005025.12 requests per second, p50=0.567 msec`

### upstream
- `SET P=1`: `SET: 189933.53 requests per second, p50=0.135 msec`
- `GET P=1`: `GET: 193050.19 requests per second, p50=0.135 msec`
- `SET P=16`: `SET: 2083333.38 requests per second, p50=0.303 msec`
- `GET P=16`: `GET: 2439024.25 requests per second, p50=0.247 msec`
- `INCR P=16`: `INCR: 2325581.25 requests per second, p50=0.263 msec`

