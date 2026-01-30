#include <zlib.h>
#include <stdio.h>
int main() {
    const char *data = "Zlib format test\n";
    printf("%lu\n", adler32(0L, (const Bytef*)data, 17));
    return 0;
}
