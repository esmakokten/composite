#include <fpds.h>

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
	struct heap    h; // you need to extend the heap , ring buffer in the thread
	void           *data[MAX_NUM_THREADS];

#ifdef PRIORITY_AWARE
#else
	cycles_t       current_timeout;
#endif
} CACHE_ALIGNED;


#ifdef PRIORITY_AWARE
static struct timer_global __timer_globals[NUM_CPU][SLM_FPRR_NPRIOS];
static cycles_t g_current_timeout;
static thdid_t g_prev_tid;
static inline struct timer_global *
timer_global_prio(int prio) {
	return &__timer_globals[cos_coreid()][prio];
}

struct slm_timer_thd *
process_prio_timers(int prio, cycles_t now)
{
	struct timer_global *g = timer_global_prio(prio);
	g_current_timeout = now;
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
		if (cycles_greater_than(tt->abs_next_processing, now)) return tt;
		
		/* Dequeue thread with closest wakeup */
		th = timer_heap_highest(&g->h);
		assert(th == tp);

		tt->timeout_idx = -1;

		//COS_TRACE("expire(): TID: %ld Timer: %llu Now: %llu\n", th->tid, tt->abs_next_processing, now);

		// Check the state
		switch(st->state) {
			case STATE_EXPENDED:
			{
				/* A thread, in its new period with no budget, wants to replenish */

				//COS_TRACE("replenish(): TID: %ld Now: %llu\n", th->tid, now, 0);
				replenish(th, now);

				/* Thread can only be in expended state if it is budgeted */
				assert(tt->is_budgeted);

				break;
			}
			case STATE_READY:
			{
				/* A thread, waiting for the execution in ready state */

				// There should not be any timer for the threads in ready state
				assert(0);

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
				slm_sched_fpds_wakeup_periodic(th, now);
				break;
			}
			case STATE_RUNNING:
			{
				/* A thread in the runqueue, executed in its current period wants to replenish */
								
				// Optimization: For the budgeted threads that still have budget(in the runqueue), 
				// We can replenish just before it is scheduled

				// replenish(th, now);

				break;
			}
			default:
				break;
		}
	}

	return NULL;
}
#else
static struct timer_global __timer_globals[NUM_CPU];
static inline struct timer_global *
timer_global(void) {
	return &__timer_globals[cos_coreid()];
}
#endif
/* The timer expired */
void
slm_timer_fpds_expire(cycles_t now)
{
#ifdef PRIORITY_AWARE
	// Empty
#else
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
		th = timer_heap_highest(&g->h);
		assert(th == tp);

		tt->timeout_idx = -1;

		// Check the state
		switch(st->state) {
			case STATE_EXPENDED:
			{
				/* A thread, in its new period with no budget, wants to replenish */

				replenish(th, now);

				/* Thread can only be in expended state if it is budgeted */
				assert(tt->is_budgeted);

				break;
			}
			case STATE_READY:
			{
				/* A thread, waiting for the execution in ready state */
				// There should not be any timer for the threads in ready state
				assert(0);

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
				slm_sched_fpds_wakeup_periodic(th, now);
				break;
			}
			case STATE_RUNNING:
			{
				/* A thread in the runqueue, executed in its current period wants to replenish */
								
				// Optimization: For the budgeted threads that still have budget(in the runqueue), 
				// We can replenish just before it is scheduled

				// replenish(th, now);

				break;
			}
			default:
				break;
		}
	}

#endif
}

/*
 * Timeout and wakeup functionality
 *
 * TODO: Replace the in-place heap with a rb-tree to avoid external, static allocation.
 */

int
slm_timer_fpds_add(struct slm_thd *t, cycles_t absolute_timeout) 
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
#ifdef PRIORITY_AWARE
	struct timer_global *g = timer_global_prio(t->priority);
#else
	struct timer_global *g = timer_global();
#endif

	struct slm_sched_thd *st = slm_thd_sched_policy(t);

	assert(tt && tt->timeout_idx == -1);
	assert(heap_size(&g->h) < MAX_NUM_THREADS);

	tt->abs_next_processing = absolute_timeout;
	timer_heap_add(&g->h, t);
 
	return 0;
}

int
slm_timer_fpds_cancel(struct slm_thd *t)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
#ifdef PRIORITY_AWARE
	struct timer_global *g = timer_global_prio(t->priority);
#else
	struct timer_global *g   = timer_global();
