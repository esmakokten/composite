#include <llprint.h>
#include <perfdata.h>
#include <res_spec.h>
#include <sched.h>
#include <cos_time.h>
#include <initargs.h>
#include <cos_trace.h>
#include <string.h>
#include <heap.h>
#include <crt_blkpt.h>

#define SL_FPRR_NPRIOS 32

#define LOWEST_PRIORITY (SL_FPRR_NPRIOS - 1)
#define HIGHEST_PRIORITY 1

#ifndef REPL_WINDOW_SIZE
#define REPL_WINDOW_SIZE 32
#endif

#ifndef MINIMUM_TIMER_COST_THRESHOLD
#define MINIMUM_TIMER_COST_THRESHOLD 1000
#endif

// HP+Interference 2000000
// THurd MAIN_THREAD_SLEEPING_TIME_US 3000000 
#ifndef MAIN_THREAD_SLEEPING_TIME_US 
#define MAIN_THREAD_SLEEPING_TIME_US 2000000 
#endif

#define ITER 1024
#define INTER_ITER 		   10000000

volatile cycles_t g_origin = 0;
volatile cycles_t g_last_rdtscl = 0;
volatile cycles_t g_preemption_lost = 0;
volatile unsigned g_rtdsc_count = 0;
volatile thdid_t g_main_tid = 0;

volatile cycles_t blocking_tau = 0;
volatile cycles_t block_for = 0;

volatile bool g_test_finished = false;
volatile int g_num_of_threads = 0;
volatile cycles_t g_last_yield_cyc = 0;
volatile cycles_t g_yielder_period_cyc = 0;
volatile cycles_t g_thunder_period_cyc = 0;

struct perfdata perf;
cycles_t result[ITER] = {0, };
cycles_t interrupt_cost[INTER_ITER] = {0, };

static void
workload(unsigned long long loop_count)
{
	unsigned long long i, j;

    for (i = 0; i < loop_count; ++i) {
        for (j = 0; j < 100; ++j) {
            __asm__ volatile(""); // Prevents the compiler from optimizing the loop away
        }
    }
}

static void
spinning_task()
{
	// Wait for the test to finish
	while (g_test_finished == false);	
	COS_TRACE("\"event\": \"spinning_task_exit\", \"tid\": %lu", cos_thdid(),0,0);
	sched_thd_block(0);

	PRINTLOG(PRINT_DEBUG, "⚠⚠ TID: %lu, Should not reach here\n", cos_thdid());
}

volatile int g_num_of_lp_threads_yieled = 0;
volatile thdid_t g_blocked_yielder_first_tid = 0;
volatile thdid_t g_blocked_yielder_last_tid = 0;

struct crt_blkpt b;
static void
yielding_task()
{
	/* We are not planning new replenishment for yield
	for (int i = 0; i < REPL_WINDOW_SIZE; i++) {
		sched_thd_yield_to(7);
	}
	*/
	//COS_TRACE("\"event\": \"yielding_task_start\", \"tid\": %lu", cos_thdid(),0,0);
	for (int i = 0; i < REPL_WINDOW_SIZE; i++) {
		
		if (g_blocked_yielder_last_tid != cos_thdid()) {	
			sched_thd_block(0);
			sched_thd_wakeup(cos_thdid()+1);
			//COS_TRACE("\"event\": \"yielding_task_iteration\", \"tid\": %lu, \"iteration\": %d, \"waking_up_tid\": %lu", cos_thdid(), i, cos_thdid()+1);

		} else {
			sched_thd_wakeup(g_blocked_yielder_first_tid+1);
			sched_thd_block(0);
			//COS_TRACE("\"event\": \"yielding_task_iteration\", \"tid\": %lu, \"iteration\": %d, \"waking_up_tid\": %lu", cos_thdid(), i, g_blocked_yielder_first_tid+1);
		}
	}

	if (g_blocked_yielder_last_tid == cos_thdid()) {
		sched_thd_wakeup(g_blocked_yielder_first_tid);
	} else {
		sched_thd_block(0);
	}
	/*
	struct crt_blkpt_checkpoint c;
	crt_blkpt_checkpoint(&b, &c);
	crt_blkpt_wait(&b, 0, &c);
	*/	
	
	ps_faa(&g_num_of_lp_threads_yieled, 1);

	//if(!(g_blocked_yielder_first_tid == cos_thdid())) {
	//	sched_thd_wakeup(cos_thdid()-1);
	//}

	if (g_num_of_lp_threads_yieled == g_num_of_threads - 2) {
		g_last_yield_cyc = time_now();
		COS_TRACE("\"event\": \"yielding_task_exit\", \"tid\": %lu", cos_thdid(),0,0);
	}

	// Wait for the test to finish
	while(g_test_finished == false);
	sched_thd_block(0);

	PRINTLOG(PRINT_DEBUG, "⚠⚠ TID: %lu, Should not reach here\n", cos_thdid());
}

