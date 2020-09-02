#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "fake.h"
#include "rcu_segcblist.c"

static inline void rcu_segcblist_dump(struct rcu_segcblist *rsclp)
{
	int i;

	pr_info("%p->head = %p\n", rsclp, rsclp->head);
	for (i = RCU_DONE_TAIL; i < RCU_CBLIST_NSEGS; i++)
		pr_info("\t->tails[%d] = %p, ->gp_seq[%d] = %ld\n",
		       i, rsclp->tails[i], i, rsclp->gp_seq[i]);
	pr_info("->len = %ld, ->len_lazy = %ld\n", rsclp->len, rsclp->len_lazy);
}

/*
 * Do consistency check on the specified rcu_segcblist structure, but
 * limiting rcu_head traversals to that specified.  Complains bitterly
 * on any inconsistencies that it spots.
 */
#define rcu_segcblist_fsck(rsclp, limit) \
({ \
	long ___lim = (limit); \
	struct rcu_segcblist *___rsclp = (rsclp); \
	long cnt = 0; \
	int i = 0; \
	struct rcu_head **rhpp = &___rsclp->head; \
	static int notdone1 = 1; \
	static int notdone2 = 1; \
	static int notdone3 = 1; \
	\
	for (;;) { \
		while (i < RCU_CBLIST_NSEGS && ___rsclp->tails[i] == rhpp) \
			++i; \
		if (*rhpp == NULL) \
			break; \
		if (++cnt > ___lim) \
			break; \
		rhpp = &(*rhpp)->next; \
	} \
	if (cnt < ___lim && cnt != ___rsclp->len && xchg(&notdone1, 0)) { \
		pr_info("cnt = %ld\n", cnt); \
		rcu_segcblist_dump(___rsclp); \
	} \
	WARN_ON_ONCE(cnt < ___lim && cnt != ___rsclp->len); \
	if (___rsclp->tails[RCU_NEXT_TAIL] && cnt < ___lim && \
	    i != RCU_CBLIST_NSEGS && xchg(&notdone2, 0)) { \
		pr_info("i = %d\n", i); \
		rcu_segcblist_dump(___rsclp); \
	} \
	WARN_ON_ONCE(___rsclp->tails[RCU_NEXT_TAIL] && cnt < ___lim && \
		     i != RCU_CBLIST_NSEGS); \
	if ((___rsclp->len < 0 || ___rsclp->len_lazy < 0 || \
	     ___rsclp->len < ___rsclp->len_lazy) && xchg(&notdone3, 0)) { \
		pr_info("Counter problems\n"); \
		rcu_segcblist_dump(___rsclp); \
	} \
	WARN_ON_ONCE(___rsclp->len < 0); \
	WARN_ON_ONCE(___rsclp->len_lazy < 0); \
	WARN_ON_ONCE(___rsclp->len < ___rsclp->len_lazy); \
	!notdone1 || !notdone2 || !notdone3; \
})

struct rcu_head rh[100];

struct rcu_cblist rcl[10];
struct rcu_segcblist rscl;

static char *fmt_one_rh(struct rcu_head *rhp, char *s_in)
{
	int idx = rhp - rh;
	char *s = s_in;

	*s++ = '|';
	*s++ = idx + 'a';
	return s;
}

static void fmt_rh(struct rcu_segcblist *rsclp, char *s_in)
{
	int i;
	char *s = s_in;
	struct rcu_head **rhpp;
	char *tails = "DWRN";

	rhpp = &rsclp->head;
	for (i = RCU_DONE_TAIL; i <= RCU_NEXT_TAIL; i++) {
		if (rhpp != rsclp->tails[i])
			while (rhpp != rsclp->tails[i]) {
				if (*rhpp == NULL) {
					*s++ = '?';
					*s++ = '\0';
					return;
				}
				s = fmt_one_rh(*rhpp, s);
				rhpp = &(*rhpp)->next;
			}
		*s++ = tails[i];
		if (i != RCU_DONE_TAIL && i != RCU_NEXT_TAIL)
			*s++ = rsclp->gp_seq[i] + '0';
	}
	while (*rhpp != NULL) {
		s = fmt_one_rh(*rhpp, s);
		rhpp = &(*rhpp)->next;
	}
	*s = '\0';
}