#endif

	if (tt->timeout_idx == -1) return 0;

	assert(heap_size(&g->h));
	assert(tt->timeout_idx > 0);

	timer_heap_remove(&g->h, tt->timeout_idx);
	tt->timeout_idx = -1;

	return 0;
}

int
slm_timer_fpds_thd_init(struct slm_thd *t)
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

	//COS_TRACE("slm_timer_fpds_thd_init(): TID: %ld Period Start: %llu, %d", t->tid, tt->abs_period_start, 0);
	// TODO: Check if the thread has higher priority than the current thread?
	// Add timer interrupt if necessary?

	return 0;
}

void
slm_timer_fpds_thd_deinit(struct slm_thd *t)
{
	// Cancel the timers
	slm_timer_fpds_cancel(t);
	return;
}

static void
slm_policy_timer_init(microsec_t period)
{
	cycles_t next_timeout;
#ifdef PRIORITY_AWARE
	for (int i = 0; i < NUM_CPU; i++) {
		for (int j = 0; j < SLM_FPRR_NPRIOS; j++) {
			struct timer_global *g = timer_global_prio(j);
			memset(g, 0, sizeof(struct timer_global));
			heap_init(&g->h, MAX_NUM_THREADS);
		}
	}
#else
	struct timer_global *g = timer_global();
	memset(g, 0, sizeof(struct timer_global));
	heap_init(&g->h, MAX_NUM_THREADS);
#endif

	next_timeout = slm_now();
#ifdef PRIORITY_AWARE
    g_current_timeout = next_timeout;
#else
	g->current_timeout = next_timeout;
#endif
	slm_timeout_set(next_timeout);
}

int
slm_timer_fpds_init(void)
{
	/* 10ms */
	slm_policy_timer_init(10000);

	return 0;
}

void
slm_sched_fpds_execution(struct slm_thd *t, cycles_t cycles, cycles_t now)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	struct slm_sched_thd *st = slm_thd_sched_policy(t);

	if(tt->is_budgeted == 0) {
		return;
	}

	tt->budget -= cycles;

	// Are period_start and period_end correct?
	assert(tt->abs_period_start <= now);
	// Did we miss the deadline? 
	// assert(tt->abs_period_end >= now + remaining WCET);
	
	// Plan the next replenishment
	// TODO: Temporary for deferrable server

	// if budget is 0, add timer and block	
	if (tt->budget <= 0) {
		tt->budget = 0;
		//COS_TRACE("expended(): TID: %ld Now: %llu\n", t->tid, now, 0);
		expended(t);
	} else if (st->state == STATE_BLOCKED_PERIODIC) {
		//Update the abs_period_start and abs_period_end
		tt->abs_period_start = tt->abs_period_start + tt->period;
		tt->abs_period_end = tt->abs_period_start + tt->period;
		//Add timer for the next replenishment
		slm_timer_fpds_add(t, tt->abs_period_start);
	}

	return; 
}

static void
#ifdef PRIORITY_AWARE
set_next_timer_interrupt(struct slm_thd *t, cycles_t now, cycles_t closest_prior_timer)
{
	cycles_t next_timeout = closest_prior_timer;

#else
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
		
	}
#endif
	
	/* Check if the next timeout is further than the budget of the current thread */
	if(t != NULL) { 
		struct slm_timer_thd *curr = slm_thd_timer_policy(t);
		if (curr->is_budgeted) {
			assert(curr->budget >= 0);
			//Check if the budget exceeds the abs_period_end
			//If it does, set the curr_deadline to abs_period_end
			cycles_t curr_deadline = (cycles_t)curr->budget > curr->abs_period_end ? curr->abs_period_end : (cycles_t)curr->budget;
			//Take the minimum of the next_timeout and curr_deadline
			next_timeout = (next_timeout > (curr_deadline + now)) ? (curr_deadline + now) : next_timeout;
		}
	}
	
	// TODO: Hacked because even clearing timeout, it continues to interrupt
	// slm_timeout_clear();
	slm_timeout_set(9999999999999999);

	/* Set the next timeout */
	if (next_timeout != 0) {
#ifdef PRIORITY_AWARE
		g_current_timeout = next_timeout;
#else
		g->current_timeout = next_timeout;
#endif
		slm_timeout_set(next_timeout);
	}
}

