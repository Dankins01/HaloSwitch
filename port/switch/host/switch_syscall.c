/*
SWITCH_SYSCALL.C

Linux system calls on behalf of the guest's musl runtime (its
syscall_arch.h sends every call to host_syscall), carried out with newlib,
libnx and the kernel's address arbiter. On Android these pass to the Linux
kernel (port/android/host/host_syscall.c); here each is emulated:

- files: newlib's descriptors, which are also the ones the platform
  layer's posix_* helpers use (switch_files.c), so a descriptor the guest
  opened works with both. Linux's open flags, struct stat and directory
  entries are translated. Directories are opened as streams under
  descriptors of their own (from DIRECTORY_FD_BASE), for getdents64;
- futexes: svcWaitForAddress and svcSignalToAddress, which compare and wait
  on a 32-bit word as the Linux futex does;
- time: the system tick (monotonic) and newlib's clock (real time);
- memory: switch_memory.c;
- the standard output and error streams: the log.

Errors go back as -errno with Linux's numbers, which differ from newlib's
above 34, and host_errno does the same for the host functions the guest
calls (switch_files.c, switch_net.c).
*/

#include "switch_host.h"
#include "guest_syscalls.h"

#include <switch.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------- Linux values */

#define LINUX_EPERM 1
#define LINUX_ENOENT 2
#define LINUX_EINTR 4
#define LINUX_EBADF 9
#define LINUX_EAGAIN 11
#define LINUX_ENOMEM 12
#define LINUX_EFAULT 14
#define LINUX_EEXIST 17
#define LINUX_ENOTDIR 20
#define LINUX_EISDIR 21
#define LINUX_EINVAL 22
#define LINUX_ENOTTY 25
#define LINUX_ENAMETOOLONG 36
#define LINUX_ENOSYS 38
#define LINUX_ENOTEMPTY 39
#define LINUX_ELOOP 40
#define LINUX_ETIMEDOUT 110

#define LINUX_O_CREAT 0100
#define LINUX_O_EXCL 0200
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000
#define LINUX_O_DIRECTORY 040000

#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EMPTY_PATH 0x1000

#define LINUX_F_GETFD 1
#define LINUX_F_SETFD 2
#define LINUX_F_GETFL 3
#define LINUX_F_SETFL 4

#define LINUX_S_IFDIR 0040000
#define LINUX_S_IFREG 0100000

#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_REQUEUE 3
#define LINUX_FUTEX_CMP_REQUEUE 4
#define LINUX_FUTEX_WAIT_BITSET 9
#define LINUX_FUTEX_WAKE_BITSET 10
#define LINUX_FUTEX_PRIVATE_FLAG 128
#define LINUX_FUTEX_CLOCK_REALTIME 256

#define LINUX_CLOCK_REALTIME 0
#define LINUX_CLOCK_REALTIME_COARSE 5
#define LINUX_TIMER_ABSTIME 1

#define LINUX_SIGABRT 6
#define LINUX_SIGKILL 9
#define LINUX_SIGSEGV 11

#define LINUX_DT_UNKNOWN 0
#define LINUX_DT_DIR 4
#define LINUX_DT_REG 8