void init_rh(int syndrome, struct rcu_segcblist *rsclp)
{
	int cur = RCU_CBLIST_NSEGS - 1;
	int i;
	struct rcu_head *lastrhp;
	int s;

	for (i = 0; i < RCU_CBLIST_NSEGS - 1; i++)
		rh[i].next = &rh[i + 1];
	rh[RCU_CBLIST_NSEGS - 1].next = NULL;
	s = syndrome;
	lastrhp = NULL;
	for (i = RCU_CBLIST_NSEGS - 1; i >= 0; i--) {
		WARN_ON(cur < 0);
		if (s == 0) { 
			rsclp->tails[i] = &rsclp->head;
		} else {
			rsclp->tails[i] = &rh[cur].next;
			lastrhp = &rh[cur];
		}
		rsclp->gp_seq[i] = cur;
		if (s & 0x1)
			cur--;
		s >>= 1;
	}
	rsclp->head = lastrhp;
	if (lastrhp == NULL)
		rsclp->len = 0;
	else
		rsclp->len = &rh[RCU_CBLIST_NSEGS - 1] - lastrhp + 1;
}

void runtest(void (*f)(struct rcu_segcblist *rsclp, char *s, unsigned long gp,
		       unsigned long seq),
	     struct rcu_segcblist *rsclp, bool in_gp, bool root)
{
	unsigned long begin, end;
	char buf0[32];
	char buf1[32];
	char buf2[32];
	char err1[32];
	char err2[32];
	int i;
	int j;
	char *s;
	unsigned long seq;

    	for (i = 0; i < 16; i++) {
		init_rh(i, rsclp);
		fmt_rh(rsclp, buf0);
		begin = end = 0;
		for (j = RCU_DONE_TAIL + 1; j < RCU_NEXT_TAIL; j++)
			if (rsclp->tails[j - 1] != rsclp->tails[j]) {
				begin = rsclp->gp_seq[j] -
					(in_gp || !root) - 1;
				end = rsclp->gp_seq[j];
				break;
			}
		for (; j < RCU_NEXT_TAIL; j++)
			if (rsclp->tails[j - 1] != rsclp->tails[j])
				end = rsclp->gp_seq[j];
		for (j = begin; j != end + 1; j++) {
			if (!in_gp && root)
				seq = j + 1;
			else
				seq = j + 2;
			f(rsclp, err1, j, seq);
			fmt_rh(rsclp, buf1);
			if (strcmp(buf0, buf1) == 0)
				s = "==";
			else
				s = "->";
			printf("%2d,%2d,%2lu: %-16s %s %-16s%s %c",
			       i, j, seq, buf0, s, buf1, err1,
			       ".F"[rcu_segcblist_future_gp_needed(rsclp, j)]);
			f(rsclp, err2, j, seq);
			fmt_rh(rsclp, buf2);
			if (strcmp(buf1, buf2) != 0)
				printf(" !!!-> %s %s", buf2, err2);
			else
				printf(" .%s", err2);
			printf("\n");
			init_rh(i, rsclp);
		}
	}
}

void runtest_no_gp(void (*f)(struct rcu_segcblist *rsclp, char *s),
		   struct rcu_segcblist *rsclp)
{
	char buf0[32];
	char buf1[32];
	char buf2[32];
	char err1[32];
	char err2[32];
	int i;
	char *s;

    	for (i = 0; i < 16; i++) {
		init_rh(i, rsclp);
		fmt_rh(rsclp, buf0);
		f(rsclp, err1);
		fmt_rh(rsclp, buf1);
		if (strcmp(buf0, buf1) == 0)
			s = "==";
		else
			s = "->";
		printf("%2d: %-16s %s %-16s%s\n", i, buf0, s, buf1, err1);
	}
}

static int have_holes(struct rcu_segcblist *rsclp)
{
	int i;

	for (i = RCU_DONE_TAIL + 1; i < RCU_NEXT_TAIL - 1; i++) {
		if (rsclp->tails[i - 1] == rsclp->tails[i] &&
		    rsclp->tails[i] != rsclp->tails[i + 1])
			return 1;
	}
	return 0;
}

static void
rcu_accelerate_cbs_test(struct rcu_segcblist *rsclp,
			char *s_in, unsigned long gp, unsigned long seq)
{
	int i;
	int had_hole = have_holes(rsclp);
	char *s = s_in;
	char *label = "DWRN";

	if (!rcu_segcblist_restempty(rsclp, RCU_DONE_TAIL))
		rcu_segcblist_accelerate(rsclp, seq);

	/*
	 * Check for ->gp_seq out of order, but ignore both
	 * RCU_DONE_TAIL and RCU_NEXT_TAIL, for which ->gp_seq
	 * is meaningless.
	 */
	for (i = RCU_WAIT_TAIL; i < RCU_NEXT_READY_TAIL; i++) {
		if (ULONG_CMP_LT(rsclp->gp_seq[i + 1],
				 rsclp->gp_seq[i])) {
			*s++ = '?';
			*s++ = 'C';
			*s++ = label[i];
			break;
		}
	}
	
	/* Check for holes in the list. */
	if (!had_hole && have_holes(rsclp))
		*s++ = 'H';
	else if (have_holes(rsclp))
		*s++ = 'h';

	*s++ = '\0';
}