struct slm_thd *
slm_sched_fpds_schedule(cycles_t now)
{
	int i;
	struct slm_sched_thd *st;
	struct slm_timer_thd *tt;
	struct ps_list_head *prios = run_queue[cos_cpuid()].prio;
#ifdef PRIORITY_AWARE
	cycles_t closest_prior_timer = 0;
	struct slm_timer_thd *next_processing_thd = NULL;
#else
	struct timer_global *g = timer_global();
#endif

	for (i = 0 ; i < SLM_FPRR_NPRIOS ; i++) {

#ifdef PRIORITY_AWARE
		/* Process the higher priority timers */
		next_processing_thd = process_prio_timers(i, now);
		if (next_processing_thd != NULL) {
			if (closest_prior_timer == 0 || (next_processing_thd->abs_next_processing!= 0 && cycles_greater_than(closest_prior_timer, next_processing_thd->abs_next_processing))) {
				closest_prior_timer = next_processing_thd->abs_next_processing;
			}
		}
#endif
		if (ps_list_head_empty(&prios[i])) continue;
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

		/* Set the timer */
#ifdef PRIORITY_AWARE
		set_next_timer_interrupt(slm_thd_from_sched(st), now, closest_prior_timer);
#else
		set_next_timer_interrupt(slm_thd_from_sched(st), now);
#endif

		st->state = STATE_RUNNING;	
		//COS_TRACE("slm_sched_fpds_schedule(): TID: %ld Next Timeout: %llu\n", slm_thd_from_sched(st)->tid, g->current_timeout, 0);
		return slm_thd_from_sched(st);
	}
#ifdef PRIORITY_AWARE
	set_next_timer_interrupt(NULL, now, closest_prior_timer);
#else
	set_next_timer_interrupt(NULL, now);
#endif
	//COS_TRACE("slm_sched_fpds_schedule(): IDLE Next Timeout: %llu\n", g->current_timeout, 0,0);
	return NULL;
}

int
slm_sched_fpds_block(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	assert(st->state != STATE_BLOCKED);
	assert(st->state != STATE_BLOCKED_PERIODIC);

	/* Remove from runqueue */
	ps_list_rem_d(st);
	st->state = STATE_BLOCKED;

	//COS_TRACE("slm_sched_fpds_block(): TID: %ld State: %d", t->tid, st->state, 0);

	// TODO: Now cancelling the timer is in sched/main.c should we move it here?

	return 0;
}

/* TODO: Not throughly tested or used, remove this comment after verification */
int
slm_sched_fpds_block_periodic(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	assert(tt->is_budgeted);
	assert(st->state != STATE_BLOCKED);
	assert(st->state != STATE_BLOCKED_PERIODIC);

	/* Cancel the timer */
	slm_timer_fpds_cancel(t);

	/* Remove from runqueue */
	st->state = STATE_BLOCKED_PERIODIC;
	ps_list_rem_d(st);

	/* Update abs_period_start, abs_period_end and abs_next_processing */
	/* Reschedule is called afterwards, wakeup timer is added there */

	return 0;
}

static void
expended(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	int ret = -1;

	// Update abs_period_start, abs_period_end
	tt->abs_period_start = tt->abs_period_start + tt->period;
	tt->abs_period_end = tt->abs_period_start + tt->period;

	//COS_TRACE("expended(): TID: %ld Period Start, End: %llu, %llu", t->tid, tt->abs_period_start, tt->abs_period_end);

	if (st->state != STATE_BLOCKED_PERIODIC && st->state != STATE_BLOCKED) {
		// Remove from runqueue, note that slm_state is still RUNNING
		ps_list_rem_d(st);
		st->state = STATE_EXPENDED;
	}
	
	// Do not add timer if the thread is STATE_BLOCKED
	if (st->state == STATE_BLOCKED) {
		return;
	}

	// Add replenishment timer	
	ret = slm_timer_fpds_add(t, tt->abs_period_start);
	assert(ret == 0);
}

static void
replenish(struct slm_thd *t, cycles_t now)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	int ret = -1;

	tt->budget = tt->initial_budget;

	assert(st->state == STATE_EXPENDED || st->state == STATE_BLOCKED_PERIODIC);
	st->state = STATE_READY;

	//COS_TRACE("replenish(): TID: %ld, planned: %llu, now:%llu", t->tid, tt->abs_next_processing, now);

	/* Add to the runqueue */
	if (tt->budget > 0) {
		ps_list_head_append_d(&run_queue[cos_cpuid()].prio[t->priority], st);
	}
}

