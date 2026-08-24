// Compares zlib's one-shot compress2()/uncompress() -- what packet.c uses --
// against a persistent z_stream reset per packet. Payload sizes chosen to
// bracket real sm64coopdx packets (PACKET_LENGTH is 3000, most are far less).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    const int sizes[] = { 64, 128, 256, 512, 1024, 3000 };
    const int N = 20000;
    unsigned char src[3000], dst[8192], back[8192];

    // Semi-structured data, like a packet: mostly small ints and repeated fields.
    for (int i = 0; i < 3000; i++) src[i] = (i % 17 == 0) ? (unsigned char)(i * 7) : (unsigned char)(i & 0x0f);

    printf("%6s | %11s %11s %7s | %11s %11s %7s | %6s\n",
           "bytes", "compress2", "persist", "speedup", "uncompress", "persist", "speedup", "ratio");

    for (unsigned s = 0; s < sizeof(sizes)/sizeof(*sizes); s++) {
        int n = sizes[s];
        uLongf clen = sizeof(dst);

        // --- one-shot compress2, as packet_compress() does today ---
        double t0 = now();
        for (int i = 0; i < N; i++) {
            clen = sizeof(dst);
            if (compress2(dst, &clen, src, n, Z_BEST_SPEED) != Z_OK) { puts("compress2 failed"); return 1; }
        }
        double t_one = (now() - t0) / N * 1e6;
        uLong comp_size = clen;

        // --- persistent deflate stream, reset per packet ---
        z_stream zs; memset(&zs, 0, sizeof(zs));
        // windowBits 11 declares a 2KB window in the zlib header; inflate()
        // accepts any window <= its own, so a stock uncompress() (15) still
        // decodes this. memLevel has no wire representation at all.
        if (deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, 11, 4, Z_DEFAULT_STRATEGY) != Z_OK) { puts("init failed"); return 1; }
        uLong persist_size = 0;
        t0 = now();
        for (int i = 0; i < N; i++) {
            deflateReset(&zs);
            zs.next_in = src; zs.avail_in = n;
            zs.next_out = dst; zs.avail_out = sizeof(dst);
            if (deflate(&zs, Z_FINISH) != Z_STREAM_END) { puts("deflate failed"); return 1; }
            persist_size = sizeof(dst) - zs.avail_out;
        }
        double t_persist = (now() - t0) / N * 1e6;
        deflateEnd(&zs);

        // --- one-shot uncompress, as packet_decompress() does today ---
        clen = comp_size;
        compress2(dst, &clen, src, n, Z_BEST_SPEED);
        t0 = now();
        for (int i = 0; i < N; i++) {
            uLongf dlen = sizeof(back);
            if (uncompress(back, &dlen, dst, clen) != Z_OK) { puts("uncompress failed"); return 1; }
        }
        double u_one = (now() - t0) / N * 1e6;

        // --- persistent inflate stream ---
        z_stream is; memset(&is, 0, sizeof(is));
        if (inflateInit(&is) != Z_OK) { puts("inflateInit failed"); return 1; }
        t0 = now();
        for (int i = 0; i < N; i++) {
            inflateReset(&is);
            is.next_in = dst; is.avail_in = clen;
            is.next_out = back; is.avail_out = sizeof(back);
            if (inflate(&is, Z_FINISH) != Z_STREAM_END) { puts("inflate failed"); return 1; }
        }
        double u_persist = (now() - t0) / N * 1e6;
        inflateEnd(&is);

        printf("%6d | %9.2fus %9.2fus %6.1fx | %9.2fus %9.2fus %6.1fx | %3lu/%-3lu\n",
               n, t_one, t_persist, t_one / t_persist,
               u_one, u_persist, u_one / u_persist, comp_size, persist_size);
    }

    // Verify the persistent-stream output is decodable by a stock uncompress().
    z_stream zs; memset(&zs, 0, sizeof(zs));
    deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, 11, 4, Z_DEFAULT_STRATEGY);
    deflateReset(&zs);
    zs.next_in = src; zs.avail_in = 512;
    zs.next_out = dst; zs.avail_out = sizeof(dst);
    deflate(&zs, Z_FINISH);
    uLong produced = sizeof(dst) - zs.avail_out;
    deflateEnd(&zs);
    uLongf dlen = sizeof(back);
    int rc = uncompress(back, &dlen, dst, produced);
    printf("\nwire-compat check: stock uncompress() on windowBits=11 stream -> rc=%d, %lu bytes, bytes match=%s\n",
           rc, dlen, (dlen == 512 && memcmp(back, src, 512) == 0) ? "yes" : "NO");
    return 0;
}