static void
rcu_advance_cbs_test(struct rcu_segcblist *rsclp, char *s_in,
		     unsigned long gp, unsigned long seq)
{
	int i;
	char *s = s_in;
	char *label = "DWRN";

	if (!rcu_segcblist_restempty(rsclp, RCU_DONE_TAIL)) {
		rcu_segcblist_advance(rsclp, gp);
		if (!rcu_segcblist_restempty(rsclp, RCU_DONE_TAIL))
			rcu_segcblist_accelerate(rsclp, seq);
	}

	/*
	 * Check for ->gp_seq out of order, but ignore both
	 * RCU_DONE_TAIL and RCU_NEXT_TAIL, for which ->gp_seq
	 * is meaningless.
	 */
	for (i = RCU_WAIT_TAIL; i < RCU_NEXT_READY_TAIL; i++) {
		if (ULONG_CMP_LT(rsclp->gp_seq[i + 1],
				 rsclp->gp_seq[i])) {
			*s++ = '?';
			*s++ = 'C';
			*s++ = label[i];
			break;
		}
	}

	/*
	 * Check for empty lists followed by non-empty lists, which
	 * would indicate that rcu_advance_cbs() is not compacting
	 * properly.  It is OK for RCU_DONE_TAIL to be empty and
	 * later lists to be non-empty, however.
	 */
	for (i = RCU_WAIT_TAIL; i < RCU_NEXT_READY_TAIL; i++) {
		if (rsclp->tails[i] == rsclp->tails[i + 1] &&
		    rsclp->tails[i + 1] != rsclp->tails[i + 2]) {
			*s++ = '?';
			*s++ = 'E';
			*s++ = label[i];
			break;
		}
	}

	/* The RCU_NEXT_LIST must be empty. */
	if (*rsclp->tails[RCU_NEXT_READY_TAIL]) {
		*s++ = '?';
		*s++ = 'N';
	}
	
	/* Check for holes in the list. */
	if (have_holes(rsclp))
		*s++ = 'H';
	*s++ = '\0';
}

static void
rcu_extract_cbs_test(struct rcu_segcblist *rsclp, char *s_in,
		     unsigned long gp, unsigned long seq)
{
	int i;
	char *s = s_in;
	char *label = "DWRN";
	long ql, qll;
	struct rcu_cblist rcl_done = RCU_CBLIST_INITIALIZER(rcl_done);
	struct rcu_cblist rcl_next;

	rcu_segcblist_fsck(rsclp, 10);
	rcu_cblist_init(&rcl_next);
	ql = rcu_segcblist_n_cbs(rsclp);
	qll = rcu_segcblist_n_lazy_cbs(rsclp);
	rcu_segcblist_extract_count(rsclp, &rcl_done);
	rcu_segcblist_extract_done_cbs(rsclp, &rcl_done);
	rcu_segcblist_extract_pend_cbs(rsclp, &rcl_next);
	rcu_segcblist_fsck(rsclp, 10);
	if (!rcu_segcblist_empty(rsclp)) {
		*s++ = '?';
		*s++ = 'E';
	}
	if (rcu_segcblist_n_cbs(rsclp)) {
		*s++ = '?';
		*s++ = 'C';
	}
	if (rcu_segcblist_n_lazy_cbs(rsclp)) {
		*s++ = '?';
		*s++ = 'L';
	}
	rcu_segcblist_insert_count(rsclp, &rcl_done);
	rcu_segcblist_insert_done_cbs(rsclp, &rcl_done);
	rcu_segcblist_insert_pend_cbs(rsclp, &rcl_next);
	assert(ql == rcu_segcblist_n_cbs(rsclp));
	assert(qll == rcu_segcblist_n_lazy_cbs(rsclp));
	if (!rcu_segcblist_segempty(rsclp, RCU_WAIT_TAIL)) {
		*s++ = '?';
		*s++ = 'C';
		*s++ = label[RCU_WAIT_TAIL];
	}
	if (!rcu_segcblist_segempty(rsclp, RCU_NEXT_READY_TAIL)) {
		*s++ = '?';
		*s++ = 'C';
		*s++ = label[RCU_NEXT_READY_TAIL];
	}
	
	/* Check for holes in the list. */
	if (have_holes(rsclp))
		*s++ = 'H';
	*s++ = '\0';
}