int
slm_sched_fpds_wakeup(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
#ifdef PRIORITY_AWARE
#else
	struct timer_global *g = timer_global();
#endif
	assert(ps_list_singleton_d(st));
	assert(st->state == STATE_BLOCKED);
	
	if (tt->is_budgeted) {
		/* Shift abs_period_start, abs_period_end and abs_next_processing */
		/* This prevents a thread from gaining advantage over other same priority */
		cycles_t offset_abs_next_processing = tt->abs_next_processing - tt->abs_period_start;
#ifdef PRIORITY_AWARE
		int periods_passed = (g_current_timeout - tt->abs_period_start) / tt->period;
		COS_TRACE("wakeup(): TID: %ld Periods Passed: %d Planned: %llu", t->tid, periods_passed, g_current_timeout);
#else		 
		int periods_passed = (g->current_timeout - tt->abs_period_start) / tt->period;
		COS_TRACE("wakeup(): TID: %ld Periods Passed: %d Planned: %llu", t->tid, periods_passed, g->current_timeout);
#endif
		tt->abs_period_start += (periods_passed * tt->period);
		tt->abs_period_end = tt->abs_period_start + tt->period;

		/* TODO: Update replenisment window abs values */
		/* Add the cancelled timer in slm_sched_fpds_block() */
		// Recover last state
		// tt->abs_next_processing = tt->abs_period_start + offset_abs_next_processing;

		// TODO: For only deferable server 
		// If there is no budget change state and add timer
		if (tt->budget <= 0 && periods_passed == 0) {
			slm_timer_fpds_add(t, tt->abs_period_end);
			st->state = STATE_EXPENDED;
			return 0;
		}

		tt->budget = tt->initial_budget;
	}
	/* Add to the runqueue */
	st->state = STATE_READY;
	ps_list_head_append_d(&run_queue[cos_cpuid()].prio[t->priority], st);

	return 0;
}


/* TODO: Not throughly tested or used, remove this comment after verification */
int
slm_sched_fpds_wakeup_periodic(struct slm_thd *t, cycles_t now)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	assert(ps_list_singleton_d(st));
	assert(st->state == STATE_BLOCKED_PERIODIC);

	assert(now < tt->abs_period_end);
	if (tt->is_budgeted) {
		tt->budget = tt->initial_budget;
	}

	/* Add to the runqueue */
	st->state = STATE_READY;
	ps_list_head_append_d(&run_queue[cos_cpuid()].prio[t->priority], st);
	return 0;
}

void
slm_sched_fpds_yield(struct slm_thd *t, struct slm_thd *yield_to)
{

	// TODO: Not implemented yet
}

int
slm_sched_fpds_thd_init(struct slm_thd *t)
{
	struct slm_sched_thd *st = slm_thd_sched_policy(t);

	t->priority = SLM_FPRR_PRIO_LOWEST;
	st->state = STATE_READY;

	ps_list_init_d(st);

	return 0;
}

void
slm_sched_fpds_thd_deinit(struct slm_thd *t)
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

	return;
}

static void
update_period(struct slm_thd *t, cycles_t period)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	tt->period = slm_usec2cyc(period);
	cycles_t offset = tt->abs_period_start % tt->period;
	tt->abs_period_start = tt->abs_period_start - offset;
	tt->abs_period_end = tt->abs_period_start + tt->period;

	//COS_TRACE("update_period(): TID: %lu Period: %llu, Start: %llu", t->tid, tt->period, tt->abs_period_start);

	return;
}

static void
update_budget(struct slm_thd *t, cycles_t budget)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	tt->budget = slm_usec2cyc(budget);
	tt->initial_budget = tt->budget;
	tt->is_budgeted = 1;

	//COS_TRACE("update_budget(): TID: %lu Budget: %llu", t->tid, tt->budget, 0);

	return;
}

int
slm_sched_fpds_thd_update(struct slm_thd *t, sched_param_type_t type, unsigned int v)
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
slm_sched_fpds_init(void)
{
	int i;

	for (i = 0 ; i < SLM_FPRR_NPRIOS ; i++) {
		ps_list_head_init(&run_queue[cos_cpuid()].prio[i]);
	}

	// TODO: Added for test purposes
	printc("### SLM FPDS Scheduler Initialized ###\n");
}
