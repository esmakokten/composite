#include <fpss.h>

#define SLM_FPRR_NPRIOS         32
#define SLM_FPRR_PRIO_HIGHEST   TCAP_PRIO_MAX
#define SLM_FPRR_PRIO_LOWEST    (SLM_FPRR_NPRIOS - 1)

#define SLM_WINDOW_HIGHEST   1000000000000 //cycles
#define SLM_WINDOW_LOWEST    1000     //cycles


struct prioqueue {
	struct ps_list_head prio[SLM_FPRR_NPRIOS];
} CACHE_ALIGNED;
struct prioqueue run_queue[NUM_CPU];

struct timer_global {
	struct heap    h;
	void           *data[MAX_NUM_THREADS];
	cycles_t       current_timeout;
	thdid_t		   previous_tid;
} CACHE_ALIGNED;
static struct timer_global __timer_globals[NUM_CPU];

static inline struct timer_global *
timer_global(void) {
	return &__timer_globals[cos_coreid()];
}

/* The timer expired */
void
slm_timer_fpss_expire(cycles_t now)
{
	struct timer_global *g = timer_global();
	g->current_timeout = now;

	/* Should we wake up the closest-timeout thread? */
	while (heap_size(&g->h) > 0) {

		struct slm_thd *tp, *th;
		struct slm_timer_thd *tt;
		struct slm_sched_thd *st;
		/* Should we wake up the closest-timeout thread? */
		tp = heap_peek(&g->h);
		assert(tp);
		tt = slm_thd_timer_policy(tp);
		st = slm_thd_sched_policy(tp);
		assert(tt && tt->timeout_idx > 0);

		/* No more threads to wake! */
		if (cycles_greater_than(tt->abs_next_processing, now)) break;
		
		/* Dequeue thread with closest wakeup */
		th = heap_highest(&g->h);
		assert(th == tp);

		tt->timeout_idx = -1;

		// Check the state
		switch(st->state) {
			case STATE_EXPENDED:
			{
				/* A thread, in its new period with no budget, wants to replenish */

				replenish(th, now);
				/* Add to the runqueue back if it has budget > 0 */
				if (tt->budget > 0)
				{
					/* Update the state */
					st->state = STATE_READY;
					ps_list_head_append_d(&run_queue[cos_cpuid()].prio[tp->priority], st);
				}

				/* Thread can only be in expended state if it is budgeted */
				assert(tt->is_budgeted);
				assert(tt->timeout_idx == -1);
				if(st->repl_count > 0) {
					slm_timer_fpss_add(th, st->replenishments[st->repl_head_idx].replenish_time_abs);
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
					slm_timer_fpss_add(th, st->replenishments[st->repl_head_idx].replenish_time_abs);
				}	

				break;
			}
			case STATE_BLOCKED:
			{
				/* A thread, blocked by the user before, wants to wake up */
				slm_thd_wakeup(th, 1);
				break;
			}
			case STATE_BLOCKED_PERIODIC:
			{
				/* A thread, blocked in the previous period wants to wake up in its next period */
				/* TODO: Not throughly tested or used, remove this comment after verification */
				slm_sched_fpss_wakeup_periodic(th, now);
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
					slm_timer_fpss_add(th, st->replenishments[st->repl_head_idx].replenish_time_abs);
				
				break;
			}
			default:
				break;
		}
	}

}

/*
 * Timeout and wakeup functionality
 *
 * TODO: Replace the in-place heap with a rb-tree to avoid external, static allocation.
 */

int
slm_timer_fpss_add(struct slm_thd *t, cycles_t absolute_timeout) 
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	struct timer_global *g = timer_global();

	assert(tt && tt->timeout_idx == -1);
	assert(heap_size(&g->h) < MAX_NUM_THREADS);

	tt->abs_next_processing = absolute_timeout;
	heap_add(&g->h, t);

	//COS_TRACE("\"event\":\"add-timer\", \"tid\":%ld, \"time\":%llu", t->tid, tt->abs_next_processing, 0);
 
	return 0;
}