static void
yielding_task_first()
{
	// Wait for the test to finish
	sched_thd_block(0);
	//COS_TRACE("\"event\": \"yielding_task_first\", \"tid\": %lu", cos_thdid(),0,0);
	//crt_blkpt_trigger(&b, CRT_BLKPT_WAKE_ALL);
	//Wake up all the threads
	for (int i = cos_thdid()+1; i < g_blocked_yielder_last_tid; i++) {
		//COS_TRACE("\"event\": \"yielding_task_first\", \"tid\": %lu, \"wake_up_tid\": %lu", cos_thdid(), i,0);
		sched_thd_wakeup(i);
	}
	//COS_TRACE("\"event\": \"yielding_task_first\", \"tid\": %lu, \"triggered\": CRT_BLKPT_WAKE_ALL", cos_thdid(),0,0);
	sched_thd_block(0);

	PRINTLOG(PRINT_DEBUG, "⚠⚠ TID: %lu, Should not reach here\n", cos_thdid());
}

int g_num_of_lp_threads_blkpnt = 0;
static void
yielding_blkpnt_task()
{
	for (int i = 0; i < REPL_WINDOW_SIZE; i++) {
		struct crt_blkpt_checkpoint c;
		crt_blkpt_checkpoint(&b, &c);
		crt_blkpt_wait(&b, 0, &c);
		COS_TRACE("TID: %lu, Yielding Blkpnt Task Iteration: %d", cos_thdid(), i,0);
	}
	ps_faa(&g_num_of_lp_threads_blkpnt, 1);	
	// Wait for the test to finish
	while(g_test_finished == false);
	sched_thd_block(0);

	PRINTLOG(PRINT_DEBUG, "⚠⚠ TID: %lu, Should not reach here\n", cos_thdid());
}

static void
blkpnt_release_task()
{
	for (int i = 0; i < REPL_WINDOW_SIZE-1; i++) {
		COS_TRACE("TID: %lu, Blkpnt Release Task Iteration: %d", cos_thdid(), i,0);
		crt_blkpt_trigger(&b, CRT_BLKPT_WAKE_ALL);
	}

	ps_faa(&g_num_of_lp_threads_blkpnt, 1);	

	if (g_num_of_lp_threads_blkpnt == g_num_of_threads - 1) {
		g_last_yield_cyc = time_now();
		PRINTLOG(PRINT_DEBUG, "⚠ TID: %lu, Last yielder reached, time: %llu\n", cos_thdid(), g_last_yield_cyc);
	}

	// Wait for the test to finish
	while(g_test_finished == false);
	sched_thd_block(0);

	PRINTLOG(PRINT_DEBUG, "⚠⚠ TID: %lu, Should not reach here\n", cos_thdid());
}

static void
blocking_task()
{
	cycles_t now = time_now();
	cycles_t wake_up = now + block_for - (now % block_for);;
	sched_thd_block_timeout(0, wake_up);
}

