/*
 * The 3DS's two file systems, as paths: "romfs:/" (the engine's own files) and
 * "sdmc:/3ds/emerald3ds/" (the data pack, saves, settings, logs). Many units write them as
 * literals, game units among them, so they are translated where the C library is entered:
 * linked with --wrap on the calls below, no unit changes.
 *
 *   romfs:/<f>                 -> <internal files>/romfs/<f>   (extracted from the APK)
 *   sdmc:/3ds/emerald3ds/<f>   -> <external files>/<f>
 *   sdmc:/3ds, sdmc:/3ds/emerald3ds                              -> <external files>
 *   sdmc:/3ds/pokeemerald/<f>  -> <external files>/pokeemerald/<f>   (the old save's home)
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "android_host.h"

#define PATH_MAX_LEN 1024

static const char *Translate(const char *path, char *out, size_t size)
{
    static const char romfs[] = "romfs:/";
    static const char app[] = "sdmc:/3ds/emerald3ds";
    static const char old[] = "sdmc:/3ds/pokeemerald/";
    int length;

    if (path == NULL)
        return NULL;
    if (strncmp(path, romfs, sizeof(romfs) - 1) == 0)
        length = snprintf(out, size, "%sromfs/%s", AndroidHost_InternalDir(), path + sizeof(romfs) - 1);
    else if (strncmp(path, app, sizeof(app) - 1) == 0 && (path[sizeof(app) - 1] == '/' || path[sizeof(app) - 1] == 0))
        length = snprintf(out, size, "%s%s", AndroidHost_ExternalDir(),
                          path[sizeof(app) - 1] == '/' ? path + sizeof(app) : "");
    else if (strcmp(path, "sdmc:/3ds") == 0)
        length = snprintf(out, size, "%s", AndroidHost_ExternalDir());
    else if (strncmp(path, old, sizeof(old) - 1) == 0)
        length = snprintf(out, size, "%spokeemerald/%s", AndroidHost_ExternalDir(), path + sizeof(old) - 1);
    else
        return path;
    return length > 0 && (size_t)length < size ? out : NULL;
}

FILE *__real_fopen(const char *path, const char *mode);
int __real_rename(const char *from, const char *to);
int __real_remove(const char *path);
int __real_mkdir(const char *path, mode_t mode);
int __real_stat(const char *path, struct stat *info);

FILE *__wrap_fopen(const char *path, const char *mode)
{
    char buffer[PATH_MAX_LEN];
    const char *real = Translate(path, buffer, sizeof(buffer));

    return real != NULL ? __real_fopen(real, mode) : NULL;
}

int __wrap_rename(const char *from, const char *to)
{
    char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
    const char *realFrom = Translate(from, a, sizeof(a)), *realTo = Translate(to, b, sizeof(b));

    return realFrom != NULL && realTo != NULL ? __real_rename(realFrom, realTo) : -1;
}

int __wrap_remove(const char *path)
{
    char buffer[PATH_MAX_LEN];
    const char *real = Translate(path, buffer, sizeof(buffer));

    return real != NULL ? __real_remove(real) : -1;
}

int __wrap_mkdir(const char *path, mode_t mode)
{
    char buffer[PATH_MAX_LEN];
    const char *real = Translate(path, buffer, sizeof(buffer));

    return real != NULL ? __real_mkdir(real, mode) : -1;
}

int __wrap_stat(const char *path, struct stat *info)
{
    char buffer[PATH_MAX_LEN];
    const char *real = Translate(path, buffer, sizeof(buffer));

    return real != NULL ? __real_stat(real, info) : -1;
}
