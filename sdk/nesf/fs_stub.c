// filestream stubs: sync32 carts have no OS filesystem; fceumm gets ROMs
// from memory (FCEUI_LoadGame databuf) and everything file-based (FDS bios,
// external palettes) simply reports absent.
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
typedef struct RFILE RFILE;
RFILE *filestream_open(const char *path, unsigned mode, unsigned hints) { (void)path; (void)mode; (void)hints; return NULL; }
long long filestream_seek(RFILE *s, long long o, int w) { (void)s; (void)o; (void)w; return -1; }
long long filestream_read(RFILE *s, void *d, long long n) { (void)s; (void)d; (void)n; return -1; }
long long filestream_write(RFILE *s, const void *d, long long n) { (void)s; (void)d; (void)n; return -1; }
long long filestream_tell(RFILE *s) { (void)s; return -1; }
long long filestream_get_size(RFILE *s) { (void)s; return -1; }
int filestream_close(RFILE *s) { (void)s; return -1; }
int filestream_eof(RFILE *s) { (void)s; return 1; }
char *filestream_getline(RFILE *s) { (void)s; return NULL; }
int filestream_getc(RFILE *s) { (void)s; return -1; }
void filestream_rewind(RFILE *s) { (void)s; }
int filestream_flush(RFILE *s) { (void)s; return -1; }
int filestream_delete(const char *p) { (void)p; return -1; }
int filestream_exists(const char *p) { (void)p; return 0; }
int path_is_valid(const char *p) { (void)p; return 0; }