// TODO pass the blocking tau as an argument
static void
sleeping_task()
{
	int repl_buffer_size = 0;
	while (repl_buffer_size < REPL_WINDOW_SIZE){
		++repl_buffer_size;
		sched_thd_block_timeout(0, g_origin + blocking_tau);
	}

	// Wait for the test to finish
	while(g_test_finished == false);
	sched_thd_block(0);
}

// TODO pass the blocking time as an argument
static void
accounting_task()
{
//#define TIMER_OVERHEAD_TEST
#ifdef TIMER_OVERHEAD_TEST
	g_last_rdtscl = time_now();
#else
	// TODO: Can use block_periodic()
	// g_last_rdtscl = time_now();
	//cycles_t wake_up = g_last_rdtscl + block_for - (g_last_rdtscl % block_for);
	// g_last_rdtscl = wake_up;
	
	// HP Interference notes
	// Block for is used to wake up at the time that YIELDERs first replenishment
	// The other important detail is that make sure to set YIELDERS period so that all YIELDERS make their first replenishment buffer full
	// So make sure to starting with the max num of threads and replenishment buffer size

	g_last_rdtscl = COS_TRACE("\"event\": \"hp_task_scheduled\", \"tid\": %lu, \"block_for\": %llu", cos_thdid(), block_for, 0);
	g_last_rdtscl += block_for;
	sched_thd_block_timeout(0, g_last_rdtscl);
	g_origin = g_last_rdtscl;
#endif
	/* Option 1 */
	while (g_test_finished == false && g_last_rdtscl < g_last_yield_cyc+g_yielder_period_cyc) {	
		// COS_TRACE("\"event\": \"hp_task_scheduled_wake_up\", \"tid\": %lu, \"time\": %llu", cos_thdid(), wake_up, 0);
		// The first loss is the time between the planned wake up and the actual wake up
		perfdata_add(&perf, time_now() - g_last_rdtscl);		
		g_last_rdtscl = time_now();
	}
	
	/* Option 2 
	while (g_test_finished == false) {
		cycles_t now = time_now();
		if (now - g_last_rdtscl > 100) {
			COS_TRACE("\"event\": \"hp_task_preemption_lost\", \"amount\": %llu, \"count\": %u", now - g_last_rdtscl, g_rtdsc_count, 0);
			g_preemption_lost += now - g_last_rdtscl;
			g_rtdsc_count++;
		}
		g_last_rdtscl = now;
	}
	*/
	COS_TRACE("\"event\": \"hp_task_itself\", \"tid\": %lu, \"execution_span\": %llu", cos_thdid(), g_last_rdtscl-g_origin, 0);
	sched_thd_block(0);

	PRINTLOG(PRINT_DEBUG, "⚠⚠ TID: %lu, Should not reach here\n", cos_thdid());
}

// TODO pass the blocking time as an argument
volatile cycles_t g_thunder_expected_wakeup = 0;

static void
hurd_accounting_task()
{
	// Defferable server and Deferrable server priority aware
    // Block periodically to be awake at the time of the activation
	// N LP Spinning tasks in DS will woke up at the same time

	g_last_rdtscl = time_now();
	g_thunder_expected_wakeup = g_last_rdtscl + g_thunder_period_cyc - (g_last_rdtscl % g_thunder_period_cyc);

	sched_thd_block_periodic(0);
	g_last_rdtscl = time_now();
	COS_TRACE("\"event\": \"th_acc_task_itself\", \"expected\": %llu, \"actual_wakeup\": %llu, \"interference\": %llu", g_thunder_expected_wakeup, g_last_rdtscl, g_last_rdtscl - g_thunder_expected_wakeup);
	g_test_finished = true;
	sched_thd_wakeup(g_main_tid);

	sched_thd_block(0);

	PRINTLOG(PRINT_DEBUG, "⚠⚠ TID: %lu, Should not reach here\n", cos_thdid());


	while(1){
		sched_thd_block(0);
	}
}

