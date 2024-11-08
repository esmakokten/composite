#include <llprint.h>
#include <perfdata.h>
#include <res_spec.h>
#include <sched.h>
#include <cos_time.h>
#include <initargs.h>
#include <cos_trace.h>
#include <string.h>
#include <heap.h>

#define SL_FPRR_NPRIOS 32
#define LOWEST_PRIORITY (SL_FPRR_NPRIOS - 1)
#define HIGHEST_PRIORITY 1
#define MID_PRIORITY (SL_FPRR_NPRIOS / 2)

#define QUANTUM 500
#define ITERATION 50000

static volatile int timer_test_done = 0;
static thdid_t main_tid = 0;

/* Test for coparing different scheduling policies */
/* The key operations compared are:
 * 1. Timer -- for rate limiting
 *    One spinning thread t 
 * 	  For FPDS, FPSS and FPRES 
 *		t->period = 2 quantums 
 * 		t->budget = 1 quantum
 * 
 * 2. Block and Wakeup API
 *    # of iterations we call sched_block(t2) and sched_wakeup(t2).
 * 	  For FPDS, FPSS and FPRES make sure t2 has infinite budget
 */

/* To measure the cost of rate limiting */
static void
spin(void *arg)
{
	/* Spin until the test is done */
	unsigned int iter = 0;
	while (iter < ITERATION) {
		iter = sched_thd_get_param(cos_thdid(), 5);
	}

	sched_thd_wakeup(main_tid);
	sched_thd_block(0);

	PRINTLOG(PRINT_DEBUG, "## 2 Should not reach here\n");
}

/* To measure the cost of Wakeup and Block API */
/* NOTE: You need to hack sched_thd_block to enable block other threads */
static void
mock(void *arg)
{
	/* We just block and wakeup this thread using another thread */
	/* The thread is assigned to a lowest priority and never gets scheduled */
	assert(0);
}

int
measure_cost_wakeup_block_api(void)
{
	thdid_t t2;
	int i = 0;
	sched_param_t sps[] = {
		SCHED_PARAM_CONS(SCHEDP_PRIO, LOWEST_PRIORITY)
	};

	t2 = sched_thd_create(mock, NULL);
	sched_thd_param_set(t2, sps[0]);

	while (i < ITERATION) {
		sched_thd_block_bench(t2);
		sched_thd_wakeup(t2);
		i++;
	}

	sched_thd_block_bench(t2);

	return 0;
}

int
measure_cost_timer_rate_limiting(void)
{
	thdid_t spinning_thread;

	sched_param_t sps[] = {
		SCHED_PARAM_CONS(SCHEDP_PRIO, LOWEST_PRIORITY),
		SCHED_PARAM_CONS(SCHEDP_WINDOW, 2 * QUANTUM),
		SCHED_PARAM_CONS(SCHEDP_BUDGET, QUANTUM)
	};

	spinning_thread = sched_thd_create(spin, NULL);
	sched_thd_param_set(spinning_thread, sps[0]);
	sched_thd_param_set(spinning_thread, sps[1]);
	sched_thd_param_set(spinning_thread, sps[2]);

	/* Trigger scheduler to measure the timer cost for rate limiting */
	return 0;
}


int
main(void)
{
	PRINTLOG(PRINT_DEBUG, "## Starting tests...\n");

	/* Set the main thread to the highest priority */
	sched_thd_param_set(cos_thdid(), sched_param_pack(SCHEDP_PRIO, HIGHEST_PRIORITY));
	main_tid = cos_thdid();
	//Start the recording of the timer cost
	sched_thd_get_param(cos_thdid(), 4);

	//measure_cost_wakeup_block_api();

	PRINTLOG(PRINT_DEBUG, "## Wakeup and Block API tests completed\n");

	measure_cost_timer_rate_limiting();
	//Block the main thread until the test is done
	sched_thd_block(0);
	//sched_thd_block_timeout(0, time_now() + time_usec2cyc(2 * QUANTUM * ITERATION));
	timer_test_done = 1;
	PRINTLOG(PRINT_DEBUG, "## Timer tests completed\n");
	
	// Check if the test is done
	unsigned int test_done = sched_thd_get_param(cos_thdid(), 5);
	PRINTLOG(PRINT_DEBUG, "Iter done: %d\n", test_done);

	// Scheduler timer for rate limiting 
	cycles_t policy_cost = sched_thd_get_param(cos_thdid(), 0);
	// Print the switch count
	unsigned int switch_cnt = sched_thd_get_param(cos_thdid(), 1);
	PRINTLOG(PRINT_DEBUG, "Policy cost: %llu\n", policy_cost);
	PRINTLOG(PRINT_DEBUG, "Switch count: %d\n", switch_cnt);
	PRINTLOG(PRINT_DEBUG, "## Tests completed\n");

	sched_thd_block(0);

	PRINTLOG(PRINT_DEBUG, "## 3 Should not reach here\n");
	return 0;
}