int
slm_timer_fpss_cancel(struct slm_thd *t)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	struct timer_global *g   = timer_global();

	if (tt->timeout_idx == -1) return 0;

	assert(heap_size(&g->h));
	assert(tt->timeout_idx > 0);

	heap_remove(&g->h, tt->timeout_idx);
	tt->timeout_idx = -1;

	return 0;
}

int
slm_timer_fpss_thd_init(struct slm_thd *t)
{
	struct timer_global *g = timer_global();
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

	COS_TRACE("\"event\":\"init\", \"tid\":%ld, \"period\":%llu, \"period-start\":%llu", t->tid, tt->period, tt->abs_period_start);
	// TODO: Check if the thread has higher priority than the current thread?
	// Add timer interrupt if necessary?

	return 0;
}

void
slm_timer_fpss_thd_deinit(struct slm_thd *t)
{
	// Cancel the timers
	slm_timer_fpss_cancel(t);
	return;
}

static int
__slm_timeout_compare_min(void *a, void *b)
{
	/* FIXME: logic for wraparound in either timeout_cycs */
	return slm_thd_timer_policy((struct slm_thd *)a)->abs_next_processing <= slm_thd_timer_policy((struct slm_thd *)b)->abs_next_processing;
}

static void
__slm_timeout_update_idx(void *e, int pos)
{ slm_thd_timer_policy((struct slm_thd *)e)->timeout_idx = pos; }

static void
slm_policy_timer_init(microsec_t period)
{
	struct timer_global *g = timer_global();
	cycles_t next_timeout;

	memset(g, 0, sizeof(struct timer_global));
	heap_init(&g->h, MAX_NUM_THREADS, __slm_timeout_compare_min, __slm_timeout_update_idx);

	next_timeout = slm_now();
	g->current_timeout = next_timeout;
	g->previous_tid = 0;
	slm_timeout_set(next_timeout);
}

int
slm_timer_fpss_init(void)
{
	/* 10ms */
	slm_policy_timer_init(10000);

	return 0;
}

void
slm_sched_fpss_execution(struct slm_thd *t, cycles_t cycles, cycles_t now)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct timer_global *g = timer_global();

	assert(st->state != STATE_EXPENDED);

	if(tt->is_budgeted == 0) {
		return;
	}

	tt->budget -= cycles;

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
	cycles_t replenish_time = now + tt->period - cycles;
	g->previous_tid = t->tid;

	int repl_next_idx = (st->repl_tail_idx + 1) == SLM_FPRES_REPL_WINDOW_SIZE ? 0 : st->repl_tail_idx + 1;
	int repl_prev_idx = (st->repl_head_idx - 1) == -1 ? SLM_FPRES_REPL_WINDOW_SIZE - 1 : st->repl_head_idx - 1;
	// Check if the replenishment window is full
	if (st->repl_count == SLM_FPRES_REPL_WINDOW_SIZE) { 
		// COS_TRACE("\"event\":\"repl-full\", \"tid\":%ld, \"repl-count\":%d", t->tid, st->repl_count, 0);
		// Merge the last replenishment with the new replenishment
		st->replenishments[repl_prev_idx].replenish_amount += cycles;
		st->replenishments[repl_prev_idx].replenish_time_abs = replenish_time;
	}
	else {
		// Add a new replenishment
		st->replenishments[st->repl_tail_idx].replenish_amount += cycles;
		st->replenishments[st->repl_tail_idx].replenish_time_abs = replenish_time;
		st->repl_tail_idx = repl_next_idx;
		st->repl_count++;
	}

	//Print all replenishments
	/*
	int idx = st->repl_head_idx;
	for (int i = 0; i < st->repl_count; i++) {
		COS_TRACE("-Replenisment: TID: %ld, Replenish Time: %llu, Replenish Amount: %llu\n", t->tid, st->replenishments[idx].replenish_time_abs, st->replenishments[idx].replenish_amount);
		idx = (idx + 1) == SLM_FPRES_REPL_WINDOW_SIZE ? 0 : idx + 1;
	}
	*/
	//COS_TRACE("Replenisment: TID: %ld, Replenish Time: %llu, Replenish Amount: %llu\n", t->tid, replenish_time, cycles);

	if (st->state == STATE_BLOCKED || st->state == STATE_BLOCKED_PERIODIC) {
		// Skip adding the timer for blocked threads	
		// COS_TRACE("\"event\":\"#ASSERT#\", \"tid\":%ld, \"state\":%d", t->tid, st->state, 0);
		// assert(0);
		return;
	}

	// if budget is 0, add timer and block	
	if (tt->budget <= 0) {
		expended(t);
	}

	// Add the earliest replenishment timer if not already added
	if (tt->timeout_idx == -1) {
		// There should be a timer for blocked threads
		assert(st->state != STATE_BLOCKED || st->state != STATE_BLOCKED_PERIODIC);
		slm_timer_fpss_add(t, st->replenishments[st->repl_head_idx].replenish_time_abs);
	}

	return; 
}