/* newlib's errno as Linux's */
static int linux_errno(int error)
{
	switch (error)
	{
	case 0: return 0;
#ifdef EDEADLK
	case EDEADLK: return 35;
#endif
	case ENAMETOOLONG: return LINUX_ENAMETOOLONG;
#ifdef ENOLCK
	case ENOLCK: return 37;
#endif
	case ENOSYS: return LINUX_ENOSYS;
	case ENOTEMPTY: return LINUX_ENOTEMPTY;
#ifdef ELOOP
	case ELOOP: return LINUX_ELOOP;
#endif
#ifdef EOVERFLOW
	case EOVERFLOW: return 75;
#endif
#ifdef EILSEQ
	case EILSEQ: return 84;
#endif
	case ENOTSOCK: return 88;
	case EDESTADDRREQ: return 89;
	case EMSGSIZE: return 90;
	case EPROTOTYPE: return 91;
	case ENOPROTOOPT: return 92;
	case EPROTONOSUPPORT: return 93;
	case EOPNOTSUPP: return 95;
	case EAFNOSUPPORT: return 97;
	case EADDRINUSE: return 98;
	case EADDRNOTAVAIL: return 99;
	case ENETDOWN: return 100;
	case ENETUNREACH: return 101;
	case ENETRESET: return 102;
	case ECONNABORTED: return 103;
	case ECONNRESET: return 104;
	case ENOBUFS: return 105;
	case EISCONN: return 106;
	case ENOTCONN: return 107;
	case ETIMEDOUT: return LINUX_ETIMEDOUT;
	case ECONNREFUSED: return 111;
	case EHOSTUNREACH: return 113;
	case EALREADY: return 114;
	case EINPROGRESS: return 115;
#ifdef ECANCELED
	case ECANCELED: return 125;
#endif
	default:
		/* the classic values (EPERM to ERANGE) are the same */
		return error <= 34 ? error : LINUX_EINVAL;
	}
}

int host_errno(void)
{
	return linux_errno(errno);
}

static long result_of(long value)
{
	return value < 0 ? -linux_errno(errno) : value;
}

/* ---------- the guest's structures */

struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};

/* the AArch64 kernel's struct stat (port/android/guest/libc/arch/arm64_32/kstat.h) */
struct guest_kstat
{
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t pad;
	int64_t st_size;
	int32_t st_blksize;
	int32_t pad2;
	int64_t st_blocks;
	int64_t st_atime_sec;
	int64_t st_atime_nsec;
	int64_t st_mtime_sec;
	int64_t st_mtime_nsec;
	int64_t st_ctime_sec;
	int64_t st_ctime_nsec;
	uint32_t unused[2];
};

/* struct utsname: six fields of 65 characters */
struct guest_utsname
{
	char fields[6][65];
};

#define GUEST(type, value) ((type)(uintptr_t)(uint32_t)(value))

static int64_t timespec_nanoseconds(uint64_t address, int *present)
{
	const struct guest_timespec *value = GUEST(const struct guest_timespec *, address);

	*present = address != 0;
	if (!address)
		return 0;
	return (int64_t)value->seconds * 1000000000LL + value->nanoseconds;
}

static void timespec_out(uint64_t address, int64_t nanoseconds)
{
	struct guest_timespec *result = GUEST(struct guest_timespec *, address);

	if (!address)
		return;
	result->seconds = (int32_t)(nanoseconds / 1000000000LL);
	result->nanoseconds = (int32_t)(nanoseconds % 1000000000LL);
}

/* ---------- time */

static int64_t monotonic_nanoseconds(void)
{
	return (int64_t)armTicksToNs(armGetSystemTick());
}

static int64_t realtime_nanoseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_REALTIME, &now) != 0)
		return (int64_t)time(NULL) * 1000000000LL;
	return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static int64_t clock_nanoseconds(int clock)
{
	return clock == LINUX_CLOCK_REALTIME || clock == LINUX_CLOCK_REALTIME_COARSE ?
		realtime_nanoseconds() : monotonic_nanoseconds();
}

static void sleep_nanoseconds(int64_t nanoseconds)
{
	if (nanoseconds > 0)
		svcSleepThread(nanoseconds);
	else
		svcSleepThread(YieldType_ToAnyThread);
}

/* ---------- futexes */

