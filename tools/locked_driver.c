/* Harness for a birbc-emitted locked/smol player: stream render() to raw
 * 16-bit mono PCM so pitch_report and the parity check can measure it.
 * The emitted players are libraries with no main of their own. */
#include <stdio.h>
typedef int i32; typedef short i16;

void render(i32 n);
i16 *outPtr(void);
i32 getLength(void);

#define CHUNK 4096

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s out.raw\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
    i32 total = getLength();
    for (i32 done = 0; done < total; ) {
        i32 n = total - done < CHUNK ? total - done : CHUNK;
        render(n);
        fwrite(outPtr(), sizeof(i16), (size_t)n, f);
        done += n;
    }
    fclose(f);
    return 0;
}