static void
set_next_timer_interrupt(struct slm_thd *t, cycles_t now)
{
	struct timer_global *g = timer_global();
	cycles_t next_timeout = 0; 

	/* Are there any thread in timer queue? */
	/* TODO: We dont pay attention to the priority now */
	if (heap_size(&g->h) > 0) {

		struct slm_thd *tp;
		struct slm_timer_thd *tt;
		/* What is the closest-timeout? */
		tp = heap_peek(&g->h);
		assert(tp);
		tt = slm_thd_timer_policy(tp);
		assert(tt && tt->timeout_idx > 0);

		next_timeout = tt->abs_next_processing;
		
	} else {
		/* No thread in the timer queue */
		next_timeout = 0;

	}
	
	
	/* Check if the next timeout is further than the budget of the current thread */
	if(t != NULL) { 
		struct slm_timer_thd *curr = slm_thd_timer_policy(t);
		if (curr->is_budgeted) {
			assert(curr->budget >= 0);
			// TODO: a WCET check can be done here
			// Check if the WCET exceeds the abs_period_end
			// assert(curr->WCET <= (curr->abs_period_end - now));

			// Take the minimum of the next_timeout and curr_deadline
			next_timeout = (next_timeout > ((cycles_t)curr->budget + now)) ? ((cycles_t)curr->budget + now) : next_timeout;
		}
	}
	
	// TODO: Hacked because even clearing timeout, it continues to interrupt
	// slm_timeout_clear();
	slm_timeout_set(9999999999999999);

	/* Set the next timeout */
	if (next_timeout != 0) {
		g->current_timeout = next_timeout;
		slm_timeout_set(next_timeout);
	}
}

