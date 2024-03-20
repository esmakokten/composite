#include <llprint.h>
#include <res_spec.h>
#include <sched.h>
#include <cos_time.h>
#include <initargs.h>
#include <cos_trace.h>
#include <string.h>
#include <cos_trace.h>

#define SL_FPRR_NPRIOS 32

#define LOWEST_PRIORITY (SL_FPRR_NPRIOS - 1)
#define HIGHEST_PRIORITY 1
#define REPL_WINDOW_SIZE 100
// #define COST_OF_YIELD 1900 // for bare metal
#define COST_OF_YIELD 4000 // for KVM

cycles_t g_origin = 0;
cycles_t g_last_rdtscl = 0;
cycles_t g_preemption_lost = 0;
unsigned g_rtdsc_count = 0;

cycles_t blocking_tau = 0;
cycles_t block_for = 0;

volatile bool g_test_finished = false;

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
	sched_thd_block(0);
}

static void
yielding_task()
{
	int repl_buffer_size = 0;
	while (repl_buffer_size < REPL_WINDOW_SIZE){
		++repl_buffer_size;
		sched_thd_yield_to(7);
	}

	// Wait for the test to finish
	while(g_test_finished == false);
	sched_thd_block(0);
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
	cycles_t wake_up = time_now() + block_for - COST_OF_YIELD;
	sched_thd_block_timeout(0, wake_up);
	COS_TRACE("\"event\": \"hp_task_scheduled_wake_up\", \"tid\": %lu, \"time\": %llu", cos_thdid(), wake_up, 0);
	g_last_rdtscl = time_now();

	/* Option 1 
	while (g_test_finished == false) {
		cycles_t now = time_now();
		g_preemption_lost += now - g_last_rdtscl;
		g_rtdsc_count++;
		g_last_rdtscl = now;
	}
	*/
	
	/* Option 2 */
	while (g_test_finished == false) {
		cycles_t now = time_now();
		if (now - g_last_rdtscl > 100) {
			COS_TRACE("\"event\": \"hp_task_preemption_lost\", \"amount\": %llu, \"count\": %u", now - g_last_rdtscl, g_rtdsc_count, 0);
			g_preemption_lost += now - g_last_rdtscl;
			g_rtdsc_count++;
		}
		g_last_rdtscl = now;
	}

	sched_thd_block(0);
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
	SLEEPER = 3, 
	THUNDER = 4,// TODO: Add more types
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

cycles_t pre_thunder_period = 0;
thdid_t
create_thread(const char* args)
{
	char *token;
	struct thread_props thd;

	int result = sscanf(args, "%d,%d,%d,%d,%d,%d", (int*)&thd.type, &thd.priority, &thd.period_us, &thd.budget_us, &thd.execution_us, &thd.block_us);
    
	if (result != 6) {
        PRINTLOG(PRINT_DEBUG, "Parsing failed\n");
        return 1;
    }

	switch (thd.type)
	{
	case SPINNER:
	{
		thd.tid = sched_thd_create(spinning_task, NULL);
		break;
	}
	case YIELDER:
	{
		thd.tid = sched_thd_create(yielding_task, NULL);
		break;
	}
	case ACCOUNTER:
	{
		block_for = time_usec2cyc(thd.block_us);
		// TODO: Add the blocking time as an argument
		thd.tid = sched_thd_create(accounting_task, NULL);
		break;
	}
	case SLEEPER:
	{	
		// Threads that wake up at the same time
		blocking_tau = time_usec2cyc(thd.block_us);
		// TODO: Add the blocking time as an argument
		thd.tid = sched_thd_create(sleeping_task, NULL);
		break;
	}
	case THUNDER:
	{
		if (thd.period_us == 0) {
			thd.period_us = time_cyc2usec(pre_thunder_period);
		}
		// Create yielding tasks with different periods
		thd.tid = sched_thd_create(yielding_task, NULL);
		pre_thunder_period = time_usec2cyc(thd.period_us) - COST_OF_YIELD;

		break;
	}
	/* TODO */
	default:
		assert(0);
		break;
	}

	PRINTLOG(PRINT_DEBUG, "\t TID: %lu, Type: %d, Priority: %d, Period: %d usec(%llu), Budget: %d usec(%llu)\n", thd.tid, thd.type, thd.priority, thd.period_us, time_usec2cyc(thd.period_us), thd.budget_us, time_usec2cyc(thd.budget_us));

	assert(thd.priority >= HIGHEST_PRIORITY && thd.priority <= LOWEST_PRIORITY);
	assert(thd.period_us > thd.budget_us);

	sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_PRIO, thd.priority));
	sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_BUDGET, time_usec2cyc(thd.budget_us)));
	sched_thd_param_set(thd.tid, sched_param_pack(SCHEDP_WINDOW, time_usec2cyc(thd.period_us)));
	
	return thd.tid;
}

