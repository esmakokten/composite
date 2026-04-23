/*
 * Test for sched_aep_create / sched_aep_create_closure.
 *
 * Creates an AEP thread via the scheduler, sends to it with cos_asnd,
 * and verifies the AEP thread is activated and receives the event.
 */

#include <llprint.h>
#include <sched.h>
#include <cos_kernel_api.h>
#include <cos_defkernel_api.h>
#include <cos_time.h>
#include <res_spec.h>

#define AEP_PRIO  20
#define MAIN_PRIO 10
#define ITER      16

static volatile int aep_activated;
static volatile int aep_rcv_count;

static void
aep_fn(arcvcap_t rcv, void *data)
{
	int rcvd = 0;

	printc("AEP thread (tid %lu): started, rcv cap = %lu\n",
	       (unsigned long)cos_thdid(), (unsigned long)rcv);

	while (1) {
		int ret;

		ret = cos_rcv(rcv, RCV_ALL_PENDING, &rcvd);
		aep_rcv_count += (rcvd > 0) ? rcvd : 1;
		aep_activated = 1;

		printc("AEP thread: cos_rcv returned %d, rcvd %d (total %d)\n",
		       ret, rcvd, aep_rcv_count);

		if (aep_rcv_count >= ITER) break;
	}

	printc("AEP thread: received all %d events, exiting\n", ITER);
	sched_thd_exit();
}

int
main(void)
{
	struct cos_aep_info aep = { 0 };
	struct cos_compinfo *ci = cos_compinfo_get(cos_defcompinfo_curr_get());
	asndcap_t snd;
	thdid_t tid;
	int i;

	printc("\n=== AEP Test (comp %ld) ===\n", cos_compid());

	/* Create AEP thread through the scheduler */
	tid = sched_aep_create(&aep, aep_fn, NULL, 0, 0, 0, 0);
	if (tid == 0) {
		printc("FAIL: sched_aep_create returned 0\n");
		return -1;
	}
	printc("AEP created: tid = %lu, rcv = %lu, thd = %lu, tc = %lu\n",
	       (unsigned long)aep.tid, (unsigned long)aep.rcv,
	       (unsigned long)aep.thd, (unsigned long)aep.tc);

	assert(aep.rcv != 0);
	assert(aep.tid != 0);

	/* Set priority so the AEP thread can be scheduled */
	sched_thd_param_set(tid, sched_param_pack(SCHEDP_PRIO, AEP_PRIO));

	/* Create an asnd capability to send to the AEP's rcv endpoint */
	printc("Main thread: creating asnd cap for AEP's rcv endpoint...\n");
	printc("AEP rcv cap = %lu, comp captbl cap = %lu\n", (unsigned long)aep.rcv, (unsigned long)ci->captbl_cap);
	snd = cos_asnd_alloc(ci, aep.rcv, ci->captbl_cap);
	if (!snd) {
		printc("FAIL: cos_asnd_alloc returned 0\n");
		return -1;
	}
	printc("asnd cap = %lu\n", (unsigned long)snd);

	/* Send events to the AEP and verify activation */
	for (i = 0; i < ITER; i++) {
		int ret;

		ret = cos_asnd(snd, 0);
		printc("cos_asnd[%d]: ret = %d\n", i, ret);

		/* Yield to let the AEP thread run */
		sched_thd_yield_to(tid);
	}

	/* Give AEP thread time to process remaining events */
	int count = 0;
	while (!aep_activated || aep_rcv_count < ITER) {
		sched_thd_yield_to(tid);
		if (count++ > ITER * 2) {
			printc("Main thread: waited too long for AEP to process events, breaking wait loop\n");
			break;
		}
	}

	printc("Main: aep_activated = %d, aep_rcv_count = %d\n",
	       aep_activated, aep_rcv_count);

	if (aep_activated && aep_rcv_count >= ITER) {
		printc("=== AEP Test PASSED ===\n\n");
	} else {
		printc("=== AEP Test FAILED ===\n\n");
	}

	printc("Done, spinning.\n");
	while (1);
	return 0;
}