struct slm_thd *
slm_sched_fpss_schedule(cycles_t now)
{
	int i;
	struct slm_sched_thd *st;
	struct slm_timer_thd *tt;
	struct ps_list_head *prios = run_queue[cos_cpuid()].prio;
	struct timer_global *g = timer_global();

	for (i = 0 ; i < SLM_FPRR_NPRIOS ; i++) {
		if (ps_list_head_empty(&prios[i])) continue;
		st = ps_list_head_first_d(&prios[i], struct slm_sched_thd);
		tt = slm_thd_timer_policy(slm_thd_from_sched(st));

		/*
		 * We want to move the selected thread to the back of the list.
		 * Otherwise it won't be truly round robin 
		 */

		/* Threads with no budget should not be in the runqueue */	
		// TODO: I saw this assert happening, need to investigate	
		// assert(st->state != STATE_EXPENDED);
		if ((tt->is_budgeted) && (tt->budget <= 0)) {
			COS_TRACE("\"event\":\"#ASSERT#\", \"tid\":%ld, \"budget\":%lld", slm_thd_from_sched(st)->tid, tt->budget, 0);
			continue;
		}	
		assert(!tt->is_budgeted || tt->budget > 0);

		ps_list_rem_d(st);
		ps_list_head_append_d(&prios[i], st);		
		
		// TODO: Workaround
		// Check if the thread same as the previous one
		if (st->repl_count > 0 && (st->repl_count != SLM_FPRES_REPL_WINDOW_SIZE) && (g->previous_tid == slm_thd_from_sched(st)->tid)) {
			// Remove the last replenishment from the replenishment window next will be added after next execution
			//COS_TRACE("\"event\":\"#ASSERT#\", \"tid\":%ld, \"repl-count\":%d", slm_thd_from_sched(st)->tid, st->repl_count, 0);
			st->repl_tail_idx = (st->repl_tail_idx - 1) == -1 ? SLM_FPRES_REPL_WINDOW_SIZE - 1 : st->repl_tail_idx - 1;
			// Cancel the timer if it is added
			if (tt->timeout_idx != -1 && (tt->abs_next_processing == st->replenishments[st->repl_tail_idx].replenish_time_abs)) {

				//COS_TRACE("\"event\":\"#ASSERT#\", \"tid\":%ld, \"timeout-idx\":%d, \"abs_next_processing\": %llu", slm_thd_from_sched(st)->tid, tt->timeout_idx, tt->abs_next_processing);
				assert(st->repl_count == 1);
				slm_timer_fpss_cancel(slm_thd_from_sched(st));
			}
			st->repl_count--;
		}

		/* Set the timer */
		set_next_timer_interrupt(slm_thd_from_sched(st), now);

		st->state = STATE_RUNNING;	
		//COS_TRACE("\"event\":\"schedule\", \"tid\":%ld, \"next-timeout\":%llu", slm_thd_from_sched(st)->tid, g->current_timeout, 0);


		return slm_thd_from_sched(st);
	}

	set_next_timer_interrupt(NULL, now);
	//COS_TRACE("\"event\":\"schedule\", \"tid\":0, \"next-timeout\":%llu", g->current_timeout, 0, 0);

	return NULL;
}

int
slm_sched_fpss_block(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	assert(st->state != STATE_BLOCKED);
	assert(st->state != STATE_BLOCKED_PERIODIC);

	COS_TRACE("\"event\":\"block\", \"tid\":%ld, \"wake-time\":%llu", t->tid, tt->abs_next_processing, 0);

	/* Remove from runqueue */
	ps_list_rem_d(st);
	st->state = STATE_BLOCKED;

	/* Cancel the timer */
	// TODO: Now cancelling the timer is in sched/main.c should we move it here?

	return 0;
}

/* TODO: Not throughly tested or used, remove this comment after verification */
int
slm_sched_fpss_block_periodic(struct slm_thd *t)
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
	int ret = -1;

	// Remove from runqueue, note that slm_state is still RUNNING
	ps_list_rem_d(st);
	st->state = STATE_EXPENDED;
		
	// Update abs_period_start, abs_period_end
	tt->abs_period_start = tt->abs_period_start + tt->period;
	tt->abs_period_end = tt->abs_period_start + tt->period;

	//COS_TRACE("expended(): TID: %ld Period Start, End: %llu, %llu\n", t->tid, tt->abs_period_start, tt->abs_period_end);
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
			tt->budget += st->replenishments[st->repl_head_idx].replenish_amount;
			COS_TRACE("\"event\":\"replenish\", \"tid\":%ld, \"amount\":%llu", t->tid, st->replenishments[st->repl_head_idx].replenish_amount, 0);
			st->replenishments[st->repl_head_idx].replenish_amount = 0;
			st->repl_head_idx = (st->repl_head_idx + 1) == SLM_FPRES_REPL_WINDOW_SIZE ? 0 : st->repl_head_idx + 1;
			st->repl_count--;
		}
		else {
			break;
		}
	}
}

