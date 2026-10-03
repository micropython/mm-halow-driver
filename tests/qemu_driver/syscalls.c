/*
 * newlib system calls for the QEMU harness.
 *
 * morselib pulls in parts of newlib that reference these.  nosys.specs would
 * supply stubs too, but they warn at link time; these are quiet, and route
 * stdout/stderr to the QEMU console so any printf is visible.
 */

#include <sys/stat.h>
#include <sys/time.h>

void qemu_putc(char c);
void qemu_exit(int code);

int _write(int fd, const char *buf, int len) {
    if (fd != 1 && fd != 2) {
        return -1;
    }
    for (int i = 0; i < len; ++i) {
        qemu_putc(buf[i]);
    }
    return len;
}

int _read(int fd, char *buf, int len) {
    return -1;
}

int _lseek(int fd, int offset, int whence) {
    return -1;
}

int _close(int fd) {
    return -1;
}

int _fstat(int fd, struct stat *st) {
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int fd) {
    return 1;
}

int _gettimeofday(struct timeval *tv, void *tz) {
    return -1;
}

int _getpid(void) {
    return 1;
}

int _kill(int pid, int sig) {
    qemu_exit(1);
    return -1;
}