int
main(void)
{
	sched_thd_param_set(cos_thdid(), sched_param_pack(SCHEDP_PRIO, HIGHEST_PRIORITY));

	int num_of_threads = 40; // To prevent dynamic allocation, just 
	thdid_t thread_ids[num_of_threads];

	struct initargs params, curr;
	struct initargs_iter i;
	char *token;
	int ret = 0;

	ret = args_get_entry("param", &params);
	assert(!ret);
	assert(args_len(&params) < num_of_threads);

	num_of_threads = args_len(&params);

	int num = 0;
	for (ret = args_iter(&params, &i, &curr) ; ret ; ret = args_iter_next(&i, &curr)) {
		int      keylen;
		char    *thread_args = args_value(&curr);
		assert(thread_args);
		thread_ids[num++] = create_thread(thread_args);
	}

	cycles_t time_before_wakeup = time_now();
	g_origin = time_before_wakeup;
	cycles_t sleep = time_now() + 4990000*4;

	PRINTLOG(PRINT_DEBUG, "Starting %d threads\n", num_of_threads);
	sched_thd_block_timeout(0, sleep);

	// Finish test threads
	g_test_finished = true;
	// Print the all trace buffer
	printc("\n## TEST TRACE ##\n");
	COS_TRACE("\"event\": \"hp_task_interference\", \"amount\": %llu, \"count\": %u", g_preemption_lost, g_rtdsc_count, 0);
	cos_trace_print_buffer(); 
	printc("## TEST TRACE END ##\n");
	cycles_t wakeup = time_now();
	cycles_t spent = wakeup - time_before_wakeup;
	printc("Time spent: %llu	\n", time_cyc2usec(spent));
	printc("hp_task_interference: %llu, rtdsc count: %u\n", g_preemption_lost, g_rtdsc_count);
	// Print the preemption lost
	
	/*
	cycles_t sched_exec_time, idle_exec_time = 0;
	sched_exec_time = (cycles_t)sched_thd_get_param(1, 0);
	idle_exec_time = (cycles_t)sched_thd_get_param(0, 0);
	*/

	// Print the total execution time of the tasks
	u16_t	 switch_cnt = 0;
	for(int i = 0; i < num_of_threads; i++) {
		cycles_t exec_time = 0;
		exec_time = (cycles_t)sched_thd_get_param(thread_ids[i], 0);
		switch_cnt = (u16_t)sched_thd_get_param(thread_ids[i], 1);
		printc("Thdid: %lu Total exec time: %llu, Switch count: %u\n", thread_ids[i], exec_time, switch_cnt);
	}
	// Is this a bug? @FIXME: assert cos_component.c:378
	// Wait for 5 seconds
	// while((time_now() - wakeup) < time_usec2cyc(5000000));
	//sched_thd_block_timeout(0, time_now() + time_usec2cyc(90000000));

	printc("Test Finished ####\n");	
	sched_thd_block(0);
}
