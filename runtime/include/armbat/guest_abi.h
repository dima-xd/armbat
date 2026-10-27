#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace armbat {

// 32-bit ARM `struct stat` (72 bytes). The host struct is 64-bit, so every call
// needs an explicit conversion; copying it wholesale would corrupt both sides.
struct GuestStat {
    std::uint32_t st_dev;
    std::uint32_t st_ino;
    std::uint16_t st_mode;
    std::uint16_t st_nlink;
    std::uint16_t st_uid;
    std::uint16_t st_gid;
    std::uint32_t st_rdev;
    std::uint32_t pad1;
    std::int32_t st_size;
    std::int32_t st_blksize;
    std::int32_t pad2;
    std::int32_t st_blocks;
    // bionic #defines st_atime as st_atim.tv_sec, so the guest fields cannot
    // use the same spelling.
    std::int32_t atime;
    std::uint32_t atime_nsec;
    std::int32_t mtime;
    std::uint32_t mtime_nsec;
    std::int32_t ctime;
    std::uint32_t ctime_nsec;
    std::uint32_t unused4;
    std::uint32_t unused5;
};

static_assert(sizeof(GuestStat) == 72, "ARM EABI struct stat must be 72 bytes");

// 32-bit ARM `struct stat` / `struct stat64` (104 bytes). Since Lollipop bionic
// defines both on the 64-bit-inode body, so every stat family call uses this
// layout. The natural 8-byte alignment of st_size leaves four bytes of padding
// after pad3, exactly as the kernel's copy of the structure does.
struct GuestStat64 {
    std::uint64_t st_dev;
    std::uint32_t pad0;
    std::uint32_t st_ino_old;
    std::uint32_t st_mode;
    std::uint32_t st_nlink;
    std::uint32_t st_uid;
    std::uint32_t st_gid;
    std::uint64_t st_rdev;
    std::uint32_t pad3;
    std::int64_t st_size;
    std::uint32_t st_blksize;
    std::uint64_t st_blocks;
    // bionic's <sys/stat.h> #defines st_atime to st_atim.tv_sec, so the guest
    // fields cannot use that spelling.
    std::int32_t atime;
    std::uint32_t atime_nsec;
    std::int32_t mtime;
    std::uint32_t mtime_nsec;
    std::int32_t ctime;
    std::uint32_t ctime_nsec;
    std::uint64_t st_ino;
};

static_assert(sizeof(GuestStat64) == 104, "ARM bionic struct stat must be 104 bytes");
static_assert(offsetof(GuestStat64, st_size) == 48, "ARM bionic stat st_size offset");

// 32-bit ARM `struct timeval`: 32-bit time_t.
struct GuestTimeval {
    std::int32_t seconds;
    std::int32_t microseconds;
};

static_assert(sizeof(GuestTimeval) == 8, "ARM EABI timeval must be 8 bytes");

// `struct utsname` on every 32-bit Linux: six 65-byte fields.
struct GuestUtsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

static_assert(sizeof(GuestUtsname) == 390, "ARM utsname must be 390 bytes");

// 32-bit ARM `struct rlimit64`.
struct GuestRlimit64 {
    std::uint64_t current;
    std::uint64_t maximum;
};

static_assert(sizeof(GuestRlimit64) == 16, "ARM rlimit64 must be 16 bytes");

// 32-bit ARM `struct rlimit`, the layout the `ugetrlimit` syscall writes:
// a 32-bit rlim_t, so eight bytes in total.
struct GuestRlimit32 {
    std::uint32_t current;
    std::uint32_t maximum;
};

static_assert(sizeof(GuestRlimit32) == 8, "ARM rlimit must be 8 bytes");

// 32-bit ARM timespec: 32-bit time_t, so 8 bytes total.
struct GuestTimespec {
    std::int32_t seconds;
    std::int32_t nanoseconds;
};

static_assert(sizeof(GuestTimespec) == 8, "ARM EABI timespec must be 8 bytes");

// 32-bit ARM `struct itimerspec`: two 32-bit timespecs.
struct GuestItimerspec {
    GuestTimespec interval;
    GuestTimespec value;
};

static_assert(sizeof(GuestItimerspec) == 16, "ARM EABI itimerspec must be 16 bytes");

// 32-bit ARM `struct rusage` (72 bytes): two timevals, then 14 32-bit longs.
struct GuestRusage {
    GuestTimeval utime;
    GuestTimeval stime;
    std::int32_t maxrss;
    std::int32_t ixrss;
    std::int32_t idrss;
    std::int32_t isrss;
    std::int32_t minflt;
    std::int32_t majflt;
    std::int32_t nswap;
    std::int32_t inblock;
    std::int32_t oublock;
    std::int32_t msgsnd;
    std::int32_t msgrcv;
    std::int32_t nsignals;
    std::int32_t nvcsw;
    std::int32_t nivcsw;
};

static_assert(sizeof(GuestRusage) == 72, "ARM EABI rusage must be 72 bytes");

