"""Exercise the actual Android path adapter, save writer and selected flash driver."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--tree', type=Path, default=ROOT / 'build/upstream')
    args = parser.parse_args()
    source = (ROOT / '3ds_port/src/3ds_compat.c').read_text(encoding='utf-8')
    dummy = (args.tree / 'src/agb_flash_dummy.c').read_text(encoding='utf-8')
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#define fsync _commit
static int real_mkdir(const char *path, mode_t mode) {(void)mode; return _mkdir(path);}
#else
static int real_mkdir(const char *path, mode_t mode) {return mkdir(path, mode);}
#endif
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define PORTABLE 1
#define PORT_BRIDGE 1
#define PLATFORM_ANDROID 1
#include "gba/flash_internal.h"
unsigned char FLASH_BASE[131072];
u16 (*ProgramFlashByte)(u16, u32, u8);
u16 (*ProgramFlashSector)(u16, u8 *);
u16 (*EraseFlashChip)(void);
u16 (*EraseFlashSector)(u16);
u16 (*WaitForFlashWrite)(u8, u8 *, u8);
const struct FlashType *gFlash;
const u16 *gFlashMaxTime;
static const char *dataDir;
const char *AndroidHost_ExternalDir(void) { return dataDir; }
const char *AndroidHost_InternalDir(void) { return dataDir; }
#define __real_fopen fopen
#define __real_rename rename
#define __real_remove remove
#define __real_mkdir real_mkdir
#define __real_stat stat
'''
    harness += (ROOT / 'android_port/src/android_paths.c').read_text(encoding='utf-8')
    harness += r'''
#define fopen __wrap_fopen
#define rename __wrap_rename
#define CTR_DATA_DIR "sdmc:/3ds/emerald3ds/"
#define CTR_LOG_FS 0
#define CTR_LOG_ERROR 1
static void CtrLog_Write(int level, const char *fmt, ...) {(void)level; (void)fmt;}
static const char *CtrScenario_SavePath(void) {return NULL;}
static bool CtrScenario_SaveWrites(void) {return true;}
u16 Port_WriteFlash(u32 offset, const void *data, u32 size);
'''
    harness += source[source.index('#define CTR_SAVE_PATH'):source.index('/* ── Entry points')]
    for signature in ('u16 WaitForFlashWrite_DUMMY(', 'u16 EraseFlashChip_DUMMY(',
                      'u16 EraseFlashSector_DUMMY(', 'u16 ProgramFlashByte_DUMMY(',
                      'u16 ProgramFlashSector_DUMMY('):
        harness += function(dummy, signature)
    for signature in ('static u32 CompareSaveBytes(', 'u32 VerifyFlashSectorNBytes(', 'u32 VerifyFlashSector('):
        harness += function(source, signature)
    harness += r'''
int main(int argc, char **argv) {
    assert(argc == 2); dataDir = argv[1];
    Port_SaveInit(); assert(Port_SaveIsAvailable());
    assert(__wrap_IdentifyFlash() == 0);
    assert(gFlash->sector.count == 32 && gFlash->sector.size == 4096);
    unsigned char sector[4096], expected[131072];
    // Every sector, including the upper bank. No adjacent sector may change.
    for (unsigned i = 0; i < 32; ++i) {
        memset(sector, i + 1, sizeof(sector));
        assert(ProgramFlashSector(i, sector) == 0);
        assert(VerifyFlashSector(i, sector) == 0);
        memcpy(expected + i * 4096, sector, sizeof(sector));
    }
    // Emerald's signature byte is committed separately after the sector.
    assert(ProgramFlashByte(17, 4088, 0x25) == 0);
    expected[17 * 4096 + 4088] = 0x25;
    memset(FLASH_BASE, 0, sizeof(FLASH_BASE));
    Port_SaveInit(); assert(Port_SaveIsAvailable());
    assert(memcmp(FLASH_BASE, expected, sizeof(expected)) == 0);
    // A mismatch must fail real verification, rather than report fake success.
    memset(sector, 18, sizeof(sector));
    assert(VerifyFlashSector(17, sector) != 0);
    assert(ProgramFlashByte(32, 0, 1) != 0);
    assert(ProgramFlashByte(0, 4096, 1) != 0);
    assert(ProgramFlashSector(32, sector) != 0);
    assert(memcmp(FLASH_BASE, expected, sizeof(expected)) == 0);
    // Keep emulator RTC trailers and the other rotating slot intact.
    FILE *f = fopen(CTR_SAVE_PATH, "ab"); assert(f);
    assert(fwrite("clock-footer-123", 1, 16, f) == 16); assert(fclose(f) == 0);
    Port_SaveInit(); assert(Port_SaveIsAvailable());
    assert(ProgramFlashByte(0, 0, 0xAA) == 0); expected[0] = 0xAA;
    f = fopen(CTR_SAVE_PATH, "rb"); assert(f);
    unsigned char disk[131088]; assert(fread(disk, 1, sizeof(disk), f) == sizeof(disk));
    assert(fclose(f) == 0);
    assert(memcmp(disk, expected, sizeof(expected)) == 0);
    assert(memcmp(disk + sizeof(expected), "clock-footer-123", 16) == 0);
    // A refused foreign save must remain untouched.
    f = fopen(CTR_SAVE_PATH, "ab"); assert(f);
    memset(sector, 0xEF, sizeof(sector)); assert(fwrite(sector, 1, sizeof(sector), f) == sizeof(sector));
    assert(fclose(f) == 0); Port_SaveInit();
    assert(!Port_SaveIsAvailable() && __wrap_IdentifyFlash() != 0);
    assert(Port_WriteFlash(0, sector, sizeof(sector)) != 0);
    puts("PASS Android save: driver, paths, sectors, signature commit, reload, trailers, bounds and refusal");
}
'''
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder)
        src, exe = path / 'save.c', path / 'save.exe'
        src.write_text(harness, encoding='utf-8')
        subprocess.run([args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-pointer-to-int-cast',
                        '-iquote', str(args.tree / 'include'), '-iquote', str(ROOT / 'android_port/include'),
                        str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe), path.as_posix() + '/'], check=True, timeout=15)


if __name__ == '__main__':
    main()
