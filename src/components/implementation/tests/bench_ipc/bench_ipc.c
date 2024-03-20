#include <cos_kernel_api.h>
#include <cos_types.h>
#include <pong.h>
#include <ps.h>
#include <perfdata.h>
#include <sched.h>


#define ITER 1024

volatile cycles_t start;
volatile cycles_t end;

struct perfdata perf;
cycles_t result[ITER] = {0, };

void
cos_init(void)
{
	perfdata_init(&perf, "Roundtrip IPC", result, ITER);
	printc("Ping component %ld: cos_init execution\n", cos_compid());

	return;
}

void
bench_pong_call(void)
{
	int i;

	for (i = 0; i < ITER; i++) {
		start = ps_tsc();
		pong_call();
		end = ps_tsc();
		perfdata_add(&perf, end - start);
	}
	perfdata_calc(&perf);
}

int
main(void)
{
	// Block the thread for 2^15 cycles
	sched_thd_block_timeout(0, ps_tsc() + (1 << 15));

	printc("Ping component %ld: main\n", cos_compid());

	bench_pong_call();

	perfdata_all(&perf);
	perfdata_print(&perf);

	return 0;
}
