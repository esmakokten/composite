#include <fpres.h>

#define SLM_FPRR_NPRIOS         32
#define SLM_FPRR_PRIO_HIGHEST   TCAP_PRIO_MAX
#define SLM_FPRR_PRIO_LOWEST    (SLM_FPRR_NPRIOS - 1)

#define SLM_WINDOW_HIGHEST   1000000000000 //cycles
#define SLM_WINDOW_LOWEST    1000     //cycles


struct prioqueue {
	struct ps_list_head prio[SLM_FPRR_NPRIOS];
} CACHE_ALIGNED;
struct prioqueue run_queue[NUM_CPU];

static int
__slm_timeout_compare_min(void *a, void *b)
{
	/* FIXME: logic for wraparound in either timeout_cycs */
	return slm_thd_timer_policy((struct slm_thd *)a)->abs_next_processing <= slm_thd_timer_policy((struct slm_thd *)b)->abs_next_processing;
}

static void
__slm_timeout_update_idx(void *e, int pos)
{ slm_thd_timer_policy((struct slm_thd *)e)->timeout_idx = pos; }

DECLARE_HEAP(timer, __slm_timeout_compare_min, __slm_timeout_update_idx);

struct timer_global {
	struct heap    h;
	void           *data[MAX_NUM_THREADS];
} CACHE_ALIGNED;
static struct timer_global __timer_globals[NUM_CPU][SLM_FPRR_NPRIOS];
static cycles_t g_current_timeout;
static thdid_t g_prev_tid;

static inline struct timer_global *
timer_global_prio(int prio) {
	return &__timer_globals[cos_coreid()][prio];
}


struct slm_timer_thd *
process_prio_timer(int prio, cycles_t now)
{
	struct timer_global *g = timer_global_prio(prio);
	
	if (heap_size(&g->h) > 0) {

		struct slm_thd *tp, *th;
		struct slm_timer_thd *tt;
		struct slm_sched_thd *st;

		tp = heap_peek(&g->h);
		assert(tp);
		tt = slm_thd_timer_policy(tp);
		st = slm_thd_sched_policy(tp);
		assert(tt && tt->timeout_idx > 0);

		/* No more threads to wake! */
		if (cycles_greater_than(tt->abs_next_processing, now)) {
			return tt;
		}

		/* Dequeue thread with closest wakeup */
		th = timer_heap_highest(&g->h);
		assert(th == tp);

		tt->timeout_idx = -1;

		// Check the state
		switch(st->state) {
			case STATE_EXPENDED:
			{
				/* A thread, in its new period with no budget, wants to replenish */

				replenish(th, now);
				if(tt->budget > 0) {
					/* Update the state */
					st->state = STATE_READY;
					/* Add to the runqueue */
					ps_list_head_append_d(&run_queue[cos_cpuid()].prio[tp->priority], st);
				}

				/* Thread can only be in expended state if it is budgeted */
				assert(tt->is_budgeted);
				assert(tt->timeout_idx == -1);
				if(st->repl_count > 0) {
					slm_timer_fpres_add(th, st->replenishments[st->repl_head_idx].replenish_time_abs);
				}

				break;
			}
			case STATE_READY:
			{
				/* A thread, waiting for the execution in ready state */
				/* There could be some threads not yet switch to STATE RUNNIG but timer fires for another replenishment */
				
				// Optimization: Since it is already in the runqueue and wait for the execution, 
				// We can wait for the scheduling and replenish just before it is scheduled

				assert(tt->is_budgeted);
				replenish(th, now);

				assert(tt->timeout_idx == -1);
				if(st->repl_count > 0) {
					slm_timer_fpres_add(th, st->replenishments[st->repl_head_idx].replenish_time_abs);
				}	

				break;
			}
			case STATE_BLOCKED:
			{
				/* A thread, blocked by the user before, wants to wake up */
				slm_thd_wakeup(th, 1);
				/* We dont allow wake up timers to fire when thread has no budget*/
				assert(!tt->is_budgeted || st->state == STATE_READY);
				break;
			}
			case STATE_BLOCKED_PERIODIC:
			{
				/* A thread, blocked in the previous period wants to wake up in its next period */
				/* TODO: Not throughly tested or used, remove this comment after verification */
				slm_sched_fpres_wakeup_periodic(th, now);
				break;
			}
			case STATE_RUNNING:
			{
				/* A thread in the runqueue, executed in its current period wants to replenish */
								
				// Optimization: For the budgeted threads that still have budget(in the runqueue), 
				// We can replenish just before it is scheduled

				assert(tt->is_budgeted);
				replenish(th, now);

				/* Add the next replenishment timer */
				assert(tt->timeout_idx == -1);
				if(st->repl_count > 0)
					slm_timer_fpres_add(th, st->replenishments[st->repl_head_idx].replenish_time_abs);
				
				break;
			}
			default:
				break;
		}
	}

	// Find the next thread to process
	if (heap_size(&g->h) > 0) {
		return slm_thd_timer_policy(heap_peek(&g->h));
	}
	return NULL;

}

