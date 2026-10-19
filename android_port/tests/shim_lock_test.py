"""Exercise the real shim with libctru's static initializer and contention."""
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
    args = parser.parse_args()
    source = (ROOT / 'android_port/src/ctr_shim.c').read_text()
    harness = '''#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
typedef int32_t s32;
typedef s32 LightLock;
'''
    for name in ('Init', 'Lock', 'Unlock'):
        harness += function(source, 'void LightLock_' + name + '(')
    harness += '''
static LightLock lock = 1;
static unsigned count;
static void *worker(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 50000; ++i) {
        LightLock_Lock(&lock); ++count; LightLock_Unlock(&lock);
    }
    return NULL;
}
int main(void) {
    LightLock_Lock(&lock); LightLock_Unlock(&lock);
    LightLock dynamic; LightLock_Init(&dynamic);
    assert(dynamic == 1); LightLock_Lock(&dynamic); LightLock_Unlock(&dynamic);
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i) assert(pthread_create(&threads[i], NULL, worker, NULL) == 0);
    for (int i = 0; i < 2; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(count == 100000 && lock == 1);
    puts("PASS shim locks: static/init compatibility, repeated use and contention");
}
'''
    with tempfile.TemporaryDirectory() as folder:
        src = Path(folder) / 'test.c'
        exe = Path(folder) / 'test.exe'
        src.write_text(harness)
        subprocess.run([args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == '__main__':
    main()