static void
rcu_entrain_cbs_test(struct rcu_segcblist *rsclp, char *s_in)
{
	bool empty;
	static struct rcu_head entrain_rh;
	int i;
	char *label = "DWRN";
	long ql, qll;
	bool retval;
	char *s = s_in;
	bool segs[RCU_CBLIST_NSEGS];

	rcu_segcblist_fsck(rsclp, 10);
	empty = rcu_segcblist_empty(rsclp);
	ql = rcu_segcblist_n_cbs(rsclp);
	qll = rcu_segcblist_n_lazy_cbs(rsclp);
	for (i = RCU_DONE_TAIL; i < RCU_CBLIST_NSEGS; i++)
		segs[i] = rcu_segcblist_segempty(rsclp, i);
	retval = rcu_segcblist_entrain(rsclp, &entrain_rh, false);
	rcu_segcblist_fsck(rsclp, 10);
	assert(empty != retval);
	assert(!empty || rcu_segcblist_empty(rsclp));
	if (!rcu_segcblist_empty(rsclp)) {
		assert(rcu_segcblist_n_cbs(rsclp) == ql + 1);
		assert(rcu_segcblist_n_lazy_cbs(rsclp) == qll);
	}
	for (i = RCU_DONE_TAIL; i < RCU_CBLIST_NSEGS; i++)
		assert(segs[i] == rcu_segcblist_segempty(rsclp, i));
	*s++ = '\0';
}