/* The timer expired */
void
slm_timer_fpres_expire(cycles_t now)
{
	// TODO

}

/*
 * Timeout and wakeup functionality
 *
 * TODO: Replace the in-place heap with a rb-tree to avoid external, static allocation.
 */

int
slm_timer_fpres_add(struct slm_thd *t, cycles_t absolute_timeout) 
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	struct slm_sched_thd *st = slm_thd_sched_policy(t);	
	struct timer_global *g = timer_global_prio(t->priority);

	assert(tt && tt->timeout_idx == -1);
	assert(heap_size(&g->h) < MAX_NUM_THREADS);

	tt->abs_next_processing = absolute_timeout;
	timer_heap_add(&g->h, t);

	//COS_TRACE("\"event\":\"add-timer\", \"tid\":%ld, \"time\":%llu", t->tid, tt->abs_next_processing, 0);
 
	return 0;
}

int
slm_timer_fpres_cancel(struct slm_thd *t)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	struct timer_global *g   = timer_global_prio(t->priority);

	if (tt->timeout_idx == -1) return 0;

	assert(heap_size(&g->h));
	assert(tt->timeout_idx > 0);

	timer_heap_remove(&g->h, tt->timeout_idx);
	tt->timeout_idx = -1;

	return 0;
}

int
slm_timer_fpres_thd_init(struct slm_thd *t)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	*tt = (struct slm_timer_thd){
		.timeout_idx = -1,
		.abs_next_processing  = 0,
		.abs_period_start = slm_now(),
		.abs_period_end = tt->abs_period_start + SLM_WINDOW_HIGHEST,
		.budget      = 0,
		.initial_budget = 0,
		.is_budgeted = 0,
		.period      = SLM_WINDOW_HIGHEST,
	};

	//COS_TRACE("\"event\":\"init\", \"tid\":%ld, \"period\":%llu, \"period-start\":%llu", t->tid, tt->period, tt->abs_period_start);
	// TODO: Check if the thread has higher priority than the current thread?
	// Add timer interrupt if necessary?

	return 0;
}

void
slm_timer_fpres_thd_deinit(struct slm_thd *t)
{
	// Cancel the timers
	slm_timer_fpres_cancel(t);
	return;
}


static void
slm_policy_timer_init(microsec_t period)
{
	struct timer_global *g = NULL;
	cycles_t next_timeout;

	// Initialize the global timer for each priority
	for (int i = 0; i < SLM_FPRR_NPRIOS; i++) {

		struct timer_global *g = timer_global_prio(i);

		memset(g, 0, sizeof(struct timer_global));
		heap_init(&g->h, MAX_NUM_THREADS);
	}

	next_timeout = slm_now();
	g_current_timeout = next_timeout;
	g_prev_tid = 0;
	slm_timeout_set(next_timeout);
}