static long guest_futex(uint64_t address, int operation, uint32_t value, uint64_t timeout, uint64_t address2,
	uint32_t value3)
{
	int command = operation & ~(LINUX_FUTEX_PRIVATE_FLAG | LINUX_FUTEX_CLOCK_REALTIME);
	int32_t *word = GUEST(int32_t *, address);
	Result result;

	(void)address2;
	if (address & 3)
		return -LINUX_EINVAL;
	switch (command)
	{
	case LINUX_FUTEX_WAIT:
	case LINUX_FUTEX_WAIT_BITSET:
	{
		int present;
		int64_t nanoseconds = timespec_nanoseconds(timeout, &present);

		/* WAIT_BITSET's timeout is absolute */
		if (present && command == LINUX_FUTEX_WAIT_BITSET)
		{
			nanoseconds -= (operation & LINUX_FUTEX_CLOCK_REALTIME) ? realtime_nanoseconds() : monotonic_nanoseconds();
			if (nanoseconds <= 0)
				return -LINUX_ETIMEDOUT;
		}
		result = svcWaitForAddress(word, ArbitrationType_WaitIfEqual, (s32)value, present ? nanoseconds : -1);
		if (R_SUCCEEDED(result))
			return 0;
		switch (R_DESCRIPTION(result))
		{
		case KernelError_TimedOut: return -LINUX_ETIMEDOUT;
		case KernelError_InvalidState: return -LINUX_EAGAIN;
		default: return -LINUX_EINTR;
		}
	}
	case LINUX_FUTEX_WAKE:
	case LINUX_FUTEX_WAKE_BITSET:
		svcSignalToAddress(word, SignalType_Signal, 0, value > INT_MAX ? -1 : (s32)value);
		return 0;
	case LINUX_FUTEX_CMP_REQUEUE:
		if ((uint32_t)*word != value3)
			return -LINUX_EAGAIN;
		/* fall through */
	case LINUX_FUTEX_REQUEUE:
		/* no requeueing: every waiter wakes, and those that should have
		moved to the second word wait there themselves (musl's condition
		variables, the only user, allow such spurious wake-ups) */
		svcSignalToAddress(word, SignalType_Signal, 0, -1);
		return 0;
	default:
		return -LINUX_ENOSYS;
	}
}

/* ---------- standard output and error */

struct log_stream
{
	char line[1024];
	size_t length;
};

static struct log_stream log_streams[2];
static Mutex log_lock;

static void log_bytes(int fd, const char *bytes, size_t size)
{
	struct log_stream *stream = &log_streams[fd == 2];
	size_t index;

	mutexLock(&log_lock);
	for (index = 0; index < size; index++)
	{
		char c = bytes[index];

		if (c == '\n' || stream->length == sizeof(stream->line) - 1)
		{
			stream->line[stream->length] = 0;
			host_log(fd == 2 ? HOST_LOG_WARN : HOST_LOG_INFO, stream->line);
			stream->length = 0;
			if (c == '\n')
				continue;
		}
		stream->line[stream->length++] = c;
	}
	mutexUnlock(&log_lock);
}

/* ---------- directories as descriptors */

#define DIRECTORY_FD_BASE 0x10000
#define DIRECTORY_COUNT 32

struct directory
{
	DIR *stream;
	char path[PATH_MAX];
	/* an entry read but not returned (no room in the caller's buffer) */
	struct dirent pending;
	int has_pending;
};

static struct directory *directories[DIRECTORY_COUNT];
static Mutex directory_lock;

static struct directory *directory_of(int fd)
{
	if (fd < DIRECTORY_FD_BASE || fd >= DIRECTORY_FD_BASE + DIRECTORY_COUNT)
		return NULL;
	return directories[fd - DIRECTORY_FD_BASE];
}

static long directory_open(const char *path)
{
	struct directory *directory;
	int index;

	directory = calloc(1, sizeof(*directory));
	if (!directory)
		return -LINUX_ENOMEM;
	directory->stream = opendir(path);
	if (!directory->stream)
	{
		long error = -linux_errno(errno);

		free(directory);
		return error;
	}
	snprintf(directory->path, sizeof(directory->path), "%s", path);
	mutexLock(&directory_lock);
	for (index = 0; index < DIRECTORY_COUNT; index++)
	{
		if (!directories[index])
		{
			directories[index] = directory;
			mutexUnlock(&directory_lock);
			return DIRECTORY_FD_BASE + index;
		}
	}
	mutexUnlock(&directory_lock);
	closedir(directory->stream);
	free(directory);
	return -24; /* EMFILE */
}

