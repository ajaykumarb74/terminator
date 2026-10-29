/* Framed message transport on pipes: [type:1][len:4][payload]. */
#include "terminator.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int ipc_write_all(int fd, const void *d, size_t n)
{
    const char *p = d;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pf = { fd, POLLOUT, 0 };
                poll(&pf, 1, -1);
                continue;
            }
            return -1;
        }
        if (k == 0) return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

int ipc_send(int fd, char type, const void *d, uint32_t n)
{
    unsigned char hdr[5];
    hdr[0] = (unsigned char)type;
    memcpy(hdr + 1, &n, 4);
    if (n <= 4096 - 5) {
        unsigned char buf[4096];
        memcpy(buf, hdr, 5);
        if (n) memcpy(buf + 5, d, n);
        return ipc_write_all(fd, buf, 5 + (size_t)n);
    }
    if (ipc_write_all(fd, hdr, 5)) return -1;
    return ipc_write_all(fd, d, n);
}

int ipc_fill(int fd, IpcReader *r, size_t maxbytes)
{
    size_t total = 0;
    if (r->off && r->off == r->len) r->off = r->len = 0;
    while (total < maxbytes) {
        if (r->cap - r->len < 4096) {
            if (r->off) {
                memmove(r->buf, r->buf + r->off, r->len - r->off);
                r->len -= r->off;
                r->off = 0;
            }
            if (r->cap - r->len < 4096) {
                size_t nc = r->cap ? r->cap * 2 : 16384;
                unsigned char *nb = realloc(r->buf, nc);
                if (!nb) return -1;
                r->buf = nb;
                r->cap = nc;
            }
        }
        ssize_t k = read(fd, r->buf + r->len, r->cap - r->len);
        if (k > 0) { r->len += (size_t)k; total += (size_t)k; continue; }
        if (k == 0) return total ? (int)total : -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return total ? (int)total : -1;
    }
    return (int)total;
}

int ipc_pop(IpcReader *r, char *type, void *dst, size_t dstsz, uint32_t *plen)
{
    if (r->len - r->off < 5) return 0;
    uint32_t n;
    memcpy(&n, r->buf + r->off + 1, 4);
    if (r->len - r->off < 5 + (size_t)n) return 0;
    *type = (char)r->buf[r->off];
    size_t c = n < dstsz ? n : dstsz;
    if (c) memcpy(dst, r->buf + r->off + 5, c);
    *plen = (uint32_t)c;
    r->off += 5 + (size_t)n;
    if (r->off == r->len) r->off = r->len = 0;
    return 1;
}

void ipc_free(IpcReader *r)
{
    free(r->buf);
    memset(r, 0, sizeof *r);
}