int
slm_timer_fpres_init(void)
{
	/* 10ms */
	slm_policy_timer_init(10000);

	return 0;
}

void
slm_sched_fpres_execution(struct slm_thd *t, cycles_t cycles, cycles_t now)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	struct slm_sched_thd *st = slm_thd_sched_policy(t);

	assert(st->state != STATE_EXPENDED);

	if(tt->is_budgeted == 0) {
		return;
	}

	tt->budget -= cycles;
	// TODO: In case of budget overrun, decide what to do

	// Are period_start and period_end correct?
	// assert(tt->abs_period_start <= now);

	// TODO: Decide where is correct
	// If needed sync abs_period_end and abs_period_start
	if (tt->abs_period_end < now) {

		tt->abs_period_start += tt->period;
		tt->abs_period_end = tt->abs_period_start + tt->period;
	}

	// Did we miss the deadline? 
	// assert(tt->abs_period_end >= now + remaining WCET);
	
	// TODO: Check budget overruns ex. sporadic server paper
	g_prev_tid = t->tid;
	cycles_t replenish_time = now + tt->period - cycles;
	// TODO: Check if the new replenishment can be added to the last replenishment

	int repl_next_idx = (st->repl_tail_idx + 1) == SLM_FPRES_REPL_WINDOW_SIZE ? 0 : st->repl_tail_idx + 1;
	int repl_prev_idx = (st->repl_head_idx - 1) == -1 ? SLM_FPRES_REPL_WINDOW_SIZE - 1 : st->repl_head_idx - 1;
	// Check if the replenishment window is full
	if (st->repl_count == SLM_FPRES_REPL_WINDOW_SIZE) { 
		// Merge the last replenishment with the new replenishment
		st->replenishments[repl_prev_idx].replenish_amount += cycles;
		st->replenishments[repl_prev_idx].replenish_time_abs = replenish_time;
	}
	else {
		// Add a new replenishment
		st->replenishments[st->repl_tail_idx].replenish_amount = cycles;
		st->replenishments[st->repl_tail_idx].replenish_time_abs = replenish_time;
		st->repl_tail_idx = repl_next_idx;
		st->repl_count++;
	}

	//Print all replenishments
	/*
	int idx = st->repl_head_idx;
	for (int i = 0; i < st->repl_count; i++) {
		//COS_TRACE("-Replenisment: TID: %ld, Replenish Time: %llu, Replenish Amount: %llu\n", t->tid, st->replenishments[idx].replenish_time_abs, st->replenishments[idx].replenish_amount);
		idx = (idx + 1) == SLM_FPRES_REPL_WINDOW_SIZE ? 0 : idx + 1;
	}
	*/
	//COS_TRACE("Replenisment: TID: %ld, Replenish Time: %llu, Replenish Amount: %llu\n", t->tid, replenish_time, cycles);

	// if budget is 0, add timer and block	
	if (tt->budget <= 0) {
		expended(t);
	}

	if (st->state == STATE_BLOCKED || st->state == STATE_BLOCKED_PERIODIC) {
		// Skip adding the timer for blocked threads	
		// COS_TRACE("\"event\":\"#ASSERT#\", \"tid\":%ld, \"state\":%d", t->tid, st->state, 0);
		// assert(0);
		return;
	}


	// Add the earliest replenishment timer if not already added
	if (tt->timeout_idx == -1) {
		// There should be a timer for blocked threads
		assert(st->state != STATE_BLOCKED || st->state != STATE_BLOCKED_PERIODIC);
		slm_timer_fpres_add(t, st->replenishments[st->repl_head_idx].replenish_time_abs);
	}

	return; 
}