static long directory_close(int fd)
{
	struct directory *directory;

	mutexLock(&directory_lock);
	directory = directory_of(fd);
	if (directory)
		directories[fd - DIRECTORY_FD_BASE] = NULL;
	mutexUnlock(&directory_lock);
	if (!directory)
		return -LINUX_EBADF;
	closedir(directory->stream);
	free(directory);
	return 0;
}

static int entry_type(const struct dirent *entry)
{
#ifdef DT_DIR
	if (entry->d_type == DT_DIR)
		return LINUX_DT_DIR;
	if (entry->d_type == DT_REG)
		return LINUX_DT_REG;
#else
	(void)entry;
#endif
	return LINUX_DT_UNKNOWN;
}

/* struct linux_dirent64: d_ino, d_off (8 bytes each), d_reclen (2), d_type
(1), then the name, padded to 8 bytes */
static long guest_getdents64(int fd, uint64_t buffer, uint32_t size)
{
	struct directory *directory = directory_of(fd);
	unsigned char *out = GUEST(unsigned char *, buffer);
	uint32_t used = 0;
	static uint64_t offset;

	if (!directory)
		return -LINUX_EBADF;
	for (;;)
	{
		struct dirent entry;
		size_t name_length;
		uint32_t record;

		if (directory->has_pending)
		{
			entry = directory->pending;
			directory->has_pending = 0;
		}
		else
		{
			struct dirent *next = readdir(directory->stream);

			if (!next)
				break;
			entry = *next;
		}
		name_length = strlen(entry.d_name);
		record = (uint32_t)((19 + name_length + 1 + 7) & ~(size_t)7);
		if (used + record > size)
		{
			directory->pending = entry;
			directory->has_pending = 1;
			if (!used)
				return -LINUX_EINVAL;
			break;
		}
		{
			uint64_t inode = used + 1;
			uint64_t next_offset = ++offset;
			uint16_t length16 = (uint16_t)record;

			memset(out + used, 0, record);
			memcpy(out + used, &inode, 8);
			memcpy(out + used + 8, &next_offset, 8);
			memcpy(out + used + 16, &length16, 2);
			out[used + 18] = (unsigned char)entry_type(&entry);
			memcpy(out + used + 19, entry.d_name, name_length + 1);
		}
		used += record;
	}
	return used;
}

/* ---------- paths */

static int resolve_path(int dirfd, const char *path, char *result, size_t size)
{
	struct directory *directory;

	if (!path)
		return -LINUX_EFAULT;
	if (path[0] == '/' || dirfd == LINUX_AT_FDCWD)
	{
		if (strlen(path) + 1 > size)
			return -LINUX_ENAMETOOLONG;
		strcpy(result, path);
		return 0;
	}
	directory = directory_of(dirfd);
	if (!directory)
		return -LINUX_EBADF;
	if ((size_t)snprintf(result, size, "%s/%s", directory->path, path) >= size)
		return -LINUX_ENAMETOOLONG;
	return 0;
}

static void kstat_from(const struct stat *st, struct guest_kstat *out, uint64_t inode)
{
	memset(out, 0, sizeof(*out));
	out->st_dev = 1;
	out->st_ino = inode;
	out->st_mode = (S_ISDIR(st->st_mode) ? LINUX_S_IFDIR | 0755 : LINUX_S_IFREG | 0644);
	out->st_nlink = 1;
	out->st_size = st->st_size;
	out->st_blksize = 4096;
	out->st_blocks = (st->st_size + 511) / 512;
	out->st_atime_sec = st->st_atime;
	out->st_mtime_sec = st->st_mtime;
	out->st_ctime_sec = st->st_ctime;
}

static uint64_t path_inode(const char *path)
{
	uint64_t hash = 1469598103934665603ULL;

	while (*path)
		hash = (hash ^ (unsigned char)*path++) * 1099511628211ULL;
	return hash;
}

