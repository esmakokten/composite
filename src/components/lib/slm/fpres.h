#ifndef FPRES_H
#define FPRES_H

#include <ps_list.h>

struct slm_sched_thd {
	struct ps_list list;
};

#include <slm.h>

SLM_MODULES_POLICY_PROTOTYPES(fpres)

SLM_MODULES_TIMER_PROTOTYPES(fpres)

struct slm_timer_thd {
	int      timeout_idx;	/* where are we in the heap? */
	cycles_t abs_wakeup;
	// ESMA budget
	// ESMA there could per thread enum event

};

#endif	/* FPRES_H */