static void
set_next_timer_interrupt(struct slm_thd *t, cycles_t now, cycles_t closest_prior_timer)
{

#ifdef MEASURE_BATCH_REPLENISHMENT
	slm_timeout_set(9999999999999999);
#else
	/* Get the closest higher or equal priority timer */
	cycles_t next_timeout = closest_prior_timer; 	


	/* Check if the next timeout is further than the budget of the current thread */
	if(t != NULL) { 
		struct slm_timer_thd *curr = slm_thd_timer_policy(t);
		if (curr->is_budgeted){
			assert(curr->budget >= 0);
			// TODO: a WCET check can be done here

#define NON_PREEMPTIVE_CHUNK_SUPPORT
#ifdef NON_PREEMPTIVE_CHUNK_SUPPORT
			// Check if the thread has a non-preemptive chunk
			if ((curr->non_preemptive_chunk + now) && next_timeout 
				&& cycles_greater_than(curr->non_preemptive_chunk + now, next_timeout)) {
				// If the non-preemptive chunk is greater than the next timeout, 
				// the next timeout should be the non-preemptive chunk
				next_timeout = (cycles_t)curr->non_preemptive_chunk + now;
			}
#endif //NON_PREEMPTIVE_CHUNK_SUPPORT

			// Comment : 2 cmov instructions may be cheaper than a branch misprediction?
			next_timeout = (next_timeout == 0) ? ((cycles_t)curr->budget + now) : next_timeout;
			next_timeout = (next_timeout > ((cycles_t)curr->budget + now)) ? ((cycles_t)curr->budget + now) : next_timeout;
		} 
	}

//#define TIMER_OVERHEAD_TEST
#ifdef TIMER_OVERHEAD_TEST
	// TODO: Added for the timer overhead test
	slm_timeout_set(9999999999999999);
	if(t != NULL) { 
		struct slm_timer_thd *curr = slm_thd_timer_policy(t);
		if (curr->is_budgeted) {
			cycles_t offset = now % 10000; // 5ms
			slm_timeout_set(now - offset + 10000);  // 5ms
		}
	} 
	else {
		/* Set the next timeout */
		if (next_timeout != 0) {
			g_current_timeout = next_timeout;
			slm_timeout_set(next_timeout);
		}
	}
#else	
	// TODO: Hacked because even clearing timeout, it continues to interrupt
	// slm_timeout_clear();
	slm_timeout_set(9999999999999999);

	/* Set the next timeout */
	if (next_timeout != 0) {
		g_current_timeout = next_timeout;
		slm_timeout_set(next_timeout);
	}

	//COS_TRACE("Next Timeout: %llu", next_timeout, 0, 0);
#endif
#endif //MEASURE_BATCH_REPLENISHMENT

}


#ifdef MEASURE_BATCH_REPLENISHMENT
void
fill_replenishments(struct slm_sched_thd* st, int count, cycles_t now)
{
	// Fill the replenishments for the thread
	cycles_t replenish_time = now;
	cycles_t cycles = 123456;

	while (st->repl_count < count)
	{
		int repl_next_idx = (st->repl_tail_idx + 1) == SLM_FPRES_REPL_WINDOW_SIZE ? 0 : st->repl_tail_idx + 1;
		int repl_prev_idx = (st->repl_head_idx - 1) == -1 ? SLM_FPRES_REPL_WINDOW_SIZE - 1 : st->repl_head_idx - 1;
		// Check if the replenishment window is full
		if (st->repl_count == SLM_FPRES_REPL_WINDOW_SIZE) { 
			// Merge the last replenishment with the new replenishment
			st->replenishments[repl_prev_idx].replenish_amount += cycles;
			st->replenishments[repl_prev_idx].replenish_time_abs = replenish_time;
			//TODO: Just for measurements
			break;
		}
		else {
			// Add a new replenishment
			st->replenishments[st->repl_tail_idx].replenish_amount = cycles;
			st->replenishments[st->repl_tail_idx].replenish_time_abs = replenish_time;
			st->repl_tail_idx = repl_next_idx;
			st->repl_count++;
		}
	}	
}

#endif //MEASURE_BATCH_REPLENISHMENT