int main(int argc, char *argv[])
{
	int i;
	struct rcu_head *rhp;

	printf("Starting smoketest\n");
	rcu_segcblist_init(&rscl);
	assert(rcu_segcblist_empty(&rscl));
	assert(rcu_segcblist_n_cbs(&rscl) == 0);
	assert(rcu_segcblist_n_lazy_cbs(&rscl) == 0);
	assert(rcu_segcblist_n_nonlazy_cbs(&rscl) == 0);
	assert(rcu_segcblist_is_enabled(&rscl));
	for (i = RCU_DONE_TAIL; i < RCU_CBLIST_NSEGS; i++) {
		assert(rcu_segcblist_segempty(&rscl, i));
		assert(rcu_segcblist_restempty(&rscl, i));
	}
	assert(!rcu_segcblist_ready_cbs(&rscl));
	assert(!rcu_segcblist_pend_cbs(&rscl));
	assert(!rcu_segcblist_first_pend_cb(&rscl));
	assert(!rcu_segcblist_new_cbs(&rscl));
	rhp = rcu_segcblist_first_cb(&rscl);
	assert(rhp == NULL);
	assert(!rcu_segcblist_head(&rscl));

	rcu_segcblist_enqueue(&rscl, &rh[0], false);
	rhp = rcu_segcblist_first_cb(&rscl);
	assert(rhp == &rh[0]);
	assert(!rcu_segcblist_empty(&rscl));
	assert(rcu_segcblist_n_cbs(&rscl) == 1);
	assert(rcu_segcblist_n_lazy_cbs(&rscl) == 0);
	assert(rcu_segcblist_n_nonlazy_cbs(&rscl) == 1);
	assert(rcu_segcblist_first_pend_cb(&rscl) == &rh[0]);
#ifdef FORCE_FAILURE
	rcu_segcblist_disable(&rscl);
#endif /* #ifdef FORCE_FAILURE */
	for (i = RCU_DONE_TAIL; i < RCU_NEXT_TAIL; i++) {
		assert(rcu_segcblist_segempty(&rscl, i));
		assert(!rcu_segcblist_restempty(&rscl, i));
	}
	assert(!rcu_segcblist_segempty(&rscl, RCU_NEXT_TAIL));
	assert(rcu_segcblist_restempty(&rscl, RCU_NEXT_TAIL));
	assert(!rcu_segcblist_ready_cbs(&rscl));
	assert(rcu_segcblist_pend_cbs(&rscl));
	assert(rcu_segcblist_new_cbs(&rscl));
	assert(rcu_segcblist_head(&rscl) == &rh[0]);
	assert(rcu_segcblist_tail(&rscl) == &rh[0].next);

	rcu_cblist_init(&rcl[0]);
	assert(!rcl[0].head);
	assert(rcl[0].len == 0);
	assert(rcl[0].len_lazy == 0);
	assert(!rcu_cblist_head(&rcl[0]));
	assert(rcu_cblist_count_cbs(&rcl[0], 100) == 0);

	rcu_segcblist_extract_done_cbs(&rscl, &rcl[0]);
	rcu_segcblist_extract_pend_cbs(&rscl, &rcl[0]);
	rcu_segcblist_extract_count(&rscl, &rcl[0]);
	assert(rcu_segcblist_empty(&rscl));
	assert(rcl[0].head);
	assert(rcl[0].len == 1);
	assert(rcl[0].len_lazy == 0);
	assert(rcu_cblist_head(&rcl[0]) == &rh[0]);
	assert(rcu_cblist_tail(&rcl[0]) == &rh[0].next);
	assert(rcu_cblist_count_cbs(&rcl[0], 100) == 1);

	rhp = rcu_cblist_dequeue(&rcl[0]);
	assert(rhp == &rh[0]);
	assert(!rcl[0].head);
	assert(rcl[0].len == 0);
	assert(rcl[0].len_lazy == 0);
	rhp = rcu_cblist_dequeue(&rcl[0]);
	assert(rhp == NULL);
	assert(rcu_cblist_count_cbs(&rcl[0], 100) == 0);

	rcu_segcblist_init(&rscl);
	rcu_segcblist_fsck(&rscl, 10);
	rcu_segcblist_enqueue(&rscl, &rh[0], false);
	rcu_segcblist_fsck(&rscl, 10);
	rcu_segcblist_accelerate(&rscl, 5);
	rcu_segcblist_fsck(&rscl, 10);
	rcu_segcblist_enqueue(&rscl, &rh[1], false);
	rcu_segcblist_advance(&rscl, 5);
	rcu_segcblist_accelerate(&rscl, 6);
	rcu_segcblist_enqueue(&rscl, &rh[2], true);
	assert(rcu_segcblist_dequeue(&rscl) == &rh[0]);
	assert(rcu_segcblist_dequeue(&rscl) == NULL);
	rcu_segcblist_advance(&rscl, 6);
	rcu_segcblist_accelerate(&rscl, 7);
	assert(rcu_segcblist_dequeue(&rscl) == &rh[1]);
	assert(rcu_segcblist_dequeue(&rscl) == NULL);
	rcu_segcblist_advance(&rscl, 7);
	assert(rcu_segcblist_dequeue(&rscl) == &rh[2]);
	rcu_segcblist_dequeued_lazy(&rscl);
	assert(rcu_segcblist_dequeue(&rscl) == NULL);
	assert(rcu_segcblist_empty(&rscl));

	rcu_segcblist_disable(&rscl);
	assert(!rcu_segcblist_is_enabled(&rscl));
	printf("Done with smoketest\n");

	printf("\n--- rcu_accelerate_cbs(!in_gp, root):\n");
	runtest(rcu_accelerate_cbs_test, &rscl, false, true);
	printf("\n--- rcu_accelerate_cbs(!in_gp, !root):\n");
	runtest(rcu_accelerate_cbs_test, &rscl, false, false);
	printf("\n--- rcu_accelerate_cbs(in_gp, root):\n");
	runtest(rcu_accelerate_cbs_test, &rscl, true, true);
	printf("\n--- rcu_accelerate_cbs(in_gp, !root):\n");
	runtest(rcu_accelerate_cbs_test, &rscl, true, false);

	printf("\n--- rcu_advance_cbs(!in_gp, root):\n");
	runtest(rcu_advance_cbs_test, &rscl, false,  true);
	printf("\n--- rcu_advance_cbs(!in_gp, !root):\n");
	runtest(rcu_advance_cbs_test, &rscl, false, false);
	printf("\n--- rcu_advance_cbs(in_gp, root):\n");
	runtest(rcu_advance_cbs_test, &rscl, true,  true);
	printf("\n--- rcu_advance_cbs(in_gp, !root):\n");
	runtest(rcu_advance_cbs_test, &rscl, true, false);

	printf("\n--- rcu_extract_cbs_test(!in_gp, root):\n");
	runtest(rcu_extract_cbs_test, &rscl, false, true);

	printf("\n--- rcu_entrain_cbs_test(!in_gp, root):\n");
	runtest_no_gp(rcu_entrain_cbs_test, &rscl);

	return 0;
}