static long guest_stat_path(const char *path, uint64_t buffer)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return -linux_errno(errno);
	kstat_from(&st, GUEST(struct guest_kstat *, buffer), path_inode(path));
	return 0;
}

static long guest_fstat(int fd, uint64_t buffer)
{
	struct directory *directory = directory_of(fd);
	struct stat st;

	if (directory)
		return guest_stat_path(directory->path, buffer);
	if (fd >= 0 && fd <= 2)
	{
		memset(&st, 0, sizeof(st));
		st.st_mode = S_IFREG;
		kstat_from(&st, GUEST(struct guest_kstat *, buffer), (uint64_t)fd + 1);
		return 0;
	}
	if (fstat(fd, &st) != 0)
		return -linux_errno(errno);
	kstat_from(&st, GUEST(struct guest_kstat *, buffer), (uint64_t)fd + 0x100000000ULL);
	return 0;
}

static int open_flags(int flags)
{
	int result = flags & 3; /* O_RDONLY, O_WRONLY, O_RDWR agree */

	if (flags & LINUX_O_CREAT)
		result |= O_CREAT;
	if (flags & LINUX_O_EXCL)
		result |= O_EXCL;
	if (flags & LINUX_O_TRUNC)
		result |= O_TRUNC;
	if (flags & LINUX_O_APPEND)
		result |= O_APPEND;
	if (flags & LINUX_O_NONBLOCK)
		result |= O_NONBLOCK;
	return result;
}

static long guest_openat(int dirfd, const char *guest_path, int flags, int mode)
{
	char path[PATH_MAX];
	struct stat st;
	int error = resolve_path(dirfd, guest_path, path, sizeof(path));

	if (error)
		return error;
	/* a directory (read only) becomes a stream for getdents64 */
	if ((flags & LINUX_O_DIRECTORY) || ((flags & 3) == 0 && !(flags & LINUX_O_CREAT) &&
		stat(path, &st) == 0 && S_ISDIR(st.st_mode)))
	{
		if (stat(path, &st) != 0)
			return -linux_errno(errno);
		if (!S_ISDIR(st.st_mode))
			return -LINUX_ENOTDIR;
		return directory_open(path);
	}
	return result_of(open(path, open_flags(flags), mode));
}

/* pread and pwrite: positioned without moving the descriptor's offset */
static Mutex positioned_lock;

static long positioned(int fd, void *buffer, size_t count, int64_t offset, int write_it)
{
	off_t previous;
	ssize_t result;

	mutexLock(&positioned_lock);
	previous = lseek(fd, 0, SEEK_CUR);
	if (previous < 0 || lseek(fd, (off_t)offset, SEEK_SET) < 0)
	{
		mutexUnlock(&positioned_lock);
		return -linux_errno(errno);
	}
	result = write_it ? write(fd, buffer, count) : read(fd, buffer, count);
	if (result < 0)
		result = -linux_errno(errno);
	lseek(fd, previous, SEEK_SET);
	mutexUnlock(&positioned_lock);
	return result;
}

static long guest_vector(int fd, uint64_t vector, int count, int64_t offset, int positional, int write_it)
{
	const struct guest_iovec *items = GUEST(const struct guest_iovec *, vector);
	long total = 0;
	int index;

	if (count < 0 || count > 1024)
		return -LINUX_EINVAL;
	for (index = 0; index < count; index++)
	{
		void *base = GUEST(void *, items[index].base);
		size_t length = items[index].length;
		long done;

		if (!length)
			continue;
		if (write_it && (fd == 1 || fd == 2))
		{
			log_bytes(fd, base, length);
			done = (long)length;
		}
		else if (positional)
			done = positioned(fd, base, length, offset + total, write_it);
		else
			done = write_it ? result_of(write(fd, base, length)) : result_of(read(fd, base, length));
		if (done < 0)
			return total ? total : done;
		total += done;
		if ((size_t)done < length)
			break;
	}
	return total;
}