struct slm_thd *
slm_sched_fpres_schedule(cycles_t now)
{
	int i;
	struct slm_sched_thd *st;
	struct slm_timer_thd *tt;
	struct ps_list_head *prios = run_queue[cos_cpuid()].prio;
	cycles_t prev_closest_prior_timer = 0;
	cycles_t closest_prior_timer = 0;
	struct slm_timer_thd *next_processing_thd = NULL;

	for (i = 0 ; i < SLM_FPRR_NPRIOS ; i++) {

		prev_closest_prior_timer = closest_prior_timer;
		/* Process the higher priority timers */
		// TODO change it so that it processes only one timer
		next_processing_thd = process_prio_timer(i, now);
		if (next_processing_thd != NULL) {
			if (closest_prior_timer == 0 || (next_processing_thd->abs_next_processing!= 0 && cycles_greater_than(closest_prior_timer, next_processing_thd->abs_next_processing))) {
				closest_prior_timer = next_processing_thd->abs_next_processing;
			}
		}
	
		if (ps_list_head_empty(&prios[i])) {
			continue;
		}
		st = ps_list_head_first_d(&prios[i], struct slm_sched_thd);
		tt = slm_thd_timer_policy(slm_thd_from_sched(st));

		/*
		 * We want to move the selected thread to the back of the list.
		 * Otherwise it won't be truly round robin 
		 */

		/* Threads with no budget should not be in the runqueue */	
		assert(st->state != STATE_EXPENDED);
		assert(!tt->is_budgeted || tt->budget > 0);

		ps_list_rem_d(st);
		ps_list_head_append_d(&prios[i], st);

		// TODO: Workaround
     	// Check if the thread same as the previous one and is budgeted
		if (g_prev_tid == slm_thd_from_sched(st)->tid && tt->is_budgeted) {

			// NOTE: Every schedule() should do a context switch
			// Every timer is meaningful for the next execution
			// COS_TRACE("####### Assert ### No context switch for this timer tid: %ld", slm_thd_from_sched(st)->tid, 0, 0); 
			//printc("####### Assert ### No context switch for this timer tid: %ld\n", slm_thd_from_sched(st)->tid, 0, 0);
			//assert(0);
		}
#ifdef MEASURE_BATCH_REPLENISHMENT
		// Fill the replenishments for the thread
		fill_replenishments(st, 1, now);
		unsigned cycles_high, cycles_low, cycles_high1, cycles_low1;

		__asm__ __volatile__("cpuid\n\t" 
							"rdtsc\n\t" 
							"mov %%edx, %0\n\t" 
							"mov %%eax, %1\n\t" : 
							"=r" (cycles_high), "=r" (cycles_low) :: "%rax", "%rbx", "%rcx", "%rdx");

		early_replenish(slm_thd_from_sched(st), now);

		__asm__ __volatile__("rdtscp\n\t" 
							"mov %%edx, %0\n\t" 
							"mov %%eax, %1\n\t" 
							"cpuid\n\t" : 
							"=r" (cycles_high1), "=r" (cycles_low1) :: "%rax", "%rbx", "%rcx", "%rdx");


		cycles_t start = (((cycles_t)cycles_high << 32) | cycles_low);
		cycles_t end = (((cycles_t)cycles_high1 << 32) | cycles_low1);

		if(iter_batch_processing < ITER) {
			perfdata_add(&perf_batch_repl, end - start);
			++iter_batch_processing;
		}

#else
#define EAGER_REPLENISHMENT
#ifdef EAGER_REPLENISHMENT
		/* Do future replenishments that budget allows 
		budget_t previous_budget;
		do {
			previous_budget = tt->budget;
			replenish(slm_thd_from_sched(st), now + tt->budget);
		} while (previous_budget != tt->budget);
		*/
		early_replenish(slm_thd_from_sched(st), now);
		//XXX !!! Important !!! We dont cancel the timer for the replenishments??
		/* 	
		  We did all possible furure replenishments at this point 
		  so we have only one timer for the current thread which is now + budget
		*/
#endif //EAGER_REPLENISHMENT
#endif //MEASURE_BATCH_REPLENISHMENT
		/* Set the timer */
		/* prev_closest_premaining_budgetrior_timer is the closest timer of the higher priority, 0 if no timer */
		set_next_timer_interrupt(slm_thd_from_sched(st), now, prev_closest_prior_timer);

		st->state = STATE_RUNNING;	
		//COS_TRACE("\"event\":\"schedule\", \"tid\":%ld, \"next-timeout\":%llu", slm_thd_from_sched(st)->tid, g_current_timeout, 0);
		return slm_thd_from_sched(st);
	}

	set_next_timer_interrupt(NULL, now, closest_prior_timer);
	//COS_TRACE("\"event\":\"schedule\", \"tid\":0, \"next-timeout\":%llu", g_current_timeout, 0, 0);
	g_prev_tid = 0; // TODO: For idle thread
	return NULL;
}