cycles_t
measure_cycles(unsigned long long loop_count)
{
	unsigned cycles_high, cycles_low, cycles_high1, cycles_low1;

	__asm__ __volatile__("cpuid\n\t" 
						 "rdtsc\n\t" 
						 "mov %%edx, %0\n\t" 
						 "mov %%eax, %1\n\t" : 
						 "=r" (cycles_high), "=r" (cycles_low) :: "%rax", "%rbx", "%rcx", "%rdx");

	workload(loop_count);

	__asm__ __volatile__("rdtscp\n\t" 
						 "mov %%edx, %0\n\t" 
						 "mov %%eax, %1\n\t" 
						 "cpuid\n\t" : 
						 "=r" (cycles_high1), "=r" (cycles_low1) :: "%rax", "%rbx", "%rcx", "%rdx");


	cycles_t start = (((cycles_t)cycles_high << 32) | cycles_low);
	cycles_t end = (((cycles_t)cycles_high1 << 32) | cycles_low1);

	PRINTLOG(PRINT_DEBUG, "Loop Count: %llu Start: %llu, End: %llu, Diff: %llu\n",loop_count , start, end, end - start);

	return end - start;
}

cycles_t
find_loop_count(cycles_t desired_execution_time)
{
	unsigned long long loop_count_min = 25000; // For 1 ms measured loop count is ~25900
	unsigned long long loop_count = 0;

	for (unsigned long long i = loop_count_min; i < desired_execution_time; i++)
	{
		cycles_t cost = 0;
		for (int j = 0; j < 1000; j++)
		{
			cost += measure_cycles(i);
		}
		cost /= 1000;

		// If it is in error margin, break
		//if (cost > desired_execution_time - 2000 && cost < desired_execution_time + 2000)
		if (cost >= desired_execution_time)	{
			loop_count = i;
			PRINTLOG(PRINT_DEBUG, "Loop count: %llu, Cycles: %llu\n", i, cost);
			break;
		}

	}

	// Measure it again with the new loop count
	cycles_t avg2 = 0;
	for (size_t i = 0; i < 1000; i++)
	{
		avg2 += measure_cycles(loop_count);

	}
	avg2 /= 1000;

	PRINTLOG(PRINT_DEBUG, "Average cycles: %llu Desired cycles: %llu\n", avg2, desired_execution_time);

	return loop_count;
}


enum thd_type_t {
	SPINNER = 0,
	YIELDER = 1, 
	ACCOUNTER = 2, 
	HURD_ACCOUNTER = 3,
	SLEEPER = 4, 
	THUNDER = 5,// TODO: Add more types
	BLOCKING = 6,
	YIELDER_BLKPNT = 7
};

struct thread_props {
	enum thd_type_t type;
	thdid_t tid;
	int priority;
	int budget_us;
	int period_us;
	int execution_us;
	int block_us;
};

