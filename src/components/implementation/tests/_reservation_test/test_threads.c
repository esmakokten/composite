#include <llprint.h>
#include <perfdata.h>
#include <res_spec.h>
#include <sched.h>
#include <cos_time.h>
#include <initargs.h>
#include <cos_trace.h>
#include <string.h>
#include <math.h>

#define SL_FPRR_NPRIOS 32
#define LOWEST_PRIORITY (SL_FPRR_NPRIOS - 1)
#define HIGHEST_PRIORITY 1
#define MID_PRIORITY (SL_FPRR_NPRIOS / 2)

#define QUANTUM 10000
#define LARGE_PERIOD 1 << 12
#define LARGE_BUDGET 1 << 12

#define ITERATION 10000

#ifndef NUM_TEST_THREADS
#define NUM_TEST_THREADS 100
#endif

static volatile int timer_test_done = 0;
static thdid_t main_tid = 0;
static int num_of_threads = 0;
static double average_interarrival_time = 100.0;

/* 
 * Comparison test for scheduler overheads and number of timer interrupts
 * Two sets of threads, one with high priority and one with low priority.
 * 1. Periodic tasks with preemption
 *	- All threads spin and consume the budget
 * 2. Aperiodic task 
 *  - threads block 
 * 
 */

/* To measure the cost of rate limiting */

enum thd_type_t {
	PERIODIC = 0,
	APERIODIC,
};

struct thread_props {
	enum thd_type_t type;
	thdid_t tid;
	int priority;
	int budget_us;
	int period_us;
};

struct thread_props threads[NUM_TEST_THREADS];

double
random_uniform(void)
{
	return (double)rand() / (double)RAND_MAX;
}

static void
periodic(void *d)
{
	while (timer_test_done == 0);

	sched_thd_block(0);
}

static void
aperiodic(void *d)
{
	while (timer_test_done == 0) {
		double lambda = 1.0 / average_interarrival_time;
		double interarrival_time = -log(1.0 - random_uniform()) / lambda;

		printc("Interarrival time: %f\n", interarrival_time);
		sched_thd_block_timeout(0, time_now() + time_usec2cyc(interarrival_time));
	}

	sched_thd_block(0);
}

int
parse_threads_from_args(void)
{
	struct initargs params, curr;
	struct initargs_iter i;
	char *token;
	int ret = 0;

	ret = args_get_entry("param", &params);
	assert(!ret);

	int num = 0;
	int len = 0;

	for (ret = args_iter(&params, &i, &curr) ; ret ; ret = args_iter_next(&i, &curr)) {
		char    *args = args_value(&curr);
		assert(args);
		struct thread_props thd = {0};

		const char* number = args_key(&curr, &len);
		// Create a char array to store the number
		char number_of_tasks_str[10] = {'\0'};
		memcpy(number_of_tasks_str, number, len);
		int number_of_tasks = atoi(number_of_tasks_str);

		PRINTLOG(PRINT_DEBUG, "Number of tasks: %d\n", number_of_tasks);

		int result = sscanf(args, "%d,%d,%d,%d", (int*)&thd.type, &thd.priority, &thd.period_us, &thd.budget_us);
		
		if (result != 4) {
			PRINTLOG(PRINT_DEBUG, "Parsing failed\n");
			return 0;
		}
		
		for (int i = 0; i < number_of_tasks; i++) {
			PRINTLOG(PRINT_DEBUG, "Thread %d: Type: %d, Priority: %d, Period: %d usec, Budget: %d usec\n", num, thd.type, thd.priority, thd.period_us, thd.budget_us);
			threads[num].type = thd.type;
			threads[num].priority = thd.priority;
			threads[num].period_us = thd.period_us;
			threads[num].budget_us = thd.budget_us;
			num++;
		}
	}

	return num;
}

