#ifndef ANDROID_SHIM_IOSUPPORT_H
#define ANDROID_SHIM_IOSUPPORT_H
/* newlib's device table, as much as 3ds_log.c needs to catch the game's printf: on Android
 * CtrShim_RedirectStdio (ctr_shim.c) points stdout and stderr at whatever it holds. */
#include <stddef.h>
#include <sys/types.h>

struct _reent { int unused; };
typedef struct
{
    const char *name;
    ssize_t (*write_r)(struct _reent *r, void *fd, const char *ptr, size_t len);
} devoptab_t;

enum { STD_IN = 0, STD_OUT = 1, STD_ERR = 2 };
extern const devoptab_t *devoptab_list[];
/* Sends stdout and stderr through devoptab_list[STD_OUT] / [STD_ERR], once set. */
void CtrShim_RedirectStdio(void);
#endif
