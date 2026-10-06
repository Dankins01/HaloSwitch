/*
GUEST_SYSCALLS.H

The Linux system call numbers the guest's musl uses (the generic table,
from port/android/guest/libc/arch/arm64_32/bits/syscall.h.in).
*/

#ifndef __HALO_SWITCH_GUEST_SYSCALLS_H
#define __HALO_SWITCH_GUEST_SYSCALLS_H

#define GUEST_SYS_read 63
#define GUEST_SYS_write 64
#define GUEST_SYS_writev 66
#define GUEST_SYS_readv 65
#define GUEST_SYS_pwritev 70
#define GUEST_SYS_preadv 69
#define GUEST_SYS_pread64 67
#define GUEST_SYS_pwrite64 68
#define GUEST_SYS_openat 56
#define GUEST_SYS_close 57
#define GUEST_SYS_lseek 62
#define GUEST_SYS_getdents64 61
#define GUEST_SYS_unlinkat 35
#define GUEST_SYS_renameat 38
#define GUEST_SYS_renameat2 276
#define GUEST_SYS_mkdirat 34
#define GUEST_SYS_fchmod 52
#define GUEST_SYS_fchmodat 53
#define GUEST_SYS_ftruncate 46
#define GUEST_SYS_fsync 82
#define GUEST_SYS_fdatasync 83
#define GUEST_SYS_fcntl 25
#define GUEST_SYS_getcwd 17
#define GUEST_SYS_chdir 49
#define GUEST_SYS_readlinkat 78
#define GUEST_SYS_faccessat 48
#define GUEST_SYS_dup 23
#define GUEST_SYS_dup3 24
#define GUEST_SYS_pipe2 59
#define GUEST_SYS_fstat 80
#define GUEST_SYS_newfstatat 79
#define GUEST_SYS_statx 291
#define GUEST_SYS_statfs 43
#define GUEST_SYS_fstatfs 44
#define GUEST_SYS_getpid 172
#define GUEST_SYS_getppid 173
#define GUEST_SYS_gettid 178
#define GUEST_SYS_getuid 174
#define GUEST_SYS_geteuid 175
#define GUEST_SYS_getgid 176
#define GUEST_SYS_getegid 177
#define GUEST_SYS_sched_yield 124
#define GUEST_SYS_getrandom 278
#define GUEST_SYS_kill 129
#define GUEST_SYS_tkill 130
#define GUEST_SYS_tgkill 131
#define GUEST_SYS_rt_sigprocmask 135
#define GUEST_SYS_rt_sigaction 134
#define GUEST_SYS_sigaltstack 132
#define GUEST_SYS_uname 160
#define GUEST_SYS_prlimit64 261
#define GUEST_SYS_getrlimit 163
#define GUEST_SYS_umask 166
#define GUEST_SYS_flock 32
#define GUEST_SYS_membarrier 283
#define GUEST_SYS_clock_gettime 113
#define GUEST_SYS_clock_getres 114
#define GUEST_SYS_gettimeofday 169
#define GUEST_SYS_nanosleep 101
#define GUEST_SYS_clock_nanosleep 115
#define GUEST_SYS_futex 98
#define GUEST_SYS_ppoll 73
#define GUEST_SYS_utimensat 88
#define GUEST_SYS_mmap 222
#define GUEST_SYS_munmap 215
#define GUEST_SYS_mprotect 226
#define GUEST_SYS_madvise 233
#define GUEST_SYS_mremap 216
#define GUEST_SYS_brk 214
#define GUEST_SYS_exit 93
#define GUEST_SYS_exit_group 94
#define GUEST_SYS_set_tid_address 96
#define GUEST_SYS_ioctl 29
#define GUEST_SYS_clone 220
#define GUEST_SYS_clone3 435
#define GUEST_SYS_execve 221
#define GUEST_SYS_rt_sigtimedwait 137
#define GUEST_SYS_pselect6 72
#define GUEST_SYS_epoll_pwait 22
#define GUEST_SYS_sysinfo 179
#define GUEST_SYS_sendmsg 211
#define GUEST_SYS_recvmsg 212
#define GUEST_SYS_timer_create 107
#define GUEST_SYS_timer_settime 110
#define GUEST_SYS_timer_gettime 108
#define GUEST_SYS_setitimer 103
#define GUEST_SYS_getitimer 102
#define GUEST_SYS_times 153
#define GUEST_SYS_getrusage 165
#define GUEST_SYS_wait4 260
#define GUEST_SYS_waitid 95

#endif