void
create_threads(int num)
{
	for (int i = 0; i < num; i++) {
		struct thread_props* thd = &threads[i];
		switch (thd->type)
		{
		case PERIODIC:
		{
			thd->tid = sched_thd_create(periodic, NULL);
			sched_thd_param_set(thd->tid, sched_param_pack(SCHEDP_PRIO, thd->priority));
			sched_thd_param_set(thd->tid, sched_param_pack(SCHEDP_BUDGET, thd->budget_us));
			sched_thd_param_set(thd->tid, sched_param_pack(SCHEDP_WINDOW, thd->period_us));
			PRINTLOG(PRINT_DEBUG, "Creating Spinning TID: %ld, priority %d, period %d, budget %d\n", thd->tid, thd->priority, thd->period_us, thd->budget_us);
			break;
		}
		case APERIODIC:
		{
			thd->tid = sched_thd_create(aperiodic, NULL);
			sched_thd_param_set(thd->tid, sched_param_pack(SCHEDP_PRIO, thd->priority));
			sched_thd_param_set(thd->tid, sched_param_pack(SCHEDP_BUDGET, thd->budget_us));
			sched_thd_param_set(thd->tid, sched_param_pack(SCHEDP_WINDOW, thd->period_us));
			PRINTLOG(PRINT_DEBUG, "Creating Aperiodic TID: %ld, priority %d, period %d, budget %d\n", thd->tid, thd->priority, thd->period_us, thd->budget_us);
			break;
		}
		default:
			assert(0);
			break;
		}
	}
}

int
main(void)
{
	printc("Interarrival times:");
	for(int i = 0; i < 100; i++) {
		double lambda = 1.0 / average_interarrival_time;
		double interarrival_time = -log(1.0 - random_uniform()) / lambda;

		printc("%f,", interarrival_time);
		sched_thd_block_timeout(0, time_now() + time_usec2cyc(interarrival_time));
	}
	printc("\n");
	// Set main thread to be the highest priority
	main_tid = cos_thdid();
	sched_thd_param_set(main_tid, sched_param_pack(SCHEDP_PRIO, HIGHEST_PRIORITY));
	srand(time_now());

	int num_of_threads = 0;
	num_of_threads = parse_threads_from_args();
	PRINTLOG(PRINT_DEBUG, "## Starting tests with %d threads...\n", num_of_threads);

	// Create threads
	create_threads(num_of_threads);

	cycles_t start, end;
	uint16_t iter;
	start = time_now();
	sched_thd_get_param(cos_thdid(), 4); // Start measurement in slm
	// Block and wait for the test to wake up back
	do {
		sched_thd_block_timeout(0, time_now() + time_usec2cyc(QUANTUM * ITERATION));
		// Check if the test is done
		timer_test_done = 1;
	} while (timer_test_done == 0);
	end = time_now();
	
	PRINTLOG(PRINT_DEBUG, "## Test duration: %llu us\n", time_cyc2usec(end - start));
	// Print the policy overhead
	cycles_t policy_overhead = sched_thd_get_param(cos_thdid(), 0);
	PRINTLOG(PRINT_DEBUG, "Policy overhead: %llu us\n", time_cyc2usec(policy_overhead));
	// Print the switch count
	PRINTLOG(PRINT_DEBUG, "Overhead/Time ratio: %f\n", (double)policy_overhead / (end - start));
	unsigned int switch_cnt = sched_thd_get_param(cos_thdid(), 1);
	PRINTLOG(PRINT_DEBUG, "Reschedule count: %d\n", switch_cnt);
	// Print the timer count
	unsigned int timer_cnt = sched_thd_get_param(cos_thdid(), 8);
	PRINTLOG(PRINT_DEBUG, "Timer count: %d\n", timer_cnt);

	// Print total execution time and switch count for each thread
	for (int i = 0; i < num_of_threads; i++) {
		cycles_t exec_time = sched_thd_get_param(threads[i].tid, 6);
		unsigned int thd_switch_cnt = sched_thd_get_param(threads[i].tid, 7);
		PRINTLOG(PRINT_DEBUG, "TID %ld: Execution time: %llu us, Switch count: %d\n", threads[i].tid, time_cyc2usec(exec_time), thd_switch_cnt);
	}
	cycles_t exec_time = sched_thd_get_param(0, 6);
	unsigned int thd_switch_cnt = sched_thd_get_param(0, 7);
	PRINTLOG(PRINT_DEBUG, "TID Idle: Execution time: %llu us, Switch count: %d\n", time_cyc2usec(exec_time), thd_switch_cnt);
	exec_time = sched_thd_get_param(1, 6);
	thd_switch_cnt = sched_thd_get_param(1, 7);
	PRINTLOG(PRINT_DEBUG, "TID Sched: Execution time: %llu us, Switch count: %d\n", time_cyc2usec(exec_time), thd_switch_cnt);
	
	PRINTLOG(PRINT_DEBUG, "## Tests completed\n");

	sched_thd_block(0);
}