int
slm_sched_fpres_block(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	assert(st->state != STATE_BLOCKED);
	assert(st->state != STATE_BLOCKED_PERIODIC);

	//COS_TRACE("\"event\":\"block\", \"tid\":%ld, \"wake-time\":%llu", t->tid, tt->abs_next_processing, 0);

	/* Remove from runqueue */
	ps_list_rem_d(st); 
	st->state = STATE_BLOCKED;

	/* Cancel the timer */
	// TODO: Now cancelling the timer is in sched/main.c should we move it here?

	return 0;
}

/* TODO: Not throughly tested or used, remove this comment after verification */
int
slm_sched_fpres_block_periodic(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);


	assert(tt->is_budgeted);
	assert(tt->abs_next_processing >= tt->abs_period_end);

	assert(st->state != STATE_BLOCKED);
	assert(st->state != STATE_BLOCKED_PERIODIC);

	/* Remove from runqueue */
	st->state = STATE_BLOCKED_PERIODIC;
	ps_list_rem_d(st);

	/* Update abs_period_start, abs_period_end and abs_next_processing */
	tt->abs_period_start = tt->abs_period_end;
	tt->abs_period_end = tt->abs_period_start + tt->period;

	// Optimization: Set the next processing time to the first replenishment time
	// TODO: Temporary for deferrable server 
	assert(tt->abs_next_processing == tt->abs_period_start);

	return 0;
}

static void
expended(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	if (st->state == STATE_BLOCKED || st->state == STATE_BLOCKED_PERIODIC) {
		// Thread exausted budget before blocked
		// Check if the wake up time is closer than first replenishment
		if (tt->timeout_idx != -1 && tt->abs_next_processing < st->replenishments[st->repl_head_idx].replenish_time_abs) {
			// Shift the wake up timer to the first replenishment
			slm_timer_fpres_cancel(t);
			slm_timer_fpres_add(t, st->replenishments[st->repl_head_idx].replenish_time_abs);
		}
	}

	// Remove from runqueue, note that slm_state is still RUNNING
	ps_list_rem_d(st);
	st->state = STATE_EXPENDED;
		
	// Update abs_period_start, abs_period_end
	tt->abs_period_start = tt->abs_period_start + tt->period;
	tt->abs_period_end = tt->abs_period_start + tt->period;

	//COS_TRACE("expended(): TID: %ld Period Start, End: %llu, %llu", t->tid, tt->abs_period_start, tt->abs_period_end);
}

static void
replenish(struct slm_thd *t, cycles_t now)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	int ret = -1;

	//TODO: Any check here?

	// Do the replenishments until the current time
	int elem_count = st->repl_count;
	for(int i = 0; i < elem_count; i++) {
		if (now >= st->replenishments[st->repl_head_idx].replenish_time_abs) {
#define DISABLE_REPLENISHMENT
#ifdef DISABLE_REPLENISHMENT
#else
			tt->budget += st->replenishments[st->repl_head_idx].replenish_amount;
#endif
			//COS_TRACE("\"event\":\"replenish\", \"tid\":%ld, \"amount\":%llu, \"now\":%llu", t->tid, st->replenishments[st->repl_head_idx].replenish_amount, now);
			st->repl_head_idx = (st->repl_head_idx + 1) == SLM_FPRES_REPL_WINDOW_SIZE ? 0 : st->repl_head_idx + 1;
			st->repl_count--;
		}
		else {
			break;
		}
	}
}