volatile cycles_t pre_thunder_period_us = 0;
thdid_t
create_thread(struct initargs * params)
{
	char    *args = args_value(params);
	assert(args);
	struct thread_props thd;

	int result = sscanf(args, "%d,%d,%d,%d,%d,%d", (int*)&thd.type, &thd.priority, &thd.period_us, &thd.budget_us, &thd.execution_us, &thd.block_us);
    
	if (result != 6) {
        PRINTLOG(PRINT_DEBUG, "Parsing failed\n");
        return 1;
    }

	// Create spinning tasks
	int len = 0;
	const char* number = args_key(params,&len);
	// Create a char array to store the number
	char number_of_tasks_str[10] = {'\0'};
	memcpy(number_of_tasks_str, number, len);
	int number_of_tasks = atoi(number_of_tasks_str);
	PRINTLOG(PRINT_DEBUG, "Number of tasks: %d\n", number_of_tasks);

	switch (thd.type)
	{
	case SPINNER:
	{	
		// Create the tasks
		for (int i = 0; i < number_of_tasks; i++)
		{
			g_num_of_threads++;
			thd.tid = sched_thd_create(spinning_task, NULL);
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_PRIO, thd.priority));
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_BUDGET, thd.budget_us));
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_WINDOW, thd.period_us));			
		}
		break;
	}
	case YIELDER:
	{
		blocking_tau = time_usec2cyc(thd.block_us);
		g_yielder_period_cyc = time_usec2cyc(thd.period_us);
		// Create yielding tasks
		for (int i = 0; i < number_of_tasks; i++)
		{
			g_num_of_threads++;
			if (i == 0) 
			{

				thd.tid = sched_thd_create(yielding_task_first, NULL);
				sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_PRIO, thd.priority-1));
				PRINTLOG(PRINT_DEBUG, "## First Yielder TID: %lu\n", thd.tid);
				g_blocked_yielder_first_tid = thd.tid;
				continue;
			}

			thd.tid = sched_thd_create(yielding_task, NULL);
			
			if (i == number_of_tasks - 1)
			{
				PRINTLOG(PRINT_DEBUG, "## Last Yielder TID: %lu\n", thd.tid);
				g_blocked_yielder_last_tid = thd.tid;
			}
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_PRIO, thd.priority));
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_BUDGET, thd.budget_us));
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_WINDOW, thd.period_us));
			
		}		
		break;
	}
	case YIELDER_BLKPNT:
	{
		blocking_tau = time_usec2cyc(thd.block_us);
		g_yielder_period_cyc = time_usec2cyc(thd.period_us);
		// Create yielding tasks
		for (int i = 0; i < number_of_tasks -1 ; i++)
		{
			g_num_of_threads++;
			thd.tid = sched_thd_create(yielding_blkpnt_task, NULL);
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_PRIO, thd.priority));
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_BUDGET, thd.budget_us));
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_WINDOW, thd.period_us));
		}		
		// Create the trigger task
		g_num_of_threads++;
		thd.tid = sched_thd_create(blkpnt_release_task, NULL);
		// Priority is set to little lower because this task should be scheduled after the yielding tasks
		sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_PRIO, thd.priority + 1));
		sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_BUDGET, thd.budget_us));
		sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_WINDOW, thd.period_us));
		PRINTLOG(PRINT_DEBUG, "Trigger task created with TID: %lu\n", thd.tid);
		break;
	}
	case ACCOUNTER:
	{
		block_for = time_usec2cyc(thd.block_us);
		// TODO: Add the blocking time as an argument
		thd.tid = sched_thd_create(accounting_task, NULL);
		g_num_of_threads++;
		sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_PRIO, thd.priority));
		sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_BUDGET, thd.budget_us));
		sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_WINDOW, thd.period_us));
		break;
	}
	case HURD_ACCOUNTER:
	{
		g_thunder_period_cyc = time_usec2cyc(thd.period_us);
		// TODO: Add the blocking time as an argument
		thd.tid = sched_thd_create(hurd_accounting_task, NULL);
		g_num_of_threads++;
		sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_PRIO, thd.priority));
		sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_BUDGET, thd.budget_us));
		sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_WINDOW, thd.period_us));
		break;
	}
	case SLEEPER:
	{	
		// Threads that wake up at the same time
		blocking_tau = time_usec2cyc(thd.block_us);
		// TODO: Add the blocking time as an argument
		thd.tid = sched_thd_create(sleeping_task, NULL);
		g_num_of_threads++;
		sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_PRIO, thd.priority));
		sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_BUDGET, thd.budget_us));
		sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_WINDOW, thd.period_us));
		break;
	}
	case THUNDER:
	{
		// Create thundering tasks
		for (int i = 0; i < number_of_tasks; i++)
		{
			thd.period_us = thd.period_us + i;
			g_num_of_threads++;
			// Create yielding tasks with different periods
			thd.tid = sched_thd_create(spinning_task, NULL);
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_PRIO, thd.priority));
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_BUDGET, thd.budget_us));
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_WINDOW, thd.period_us));
		}
		
		break;
	}
	case BLOCKING:
	{
		block_for = time_usec2cyc(thd.block_us);
		// Create blocking tasks
		for (int i = 0; i < number_of_tasks; i++)
		{
			g_num_of_threads++;
			thd.tid = sched_thd_create(blocking_task, NULL);
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_PRIO, thd.priority));
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_BUDGET, thd.budget_us));
			sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_WINDOW, thd.period_us));

		}
		break;
	}
	/* TODO */
	default:
		assert(0);
		break;
	}

	PRINTLOG(PRINT_DEBUG, "\t TID: %lu, Type: %d, Priority: %d, Period: %d usec(%llu), Budget: %d usec(%llu)\n", thd.tid, thd.type, thd.priority, thd.period_us, time_usec2cyc(thd.period_us), thd.budget_us, time_usec2cyc(thd.budget_us));

	assert(thd.priority >= HIGHEST_PRIORITY && thd.priority <= LOWEST_PRIORITY);
	assert(thd.period_us >= thd.budget_us);

	if (thd.type == YIELDER || thd.type == SPINNER) {
		return thd.tid;
	}
	
	return thd.tid;
}

