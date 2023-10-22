#include <cos_types.h>
#include <cos_component.h>
#include <slm.h>
#include <slm_api.h>
#include <heap.h>
#include <fpres.h>

/***
 * Quantum-based time management. Wooo. Periodic timer FTW.
 */

struct timer_global {
	struct heap  h; // you need to extend the heap , ring buffer in the thread
	void        *data[MAX_NUM_THREADS];
	cycles_t     period;
	cycles_t     current_timeout;
	// ESMA timer event type? BUDGET / REPLENISH / ACTIVATION

} CACHE_ALIGNED;

static struct timer_global __timer_globals[NUM_CPU];

static inline struct timer_global *
timer_global(void) {
	return &__timer_globals[cos_coreid()];
}

/* wakeup any blocked threads! */
static void
quantum_wakeup_expired(cycles_t now)
{
	struct timer_global *g = timer_global();

	while (heap_size(&g->h) > 0) {
		struct slm_thd *tp, *th;
		struct slm_timer_thd *tt;

		/* Should we wake up the closest-timeout thread? */
		tp = heap_peek(&g->h);
		assert(tp);
		tt = slm_thd_timer_policy(tp);
		assert(tt && tt->timeout_idx > 0);

		/* No more threads to wake! */
		if (cycles_greater_than(tt->abs_wakeup, now)) break;

		/* Dequeue thread with closest wakeup */
		th = heap_highest(&g->h);
		assert(th == tp);

		tt->timeout_idx = -1;
		tt->abs_wakeup  = now;
		slm_thd_wakeup(th, 1);
	}
}

/* The timer expired */
void
slm_timer_fpres_expire(cycles_t now)
{
	struct timer_global *g = timer_global();
	cycles_t             offset;
	cycles_t             next_timeout;

	/*
	 * Note that we might miss specific quantum if we are in a
	 * virtualized environment. Thus we might be multiple periods
	 * into the future.
	 */
	// ESMA repleni

	assert(now >= g->current_timeout);

	offset = (now - g->current_timeout) % g->period;
 	assert(g->period > offset);
	next_timeout = now + (g->period - offset);
	assert(next_timeout > now);

	slm_timeout_set(next_timeout);
	g->current_timeout = next_timeout;

	quantum_wakeup_expired(now);
}

/*
 * Timeout and wakeup functionality
 *
 * TODO: Replace the in-place heap with a rb-tree to avoid external, static allocation.
 */

int
slm_timer_fpres_add(struct slm_thd *t, cycles_t absolute_timeout) // 
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);
	struct timer_global *g = timer_global();

	assert(tt && tt->timeout_idx == -1);
	assert(heap_size(&g->h) < MAX_NUM_THREADS);

	tt->abs_wakeup = absolute_timeout;
	heap_add(&g->h, t);

	return 0;
}

int
slm_timer_fpres_cancel(struct slm_thd *t)
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
slm_timer_fpres_thd_init(struct slm_thd *t)
{
	struct slm_timer_thd *tt = slm_thd_timer_policy(t);

	*tt = (struct slm_timer_thd){
		.timeout_idx = -1,
		.abs_wakeup  = 0
	};

	return 0;
}

void
slm_timer_fpres_thd_deinit(struct slm_thd *t)
{
	return;
}

static int
__slm_timeout_compare_min(void *a, void *b)
{
	/* FIXME: logic for wraparound in either timeout_cycs */
	return slm_thd_timer_policy((struct slm_thd *)a)->abs_wakeup <= slm_thd_timer_policy((struct slm_thd *)b)->abs_wakeup;
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
	g->period = slm_usec2cyc(period);
	heap_init(&g->h, MAX_NUM_THREADS, __slm_timeout_compare_min, __slm_timeout_update_idx);

	next_timeout = slm_now() + g->period;
	g->current_timeout = next_timeout;
	slm_timeout_set(next_timeout);
}

int
slm_timer_fpres_init(void)
{
	/* 10ms */
	slm_policy_timer_init(10000);

	return 0;
}



#define SLM_FPRR_NPRIOS         32
#define SLM_FPRR_PRIO_HIGHEST   TCAP_PRIO_MAX
#define SLM_FPRR_PRIO_LOWEST    (SLM_FPRR_NPRIOS - 1)

struct runqueue {
	struct ps_list_head prio[SLM_FPRR_NPRIOS];
} CACHE_ALIGNED;
struct runqueue threads[NUM_CPU];

// ESMA event queue
struct replqueue {
	struct ps_list_head prio[SLM_FPRR_NPRIOS];
} CACHE_ALIGNED;
struct replqueue replenishments[NUM_CPU];

/* No RR based on execution, yet */
void
slm_sched_fpres_execution(struct slm_thd *t, cycles_t cycles)
{ return; }

struct slm_thd *
slm_sched_fpres_schedule(void)
{
	int i;
	struct slm_sched_thd *t;
	struct ps_list_head *prios = threads[cos_cpuid()].prio;

    // ESMA check the replenishment queue
	// ESMA 

	for (i = 0 ; i < SLM_FPRR_NPRIOS ; i++) {
		if (ps_list_head_empty(&prios[i])) continue;
		t = ps_list_head_first_d(&prios[i], struct slm_sched_thd);

		/*
		 * We want to move the selected thread to the back of the list.
		 * Otherwise fprr won't be truly round robin
		 */
		ps_list_rem_d(t);
		ps_list_head_append_d(&prios[i], t);

//		printc("Schedule -> %ld\n", slm_thd_from_sched(t)->tid);
		return slm_thd_from_sched(t);
	}
//	printc("Schedule -> idle\n");

	return NULL;
}

int
slm_sched_fpres_block(struct slm_thd *t)
{
	struct slm_sched_thd *p = slm_thd_sched_policy(t);

	ps_list_rem_d(p);

	return 0;
}

int
slm_sched_fpres_wakeup(struct slm_thd *t)
{
	struct slm_sched_thd *p = slm_thd_sched_policy(t);

	assert(ps_list_singleton_d(p));

	ps_list_head_append_d(&threads[cos_cpuid()].prio[t->priority - 1], p);

	return 0;
}

void
slm_sched_fpres_yield(struct slm_thd *t, struct slm_thd *yield_to)
{
	struct slm_sched_thd *p = slm_thd_sched_policy(t);

	ps_list_rem_d(p);
	ps_list_head_append_d(&threads[cos_cpuid()].prio[t->priority], p);
}

int
slm_sched_fpres_thd_init(struct slm_thd *t)
{
	t->priority = SLM_FPRR_PRIO_LOWEST;
	ps_list_init_d(slm_thd_sched_policy(t));

	return 0;
}

void
slm_sched_fpres_thd_deinit(struct slm_thd *t)
{
	ps_list_rem_d(slm_thd_sched_policy(t));
}

static void
update_queue(struct slm_thd *t, tcap_prio_t prio)
{
	struct slm_sched_thd *p = slm_thd_sched_policy(t);

	t->priority = prio;
	ps_list_rem_d(p); /* if we're already on a list, and we're updating priority */
	ps_list_head_append_d(&threads[cos_cpuid()].prio[prio], p);

	return;
}

int
slm_sched_fpres_thd_update(struct slm_thd *t, sched_param_type_t type, unsigned int v)
{

	// ESMA Budget updates??
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
	/* Only support priority, for now */
	default:
		return -1;
	}
}

void
slm_sched_fpres_init(void)
{
	int i;

	for (i = 0 ; i < SLM_FPRR_NPRIOS ; i++) {
		ps_list_head_init(&threads[cos_cpuid()].prio[i]);
	}
}