static void
early_replenish(struct slm_thd *t, cycles_t now)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	int ret = -1;
	//TODO: Any check here?
	cycles_t up_to = now + tt->budget;
	
	// Do the replenishments until the current time
	int elem_count = st->repl_count;
	for(int i = 0; i < elem_count; i++) {
		if (up_to >= st->replenishments[st->repl_head_idx].replenish_time_abs) {
			tt->budget += st->replenishments[st->repl_head_idx].replenish_amount;
			up_to += st->replenishments[st->repl_head_idx].replenish_amount;
			//COS_TRACE("\"event\":\"early_replenish\", \"tid\":%ld, \"amount\":%llu, \"now\":%llu", t->tid, st->replenishments[st->repl_head_idx].replenish_amount, now);
			st->repl_head_idx = (st->repl_head_idx + 1) == SLM_FPRES_REPL_WINDOW_SIZE ? 0 : st->repl_head_idx + 1;
			st->repl_count--;
		}
		else {
			break;
		}
	}
}

int
slm_sched_fpres_wakeup(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	assert(ps_list_singleton_d(st));
	assert(st->state == STATE_BLOCKED);
	
	assert(tt->timeout_idx == -1);

	//COS_TRACE("\"event\":\"wakeup\", \"tid\":%ld", t->tid, 0, 0);
	
	if (tt->is_budgeted) {
		/* Shift abs_period_start, abs_period_end 
		 This prevents a thread from gaining advantage over other same priority 
		*/
		/*int periods_passed = (now - tt->abs_period_start) / tt->period;
		
		if (periods_passed > 0) {
			//COS_TRACE("\"event\":\"wakeup\", \"tid\":%ld, \"periods-passed\":%d", t->tid, periods_passed, 0);
			tt->abs_period_start += (periods_passed * tt->period);
			tt->abs_period_end = tt->abs_period_start + tt->period;
		}
		Replenish the thread */
		//replenish(t, now);
		if(tt->budget > 0) {
			/* Update the state */
			st->state = STATE_READY;
			/* Add to the runqueue */
			//Add the cancelled timer in slm_sched_fpres_block() 
			ps_list_head_append_d(&run_queue[cos_cpuid()].prio[t->priority], st);		
			if(st->repl_count > 0) {
				slm_timer_fpres_add(t, st->replenishments[st->repl_head_idx].replenish_time_abs);
			}
			return 0;
		} else {
			assert(st->repl_count > 0);
			slm_timer_fpres_add(t, st->replenishments[st->repl_head_idx].replenish_time_abs);
			st->state = STATE_EXPENDED;
			return 0;
		}
	}
	// If there is no budget change state and return
	if (!tt->is_budgeted) {
		/* Add to the runqueue */
		st->state = STATE_READY;
		ps_list_head_append_d(&run_queue[cos_cpuid()].prio[t->priority], st);
	}
	return 0;
}


/* TODO: Not throughly tested or used, remove this comment after verification */
int
slm_sched_fpres_wakeup_periodic(struct slm_thd *t, cycles_t now)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	assert(ps_list_singleton_d(st));
	assert(st->state == STATE_BLOCKED_PERIODIC);

	assert(now < tt->abs_period_end);
	replenish(t, now);

	/* Add to the runqueue */
	st->state = STATE_READY;
	ps_list_head_append_d(&run_queue[cos_cpuid()].prio[t->priority], st);

	return 0;
}