void
cos_init(void)
{
	perfdata_init(&perf, "Single trace cost", result, ITER);
	// Measure the cost of trace
	for (int i = 0; i < ITER; i++) {
		cycles_t start = time_now();
		COS_TRACE("\"event\": \"measure_trace\", \"tid\": %lu, \"arg1\": %d, \"arg2\": %d", cos_thdid(), 1, 2);
		perfdata_add(&perf, time_now() - start);
	}
	// Empty the trace buffer
	cos_trace_empty_buffer();
	perfdata_calc(&perf);
	perfdata_print(&perf);

	perfdata_init(&perf, "Single rtdsc cost", result, ITER);
	// Measure rtdsc cost
	for(int i = 0; i < ITER; i++) {
		cycles_t first = time_now();
		perfdata_add(&perf, time_now() - first);
	}
	perfdata_calc(&perf);
	perfdata_print(&perf);

	/*
	perfdata_init(&perf, "heap_add cost", result, ITER);
	// Measure heap_highest cost
	struct timer_global *g = timer_global();
	memset(g, 0, sizeof(struct timer_global));
	heap_init(&g->h, MAX_NUM_THREADS, __compare_min, __update_idx);
	// Fill heap with the same value
	struct mock_thd t[ITER];
	for (int i = 0; i < ITER; i++) {
		t[i].abs_next_processing = 11110000; 
		cycles_t start = time_now();
		heap_add(&g->h, &t[i]);
		cycles_t end = time_now();
		perfdata_add(&per/f, end - start);
	}
	perfdata_calc(&perf);
	perfdata_all(&perf);

	perfdata_init(&perf, "heap_highest(same values) cost", result, ITER);
	// Dequeue thread with closest wakeup 
	for (int i = 0; i < ITER; i++) {
		cycles_t start = time_now();
		struct mock_thd *t = heap_highest(&g->h);
		cycles_t end = time_now();
		perfdata_add(&perf, end - start);
	}
	perfdata_calc(&perf);
	perfdata_all(&perf);

	// Fill heap with the different values
	for (int i = 0; i < ITER; i++) {
		t[i].abs_next_processing = 11110000 + i;
		heap_add(&g->h, &t[i]);
	}
	perfdata_init(&perf, "heap_highest(different values) cost", result, ITER);
	// Dequeue thread with closest wakeup 
	for (int i = 0; i < ITER; i++) {
		cycles_t start = time_now();
		struct mock_thd *t = heap_highest(&g->h);
		cycles_t end = time_now();
		perfdata_add(&perf, end - start);
	}
	perfdata_calc(&perf);
	perfdata_all(&perf);
	*/

	perfdata_init(&perf, "HP Timer Interference", interrupt_cost, INTER_ITER);
	assert(crt_blkpt_init(&b) == 0);
	return;
}

