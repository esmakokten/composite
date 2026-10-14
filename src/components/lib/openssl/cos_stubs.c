/*
 * The system calls libcrypto's random generator makes, answered without a system call (a
 * component's default cos_syscall_handler asserts on any). Only the DRBG reaches them, and only
 * ECDSA signing (Phase 3) uses the DRBG; AES-GCM never does. Entropy is not among them: the
 * library is configured --with-rand-seed=rdcpu (configure-cos.sh), so seeds come from
 * RDSEED/RDRAND. What remains is the per-instance uniqueness input:
 *
 *   getpid        the DRBG's fork detection (openssl_get_fork_id) on every generate, and the
 *                 nonce at instantiation. A component never forks: a constant.
 *   clock_gettime the timestamp in the nonce and in the additional data, and (through time)
 *   gettimeofday  the DRBG's time-based reseed interval. The TSC read as nanoseconds: monotonic,
 *   time          unique per call, not a wall clock (reseeds come ~2x sooner than the interval
 *                 says, which only adds reseeds).
 *
 * Defined here so that musl's syscall versions are never linked into the component (musl's
 * clock_gettime is a weak alias; getpid, gettimeofday and time are only pulled when unresolved).
 */
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>

static inline unsigned long long
tsc(void)
{
	unsigned int a, d;

	__asm__ __volatile__("rdtsc" : "=a"(a), "=d"(d));

	return ((unsigned long long)d << 32) | a;
}

pid_t
getpid(void)
{
	return 1;
}

int
clock_gettime(clockid_t clk, struct timespec *ts)
{
	unsigned long long t = tsc();

	ts->tv_sec  = (time_t)(t / 1000000000ULL);
	ts->tv_nsec = (long)(t % 1000000000ULL);

	return 0;
}

int
gettimeofday(struct timeval *restrict tv, void *restrict tz)
{
	unsigned long long t = tsc();

	if (tv) {
		tv->tv_sec  = (time_t)(t / 1000000000ULL);
		tv->tv_usec = (suseconds_t)(t % 1000000000ULL / 1000);
	}

	return 0;
}

time_t
time(time_t *t)
{
	time_t s = (time_t)(tsc() / 1000000000ULL);

	if (t) *t = s;

	return s;
}