// 32-bit ARM `struct sysinfo` (64 bytes): 32-bit longs, a 16-bit process count,
// and eight bytes of padding the kernel keeps for a not-yet-defined field.
struct GuestSysinfo {
    std::int32_t uptime;
    std::uint32_t loads[3];
    std::uint32_t totalram;
    std::uint32_t freeram;
    std::uint32_t sharedram;
    std::uint32_t bufferram;
    std::uint32_t totalswap;
    std::uint32_t freeswap;
    std::uint16_t procs;
    std::uint16_t pad;
    std::uint32_t totalhigh;
    std::uint32_t freehigh;
    std::uint32_t mem_unit;
    char spare[8];
};

static_assert(sizeof(GuestSysinfo) == 64, "ARM EABI sysinfo must be 64 bytes");

// 32-bit ARM `struct statfs` and `struct statfs64` are the same 88 bytes on
// bionic: the 64-bit counters start after the two 32-bit words and are aligned
// to eight, which is also what the kernel's copy of the structure looks like.
struct GuestStatfs {
    std::uint32_t type;
    std::uint32_t block_size;
    std::uint64_t blocks;
    std::uint64_t blocks_free;
    std::uint64_t blocks_available;
    std::uint64_t files;
    std::uint64_t files_free;
    std::int32_t fsid[2];
    std::uint32_t name_length;
    std::uint32_t fragment_size;
    std::uint32_t flags;
    std::uint32_t spare[4];
};

static_assert(sizeof(GuestStatfs) == 88, "ARM EABI statfs must be 88 bytes");
static_assert(offsetof(GuestStatfs, blocks) == 8, "ARM EABI statfs blocks offset");
static_assert(offsetof(GuestStatfs, fsid) == 48, "ARM EABI statfs fsid offset");

// The two timespecs the guest and the kernel exchange: same fields, different
// widths, so every conversion goes through one place.
inline struct timespec ToHostTimespec(const GuestTimespec& guest) {
    struct timespec host{};
    host.tv_sec = static_cast<time_t>(guest.seconds);
    host.tv_nsec = static_cast<long>(guest.nanoseconds);
    return host;
}

inline GuestTimespec FromHostTimespec(const struct timespec& host) {
    GuestTimespec guest{};
    guest.seconds = static_cast<std::int32_t>(host.tv_sec);
    guest.nanoseconds = static_cast<std::int32_t>(host.tv_nsec);
    return guest;
}

// 32-bit ARM `struct iovec`: the pointer is 32 bits wide, so 8 bytes.
struct GuestIoVec {
    std::uint32_t base;
    std::uint32_t length;
};

static_assert(sizeof(GuestIoVec) == 8, "ARM EABI iovec must be 8 bytes");

// 32-bit ARM `struct tm` (44 bytes): nine ints, then a 32-bit gmtoff and pointer.
struct GuestTm {
    std::int32_t tm_sec;
    std::int32_t tm_min;
    std::int32_t tm_hour;
    std::int32_t tm_mday;
    std::int32_t tm_mon;
    std::int32_t tm_year;
    std::int32_t tm_wday;
    std::int32_t tm_yday;
    std::int32_t tm_isdst;
    std::int32_t tm_gmtoff;
    std::uint32_t tm_zone;
};

static_assert(sizeof(GuestTm) == 44, "ARM EABI struct tm must be 44 bytes");

inline GuestStat ToGuestStat(const struct stat& host) {
    GuestStat guest{};
    guest.st_dev = static_cast<std::uint32_t>(host.st_dev);
    guest.st_ino = static_cast<std::uint32_t>(host.st_ino);
    guest.st_mode = static_cast<std::uint16_t>(host.st_mode);
    guest.st_nlink = static_cast<std::uint16_t>(host.st_nlink);
    guest.st_uid = static_cast<std::uint16_t>(host.st_uid);
    guest.st_gid = static_cast<std::uint16_t>(host.st_gid);
    guest.st_rdev = static_cast<std::uint32_t>(host.st_rdev);
    guest.st_size = static_cast<std::int32_t>(host.st_size);
    guest.st_blksize = static_cast<std::int32_t>(host.st_blksize);
    guest.st_blocks = static_cast<std::int32_t>(host.st_blocks);
    guest.atime = static_cast<std::int32_t>(host.st_atim.tv_sec);
    guest.atime_nsec = static_cast<std::uint32_t>(host.st_atim.tv_nsec);
    guest.mtime = static_cast<std::int32_t>(host.st_mtim.tv_sec);
    guest.mtime_nsec = static_cast<std::uint32_t>(host.st_mtim.tv_nsec);
    guest.ctime = static_cast<std::int32_t>(host.st_ctim.tv_sec);
    guest.ctime_nsec = static_cast<std::uint32_t>(host.st_ctim.tv_nsec);
    return guest;
}

inline struct tm FromGuestTm(const GuestTm& guest) {
    struct tm host{};
    host.tm_sec = guest.tm_sec;
    host.tm_min = guest.tm_min;
    host.tm_hour = guest.tm_hour;
    host.tm_mday = guest.tm_mday;
    host.tm_mon = guest.tm_mon;
    host.tm_year = guest.tm_year;
    host.tm_wday = guest.tm_wday;
    host.tm_yday = guest.tm_yday;
    host.tm_isdst = guest.tm_isdst;
    host.tm_gmtoff = guest.tm_gmtoff;
    return host;
}

}  // namespace armbat