int
main(void)
{
	g_main_tid = cos_thdid();
	sched_thd_param_set(g_main_tid, sched_param_pack(SCHEDP_PRIO, HIGHEST_PRIORITY));

	struct initargs params, curr;
	struct initargs_iter i;
	char *token;
	int ret = 0;

	ret = args_get_entry("param", &params);
	assert(!ret);

	int num = 0;
	for (ret = args_iter(&params, &i, &curr) ; ret ; ret = args_iter_next(&i, &curr)) {
		//thread_ids[num++] = 
		create_thread(&curr);
	}

	// Start the test
	cycles_t time_before_wakeup = time_now();
	g_origin = time_before_wakeup;
	cycles_t sleep = time_before_wakeup + time_usec2cyc(MAIN_THREAD_SLEEPING_TIME_US);
	PRINTLOG(PRINT_DEBUG, "## TEST START ##\n #Thds: %d, ReplWinSize: %d\n", g_num_of_threads, REPL_WINDOW_SIZE);

#define HP_INTERFERENCE
#ifdef HP_INTERFERENCE
	sched_thd_block_timeout(0, sleep);
#else
	//sched_thd_block_timeout(0, sleep);
	sched_thd_block(0);
#endif	

	// Finish test threads
	g_test_finished = true;
	cycles_t wakeup = time_now();
	cycles_t spent = wakeup - time_before_wakeup; 	
	printc("## TEST FINISHED ##\n");

#ifdef HP_INTERFERENCE		
	printc("\n## TEST PERF DATA ##\n");
	//perfdata_all(&perf);
	printc("\nIgnore values under the threshold: %d\n", MINIMUM_TIMER_COST_THRESHOLD);
	perfdata_special2_calc(&perf, MINIMUM_TIMER_COST_THRESHOLD);
	perfdata_special_print(&perf, MINIMUM_TIMER_COST_THRESHOLD);

	printc("\n## SUMMARY ##\n");
	printc("Time spent: %llu us, Time before wakeup: %llu, Time after wakeup: %llu\n", time_cyc2usec(spent), time_before_wakeup, wakeup);
	printc("Yielded threads: %d, Yielder Period: %llu, Last yield time: %llu\n", g_num_of_lp_threads_yieled, g_yielder_period_cyc, (g_last_yield_cyc + g_yielder_period_cyc));
	COS_TRACE("\"event\": \"hp_task_finished\", \"start\": %llu, \"last measured\": %llu, \"execution span\": %llu", g_origin, g_last_rdtscl, g_last_rdtscl - g_origin);
	COS_TRACE("\"event\": \"result_cyc\", \"total interference\": %lu, \"execution span\": %llu", perf.total, g_last_rdtscl - g_origin, 0);
	COS_TRACE("\"event\": \"result\", \"total interference_us\": %lu, \"execution span_us\": %llu", time_cyc2usec(perf.total), time_cyc2usec(g_last_rdtscl - g_origin), 0);
#endif

//#define THUNDERING
#ifdef THUNDERING
	printc("\n## SUMMARY ##\n");
	COS_TRACE("\"event\": \"result\", \"wakeup_latency_us\": %lu, \"number of threads\": %d, \"replenishment window size\": %d", time_cyc2usec(g_last_rdtscl - g_thunder_expected_wakeup), g_num_of_threads-1, REPL_WINDOW_SIZE);
#endif

	cos_trace_print_buffer(); 
	
	printc("## TEST MAIN END ##\n");
	
	//Enable trace print in slm
	sched_thd_get_param(cos_thdid(), 1);
	sched_thd_block(0);

	PRINTLOG(PRINT_DEBUG, "⚠⚠ TID: %lu, Should not reach here\n", cos_thdid());
}
