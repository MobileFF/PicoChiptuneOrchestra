// Minimal stand-in for FatFs's ff.h, backed by stdio, covering exactly the
// subset master/src/vgm_player.c and vgz_inflate.c use. Host-test only.
#pragma once
#include <stdio.h>
#include <stdint.h>

typedef unsigned int UINT;
typedef uint32_t FSIZE_t;
typedef enum { FR_OK = 0, FR_DISK_ERR = 1 } FRESULT;
typedef struct { FILE *fp; } FIL;

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_ALWAYS 0x08

static inline FRESULT f_open(FIL *f, const char *path, unsigned char mode) {
    const char *m = (mode & FA_WRITE) ? "wb" : "rb";
    f->fp = fopen(path, m);
    return f->fp ? FR_OK : FR_DISK_ERR;
}
static inline FRESULT f_read(FIL *f, void *buf, UINT btr, UINT *br) {
    *br = (UINT)fread(buf, 1, btr, f->fp);
    return FR_OK;
}
static inline FRESULT f_write(FIL *f, const void *buf, UINT btw, UINT *bw) {
    *bw = (UINT)fwrite(buf, 1, btw, f->fp);
    return FR_OK;
}
static inline FRESULT f_lseek(FIL *f, FSIZE_t pos) {
    return fseek(f->fp, (long)pos, SEEK_SET) == 0 ? FR_OK : FR_DISK_ERR;
}
static inline FSIZE_t f_tell(FIL *f) { return (FSIZE_t)ftell(f->fp); }
static inline FRESULT f_close(FIL *f) { fclose(f->fp); return FR_OK; }

// Directory iteration -- player_config.c's autoload uses it to find a
// "vgmplay*.ini". Stubbed to "empty directory" for host tests (they call
// player_config_apply directly, never the SD-scanning path).
#define AM_DIR 0x10
typedef struct { int _unused; } DIR;
typedef struct { char fname[256]; unsigned char fattrib; } FILINFO;
static inline FRESULT f_findfirst(DIR *d, FILINFO *fno, const char *path, const char *pat) {
    (void)d; (void)path; (void)pat; fno->fname[0] = '\0'; fno->fattrib = 0; return FR_OK;
}
static inline FRESULT f_findnext(DIR *d, FILINFO *fno) { (void)d; fno->fname[0] = '\0'; return FR_OK; }
static inline FRESULT f_closedir(DIR *d) { (void)d; return FR_OK; }