void
slm_sched_fpres_yield(struct slm_thd *t, struct slm_thd *yield_to)
{
	// TODO: Not implemented yet
	
	// Do nothing for the same priority threads it will 
	// COS_TRACE("\"event\":\"yield\", \"tid\":%ld, \"yield-to\":%ld", t->tid, yield_to->tid, 0);
}

int
slm_sched_fpres_thd_init(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);

	t->priority = SLM_FPRR_PRIO_LOWEST;
	st->state = STATE_READY;

	// Initialize the replenishment window 
	st->repl_head_idx = 0;
	st->repl_tail_idx = 0;
	for (int i = 0; i < SLM_FPRES_REPL_WINDOW_SIZE; i++) {
	 	st->replenishments[i].replenish_time_abs = 0;
	 	st->replenishments[i].replenish_amount = 0;
	}

	ps_list_init_d(st);

	return 0;
}

void
slm_sched_fpres_thd_deinit(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);

	// Remove from runqueue
	st->state = STATE_DEINIT;
	ps_list_rem_d(slm_thd_sched_policy(t));
}

static void
update_queue(struct slm_thd *t, tcap_prio_t prio)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);

	/* For index to start from 0 */
	t->priority = prio - 1;
	ps_list_rem_d(st); /* if we're already on a list, and we're updating priority */
	ps_list_head_append_d(&run_queue[cos_cpuid()].prio[t->priority], st);

	//COS_TRACE("\"event\":\"update-priority\", \"tid\":%ld, \"priority\":%d", t->tid, t->priority, 0);

	return;
}

static void
update_period(struct slm_thd *t, cycles_t period)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	// TODO unsigned int is used so expect period to be given in us
	tt->period = slm_usec2cyc(period);
	cycles_t offset = tt->abs_period_start % tt->period;
	tt->abs_period_start = tt->abs_period_start - offset;
	tt->abs_period_end = tt->abs_period_start + period;

	//COS_TRACE("\"event\":\"update-period\", \"tid\":%ld, \"period\":%llu", t->tid, tt->period, 0);
	return;
}

static void
update_budget(struct slm_thd *t, budget_t budget)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	// TODO unsigned int is used so expect budget to be given in us
	tt->budget = slm_usec2cyc(budget);
	tt->initial_budget = budget;
	tt->is_budgeted = 1;

	//COS_TRACE("\"event\":\"update-budget\", \"tid\":%ld, \"budget\":%lld", t->tid, tt->budget, 0);

	return;
}

int
slm_sched_fpres_thd_update(struct slm_thd *t, sched_param_type_t type, unsigned int v)
{

	switch (type) {
	case SCHEDP_INIT_PROTO:
	{
		update_queue(t, 0);

		return 0;
	}
	case SCHEDP_INIT:
	{
		update_queue(t, SLM_FPRR_PRIO_LOWEST);

		return 0;
	}
	case SCHEDP_PRIO:
	{
		assert(v >= SLM_FPRR_PRIO_HIGHEST && v <= SLM_FPRR_PRIO_LOWEST);
		update_queue(t, v);

		return 0;
	}
	case SCHEDP_BUDGET:
	{
		update_budget(t, v);

		return 0;
	}
	case SCHEDP_WINDOW:
	{
		assert(v <= SLM_WINDOW_HIGHEST && v >= SLM_WINDOW_LOWEST);
		update_period(t, v);

		return 0;
	}
	case SCHEDP_NONPREEMPT:
	{
		struct slm_timer_thd *tt = slm_thd_timer_policy(t);
		tt->non_preemptive_chunk = slm_usec2cyc(v);

		return 0;
	}
	default:
		return -1;
	}
}

void
slm_sched_fpres_init(void)
{
	int i;

	for (i = 0 ; i < SLM_FPRR_NPRIOS ; i++) {
		ps_list_head_init(&run_queue[cos_cpuid()].prio[i]);
	}

	// TODO: Added for test purposes
	printc("### SLM FPRES Scheduler Initialized ###\n");
}
