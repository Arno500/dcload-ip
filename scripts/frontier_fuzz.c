/* Does the win_mask frontier accounting tell the truth?
 *
 * Replicates three implementations of "contiguous bytes received" over a stream
 * of chunk arrivals in arbitrary order, and compares them against a brute-force
 * reference that keeps a real per-chunk bitmap.
 *
 *   ref   - ground truth: real bitmap, frontier = first unset chunk
 *   old   - shift only inside the absorption loop (what shipped for the cdfs
 *           slots, and what I copied into bin_info)
 *   new   - unconditional one-position rebase on every frontier advance
 *
 * A frontier ABOVE the reference is the fatal direction: dcload then announces
 * contiguous data it does not hold, the host stops resending a real hole, and
 * the read the game waits on can never complete. A frontier BELOW the reference
 * is merely slow (the host resends what we already have).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 1440U
#define MAXCH 64

typedef struct {
	unsigned int recv;      /* contiguous watermark */
	unsigned char mask;
	unsigned int total;
} acct_t;

static unsigned int chunk_off(unsigned int i) { return i * CHUNK; }

static unsigned int chunk_len(const acct_t *a, unsigned int i)
{
	unsigned int off = chunk_off(i);
	if (off + CHUNK > a->total) return a->total - off;
	return CHUNK;
}

/* ---- old: shift only when absorbing --------------------------------------- */
static void old_arrive(acct_t *a, unsigned int off, unsigned int size)
{
	if ((off <= a->recv) && ((off + size) > a->recv)) {
		a->recv = off + size;
		while ((a->mask & 1u) && (a->recv < a->total)) {
			unsigned int step = CHUNK;
			a->mask = (unsigned char)(a->mask >> 1);
			if (a->recv + step > a->total) step = a->total - a->recv;
			a->recv += step;
		}
		if (a->recv >= a->total) a->mask = 0;
	} else if (off > a->recv) {
		unsigned int k = (off - a->recv) / CHUNK;
		if (k >= 1u && k <= 8u) a->mask |= (unsigned char)(1u << (k - 1u));
	}
}

/* ---- new: unconditional rebase on every frontier advance ------------------ */
static void new_arrive(acct_t *a, unsigned int off, unsigned int size)
{
	if ((off <= a->recv) && ((off + size) > a->recv)) {
		unsigned int absorbed;
		a->recv = off + size;
		absorbed = a->mask & 1u;
		a->mask = (unsigned char)(a->mask >> 1);
		while (absorbed && (a->recv < a->total)) {
			unsigned int step = CHUNK;
			if (a->recv + step > a->total) step = a->total - a->recv;
			a->recv += step;
			absorbed = a->mask & 1u;
			a->mask = (unsigned char)(a->mask >> 1);
		}
		if (a->recv >= a->total) a->mask = 0;
	} else if (off > a->recv) {
		unsigned int k = (off - a->recv) / CHUNK;
		if (k >= 1u && k <= 8u) a->mask |= (unsigned char)(1u << (k - 1u));
	}
}

int main(void)
{
	unsigned long seed = 12345;
	int trial;
	long old_over = 0, old_under = 0, new_over = 0, new_under = 0;
	long trials = 200000;
	int first_new_over = -1, first_old_over = -1;

	for (trial = 0; trial < trials; trial++) {
		unsigned int nch = 3 + (unsigned int)(seed % 20);
		unsigned int total, i, step;
		unsigned char got[MAXCH];
		unsigned int order[MAXCH];
		acct_t o, n;
		unsigned int ref;

		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		total = nch * CHUNK - (unsigned int)(seed % CHUNK); /* short last chunk */
		if (total == 0) continue;
		nch = (total + CHUNK - 1) / CHUNK;

		memset(got, 0, sizeof got);
		for (i = 0; i < nch; i++) order[i] = i;
		/* shuffle */
		for (i = nch; i > 1; i--) {
			unsigned int j;
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			j = (unsigned int)(seed >> 33) % i;
			step = order[i - 1]; order[i - 1] = order[j]; order[j] = step;
		}

		memset(&o, 0, sizeof o); o.total = total;
		memset(&n, 0, sizeof n); n.total = total;

		for (i = 0; i < nch; i++) {
			unsigned int c = order[i];
			got[c] = 1;
			old_arrive(&o, chunk_off(c), chunk_len(&o, c));
			new_arrive(&n, chunk_off(c), chunk_len(&n, c));

			/* reference frontier: bytes up to the first missing chunk */
			ref = 0;
			{
				unsigned int j;
				for (j = 0; j < nch; j++) {
					if (!got[j]) break;
					ref += chunk_len(&o, j);
				}
			}
			if (o.recv > ref) { old_over++; if (first_old_over < 0) first_old_over = trial; }
			if (o.recv < ref) old_under++;
			if (n.recv > ref) { new_over++; if (first_new_over < 0) first_new_over = trial; }
			if (n.recv < ref) new_under++;
		}
	}

	printf("trials=%ld  (each = one full read delivered in random chunk order)\n\n", trials);
	printf("               OVER-report (FATAL)   UNDER-report (slow only)\n");
	printf("old (shipped)  %-20ld %ld\n", old_over, old_under);
	printf("new (rebased)  %-20ld %ld\n", new_over, new_under);
	if (first_old_over >= 0) printf("\nfirst old over-report at trial %d\n", first_old_over);
	if (first_new_over >= 0) printf("first new over-report at trial %d\n", first_new_over);
	return 0;
}
