#ifndef FPRES_H
#define FPRES_H

#include <cos_types.h>
#include <cos_component.h>
#include <slm.h>
#include <slm_api.h>
#include <heap.h>
#include <ps_list.h>
#include <slm.h>
#include <ck_ring.h>

SLM_MODULES_POLICY_PROTOTYPES(fpres)

SLM_MODULES_TIMER_PROTOTYPES(fpres)

#define SLM_FPRES_REPL_WINDOW_SIZE   5

enum state_t {
	STATE_EXPENDED,
	STATE_READY,
	STATE_BLOCKED,
	STATE_BLOCKED_PERIODIC,
	STATE_RUNNING,
	STATE_DEINIT,
};

struct replenishment {
	cycles_t replenish_time_abs;
	cycles_t replenish_amount;
};

struct slm_sched_thd {
	struct ps_list list;
	enum   state_t state; /* thread state for the policy */
	struct replenishment replenishments[SLM_FPRES_REPL_WINDOW_SIZE]; 
	int repl_head_idx;
	int repl_tail_idx;
	int repl_count;
};

struct slm_timer_thd {
	int      timeout_idx;	/* where are we in the heap? */
	cycles_t abs_next_processing; 
	cycles_t abs_period_start;
	cycles_t abs_period_end; 
	cycles_t period;
	budget_t budget;	/* budget can hold negative values */
	budget_t initial_budget;
	int      is_budgeted;
};

// TODO: Move it
int slm_sched_fpres_wakeup_periodic(struct slm_thd *t, cycles_t now);

static void set_next_timer_interrupt(struct slm_thd *t, cycles_t now, cycles_t closest_prior_timer);
static void expended(struct slm_thd *t);
static void replenish(struct slm_thd *t, cycles_t now);

#endif	/* FPRES_H */
