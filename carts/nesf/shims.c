// newlib syscall shims for the fceumm cart: heap over a static arena,
// no filesystem (ROMs come from memory via the sync32 disk API).
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>

static uint8_t arena[94 * 1024] __attribute__((aligned(8)));
static uint8_t *brk = arena;

void *_sbrk(ptrdiff_t incr) {
    if (brk + incr > arena + sizeof arena) { errno = ENOMEM; return (void *)-1; }
    uint8_t *p = brk;
    brk += incr;
    return p;
}

int _open(const char *p, int f, int m) { (void)p; (void)f; (void)m; errno = ENOENT; return -1; }
int _close(int fd) { (void)fd; return -1; }
int _read(int fd, void *b, size_t n) { (void)fd; (void)b; (void)n; return -1; }
int _write(int fd, const void *b, size_t n) { (void)fd; (void)b; return (int)n; }
long _lseek(int fd, long o, int w) { (void)fd; (void)o; (void)w; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }
void _exit(int c) { (void)c; for (;;) ; }
int _gettimeofday(void *tv, void *tz) { (void)tv; (void)tz; return -1; }