static long guest_rename(const char *from, const char *to)
{
	if (rename(from, to) == 0)
		return 0;
	/* the SD card's file systems do not replace an existing file; POSIX
	rename does (the game saves to a temporary file and renames it) */
	if (errno == EEXIST || access(to, F_OK) == 0)
	{
		struct stat st;

		if (stat(to, &st) == 0 && !S_ISDIR(st.st_mode) && unlink(to) == 0 && rename(from, to) == 0)
			return 0;
	}
	return -linux_errno(errno);
}

/* ---------- dispatch */

long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f)
{
	switch (number)
	{
	/* files */
	case GUEST_SYS_read:
		if (a == 0)
			return 0;
		return result_of(read((int)a, GUEST(void *, b), (size_t)(uint32_t)c));
	case GUEST_SYS_write:
		if (a == 1 || a == 2)
		{
			log_bytes((int)a, GUEST(const char *, b), (size_t)(uint32_t)c);
			return (uint32_t)c;
		}
		return result_of(write((int)a, GUEST(const void *, b), (size_t)(uint32_t)c));
	case GUEST_SYS_readv:
		return guest_vector((int)a, (uint64_t)b, (int)c, 0, 0, 0);
	case GUEST_SYS_writev:
		return guest_vector((int)a, (uint64_t)b, (int)c, 0, 0, 1);
	case GUEST_SYS_preadv:
		return guest_vector((int)a, (uint64_t)b, (int)c, d, 1, 0);
	case GUEST_SYS_pwritev:
		return guest_vector((int)a, (uint64_t)b, (int)c, d, 1, 1);
	case GUEST_SYS_pread64:
		return positioned((int)a, GUEST(void *, b), (size_t)(uint32_t)c, d, 0);
	case GUEST_SYS_pwrite64:
		return positioned((int)a, GUEST(void *, b), (size_t)(uint32_t)c, d, 1);
	case GUEST_SYS_openat:
		return guest_openat((int)a, GUEST(const char *, b), (int)c, (int)d);
	case GUEST_SYS_close:
		if (directory_of((int)a))
			return directory_close((int)a);
		if (a >= 0 && a <= 2)
			return 0;
		return result_of(close((int)a));
	case GUEST_SYS_lseek:
		return result_of((long)lseek((int)a, (off_t)b, (int)c));
	case GUEST_SYS_getdents64:
		return guest_getdents64((int)a, (uint64_t)b, (uint32_t)c);
	case GUEST_SYS_fstat:
		return guest_fstat((int)a, (uint64_t)b);
	case GUEST_SYS_newfstatat:
	{
		char path[PATH_MAX];
		long error;

		if ((d & LINUX_AT_EMPTY_PATH) && (!b || !*GUEST(const char *, b)))
			return guest_fstat((int)a, (uint64_t)c);
		error = resolve_path((int)a, GUEST(const char *, b), path, sizeof(path));
		return error ? error : guest_stat_path(path, (uint64_t)c);
	}
	case GUEST_SYS_faccessat:
	{
		char path[PATH_MAX];
		struct stat st;
		long error = resolve_path((int)a, GUEST(const char *, b), path, sizeof(path));

		if (error)
			return error;
		return stat(path, &st) == 0 ? 0 : -linux_errno(errno);
	}
	case GUEST_SYS_unlinkat:
	{
		char path[PATH_MAX];
		long error = resolve_path((int)a, GUEST(const char *, b), path, sizeof(path));

		if (error)
			return error;
		return result_of((c & LINUX_AT_REMOVEDIR) ? rmdir(path) : unlink(path));
	}
	case GUEST_SYS_renameat:
	case GUEST_SYS_renameat2:
	{
		char from[PATH_MAX], to[PATH_MAX];
		long error = resolve_path((int)a, GUEST(const char *, b), from, sizeof(from));

		if (!error)
			error = resolve_path((int)c, GUEST(const char *, d), to, sizeof(to));
		return error ? error : guest_rename(from, to);
	}
	case GUEST_SYS_mkdirat:
	{
		char path[PATH_MAX];
		long error = resolve_path((int)a, GUEST(const char *, b), path, sizeof(path));

		if (error)
			return error;
		return result_of(mkdir(path, (mode_t)c));
	}
	case GUEST_SYS_ftruncate:
		return result_of(ftruncate((int)a, (off_t)b));
	case GUEST_SYS_fsync:
	case GUEST_SYS_fdatasync:
		return result_of(fsync((int)a));
	case GUEST_SYS_fcntl:
		switch ((int)b)
		{
		case LINUX_F_GETFD:
		case LINUX_F_SETFD:
			return 0;
		case LINUX_F_GETFL:
			return 2; /* O_RDWR */
		case LINUX_F_SETFL:
			return 0;
		default:
			return -LINUX_EINVAL;
		}
	case GUEST_SYS_getcwd:
	{
		char path[PATH_MAX];
		const char *shown;
		size_t length;

		if (!getcwd(path, sizeof(path)))
			return -linux_errno(errno);
		/* without the device ("sdmc:"), as a POSIX path */
		shown = strchr(path, ':') ? strchr(path, ':') + 1 : path;
		length = strlen(shown) + 1;
		if (length > (size_t)(uint32_t)b)
			return -34; /* ERANGE */
		memcpy(GUEST(char *, a), shown, length);
		return (long)length;
	}
	case GUEST_SYS_chdir:
		return result_of(chdir(GUEST(const char *, a)));
	case GUEST_SYS_readlinkat:
		return -LINUX_EINVAL;
	case GUEST_SYS_utimensat:
	case GUEST_SYS_fchmod:
	case GUEST_SYS_fchmodat:
	case GUEST_SYS_flock:
		return 0;
	case GUEST_SYS_umask:
		return 022;
	case GUEST_SYS_ioctl:
		return -LINUX_ENOTTY;

	/* time */
	case GUEST_SYS_clock_gettime:
		timespec_out((uint64_t)b, clock_nanoseconds((int)a));
		return 0;
	case GUEST_SYS_clock_getres:
		timespec_out((uint64_t)b, 1);
		return 0;
	case GUEST_SYS_gettimeofday:
	{
		struct guest_timespec *result = GUEST(struct guest_timespec *, a);
		int64_t now = realtime_nanoseconds();

		if (result)
		{
			result->seconds = (int32_t)(now / 1000000000LL);
			result->nanoseconds = (int32_t)((now % 1000000000LL) / 1000);
		}
		return 0;
	}
	case GUEST_SYS_nanosleep:
	{
		int present;
		int64_t nanoseconds = timespec_nanoseconds((uint64_t)a, &present);

		if (!present)
			return -LINUX_EFAULT;
		sleep_nanoseconds(nanoseconds);
		return 0;
	}
	case GUEST_SYS_clock_nanosleep:
	{
		int present;
		int64_t nanoseconds = timespec_nanoseconds((uint64_t)c, &present);

		if (!present)
			return -LINUX_EFAULT;
		if (b & LINUX_TIMER_ABSTIME)
			nanoseconds -= clock_nanoseconds((int)a);
		sleep_nanoseconds(nanoseconds);
		return 0;
	}
	case GUEST_SYS_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d, (uint64_t)e, (uint32_t)f);
	case GUEST_SYS_sched_yield:
		svcSleepThread(YieldType_ToAnyThread);
		return 0;
	case GUEST_SYS_ppoll:
	{
		int present;
		int64_t nanoseconds = timespec_nanoseconds((uint64_t)c, &present);
		int milliseconds = present ? (int)((nanoseconds + 999999) / 1000000) : -1;

		if ((uint32_t)b == 0)
		{
			if (present)
				sleep_nanoseconds(nanoseconds);
			return 0;
		}
		return result_of(poll(GUEST(struct pollfd *, a), (nfds_t)(uint32_t)b, milliseconds));
	}

	/* memory */
	case GUEST_SYS_mmap:
		return host_guest_mmap((uint64_t)a, (uint64_t)b, (int)c, (int)d, (int)e, f);
	case GUEST_SYS_munmap:
		return host_guest_munmap((uint64_t)a, (uint64_t)b);
	case GUEST_SYS_mprotect:
		return host_guest_mprotect((uint64_t)a, (uint64_t)b, (int)c);
	case GUEST_SYS_madvise:
		return 0;
	case GUEST_SYS_mremap:
	case GUEST_SYS_brk:
		/* musl then falls back to mmap and copying */
		return -LINUX_ENOMEM;
	case GUEST_SYS_membarrier:
		__sync_synchronize();
		return 0;

	/* the process */
	case GUEST_SYS_exit:
	case GUEST_SYS_exit_group:
		host_exit((int)a);
	case GUEST_SYS_getpid:
	case GUEST_SYS_getppid:
	{
		u64 id = 0;

		if (number == GUEST_SYS_getppid)
			return 1;
		svcGetProcessId(&id, CUR_PROCESS_HANDLE);
		return (long)(id & 0x7fffffff);
	}
	case GUEST_SYS_gettid:
	case GUEST_SYS_set_tid_address:
		return 1000 + host_thread_number();
	case GUEST_SYS_getuid:
	case GUEST_SYS_geteuid:
	case GUEST_SYS_getgid:
	case GUEST_SYS_getegid:
		return 0;
	case GUEST_SYS_getrandom:
		randomGet(GUEST(void *, a), (size_t)(uint32_t)b);
		return (uint32_t)b;
	case GUEST_SYS_kill:
	case GUEST_SYS_tkill:
	case GUEST_SYS_tgkill:
	{
		int signal_number = (int)(number == GUEST_SYS_tgkill ? c : b);

		if (signal_number == LINUX_SIGABRT || signal_number == LINUX_SIGKILL || signal_number == LINUX_SIGSEGV)
		{
			char reason[32];

			snprintf(reason, sizeof(reason), "signal %d", signal_number);
			host_abort(reason);
		}
		return 0;
	}
	case GUEST_SYS_rt_sigprocmask:
	case GUEST_SYS_rt_sigaction:
	case GUEST_SYS_sigaltstack:
		/* no signals here */
		return 0;
	case GUEST_SYS_uname:
	{
		struct guest_utsname *name = GUEST(struct guest_utsname *, a);

		memset(name, 0, sizeof(*name));
		strcpy(name->fields[0], "Horizon");
		strcpy(name->fields[1], "switch");
		strcpy(name->fields[2], "1");
		strcpy(name->fields[3], "1");
		strcpy(name->fields[4], "aarch64");
		return 0;
	}

	case GUEST_SYS_statx:
	case GUEST_SYS_statfs:
	case GUEST_SYS_fstatfs:
	case GUEST_SYS_dup:
	case GUEST_SYS_dup3:
	case GUEST_SYS_pipe2:
	case GUEST_SYS_prlimit64:
	case GUEST_SYS_getrlimit:
	case GUEST_SYS_clone:
	case GUEST_SYS_clone3:
	case GUEST_SYS_execve:
	case GUEST_SYS_rt_sigtimedwait:
	case GUEST_SYS_pselect6:
	case GUEST_SYS_epoll_pwait:
	case GUEST_SYS_sysinfo:
	case GUEST_SYS_sendmsg:
	case GUEST_SYS_recvmsg:
	case GUEST_SYS_timer_create:
	case GUEST_SYS_timer_settime:
	case GUEST_SYS_timer_gettime:
	case GUEST_SYS_setitimer:
	case GUEST_SYS_getitimer:
	case GUEST_SYS_times:
	case GUEST_SYS_getrusage:
	case GUEST_SYS_wait4:
	case GUEST_SYS_waitid:
		return -LINUX_ENOSYS;

	default:
	{
		static uint8_t warned[512];

		if (number >= 0 && number < 512 && !warned[number])
		{
			warned[number] = 1;
			host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		}
		return -LINUX_ENOSYS;
	}
	}
}