int
slm_sched_fpss_wakeup(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	struct timer_global *g = timer_global();

	assert(ps_list_singleton_d(st));
	assert(st->state == STATE_BLOCKED);
	
	if (tt->is_budgeted) {
		/* Shift abs_period_start, abs_period_end */
		/* This prevents a thread from gaining advantage over other same priority */

		int periods_passed = (g->current_timeout - tt->abs_period_start) / tt->period;
		
		if (periods_passed > 0) {
			
			tt->abs_period_start += (periods_passed * tt->period);
			tt->abs_period_end = tt->abs_period_start + tt->period;

			/* Shift the replenishment window */
			int idx = st->repl_head_idx;
			for (int i = 0; i < st->repl_count; i++) {
				st->replenishments[idx].replenish_time_abs += (periods_passed * tt->period);
				idx = (idx + 1) == SLM_FPRES_REPL_WINDOW_SIZE ? 0 : idx + 1;
			}
		}

		//COS_TRACE("\"event\":\"wakeup\", \"tid\":%ld, \"period-start\":%llu, \"period-end\":%llu", t->tid, tt->abs_period_start, tt->abs_period_end);

		/* Add the cancelled timer in slm_sched_fpss_block() */
		assert(tt->timeout_idx == -1);
		slm_timer_fpss_add(t, st->replenishments[st->repl_head_idx].replenish_time_abs);

		// If there is no budget change state and return
		if (tt->budget <= 0) {
			st->state = STATE_EXPENDED;
			return 0;
		}
	}

	/* Add to the runqueue */
	st->state = STATE_READY;
	ps_list_head_append_d(&run_queue[cos_cpuid()].prio[t->priority], st);

	return 0;
}


/* TODO: Not throughly tested or used, remove this comment after verification */
int
slm_sched_fpss_wakeup_periodic(struct slm_thd *t, cycles_t now)
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
slm_sched_fpss_yield(struct slm_thd *t, struct slm_thd *yield_to)
{
	// TODO: Not implemented yet
	
	// Do nothing for the same priority threads it will 
	// COS_TRACE("\"event\":\"yield\", \"tid\":%ld, \"yield-to\":%ld", t->tid, yield_to->tid, 0);
}

int
slm_sched_fpss_thd_init(struct slm_thd *t)
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
slm_sched_fpss_thd_deinit(struct slm_thd *t)
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

	t->priority = prio - 1;
	ps_list_rem_d(st); /* if we're already on a list, and we're updating priority */
	ps_list_head_append_d(&run_queue[cos_cpuid()].prio[prio], st);

	COS_TRACE("\"event\":\"update-priority\", \"tid\":%ld, \"priority\":%d", t->tid, t->priority, 0);

	return;
}

static void
update_period(struct slm_thd *t, cycles_t period)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	tt->period = period;

	/* TODO: Align the period start for the threads has the same period */
	// tt->abs_period_start = slm_now();
	// tt->abs_period_start = tt->abs_period_start - (tt->abs_period_start % period);
	tt->abs_period_end = tt->abs_period_start + period;

	COS_TRACE("\"event\":\"update-period\", \"tid\":%ld, \"period\":%llu", t->tid, tt->period, 0);
	return;
}

static void
update_budget(struct slm_thd *t, cycles_t budget)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	tt->budget = budget;
	tt->initial_budget = budget;
	tt->is_budgeted = 1;

	COS_TRACE("\"event\":\"update-budget\", \"tid\":%ld, \"budget\":%llu", t->tid, tt->budget, 0);

	return;
}

int
slm_sched_fpss_thd_update(struct slm_thd *t, sched_param_type_t type, unsigned int v)
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
	default:
		return -1;
	}
}

void
slm_sched_fpss_init(void)
{
	int i;

	for (i = 0 ; i < SLM_FPRR_NPRIOS ; i++) {
		ps_list_head_init(&run_queue[cos_cpuid()].prio[i]);
	}
}
