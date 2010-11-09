#include <assert.h>
#include <ctype.h>
#include "bam.h"
#include "bam2bcf.h"
#include "ksort.h"
#include "kaln.h"

#define MINUS_CONST 0x10000000
#define INDEL_WINDOW_SIZE 50

static int tpos2qpos(const bam1_core_t *c, const uint32_t *cigar, int32_t tpos, int is_left, int32_t *_tpos)
{
	int k, x = c->pos, y = 0, last_y = 0;
	*_tpos = c->pos;
	for (k = 0; k < c->n_cigar; ++k) {
		int op = cigar[k] & BAM_CIGAR_MASK;
		int l = cigar[k] >> BAM_CIGAR_SHIFT;
		if (op == BAM_CMATCH) {
			if (c->pos > tpos) return y;
			if (x + l > tpos) {
				*_tpos = tpos;
				return y + (tpos - x);
			}
			x += l; y += l;
			last_y = y;
		} else if (op == BAM_CINS || op == BAM_CSOFT_CLIP) y += l;
		else if (op == BAM_CDEL || op == BAM_CREF_SKIP) {
			if (x + l > tpos) {
				*_tpos = is_left? x : x + l;
				return y;
			}
			x += l;
		}
	}
	*_tpos = x;
	return last_y;
}
// FIXME: check if the inserted sequence is consistent with the homopolymer run
// l is the relative gap length and l_run is the length of the homopolymer on the reference
static inline int est_seqQ(const bcf_callaux_t *bca, int l, int l_run)
{
	int q, qh;
	q = bca->openQ + bca->extQ * (abs(l) - 1);
	qh = l_run >= 3? (int)(bca->tandemQ * (double)abs(l) / l_run + .499) : 1000;
	return q < qh? q : qh;
}

static inline int est_indelreg(int pos, const char *ref, int l, char *ins4)
{
	int i, j, max = 0, max_i = pos, score = 0;
	l = abs(l);
	for (i = pos + 1, j = 0; ref[i]; ++i, ++j) {
		if (ins4) score += (toupper(ref[i]) != "ACGTN"[(int)ins4[j%l]])? -10 : 1;
		else score += (toupper(ref[i]) != toupper(ref[pos+1+j%l]))? -10 : 1;
		if (score < 0) break;
		if (max < score) max = score, max_i = i;
	}
	return max_i - pos;
}

int bcf_call_gap_prep(int n, int *n_plp, bam_pileup1_t **plp, int pos, bcf_callaux_t *bca, const char *ref)
{
	extern void ks_introsort_uint32_t(int, uint32_t*);
	int i, s, j, k, t, n_types, *types, max_rd_len, left, right, max_ins, *score, N, K, l_run, ref_type;
	char *inscns = 0, *ref2, *query;
	if (ref == 0 || bca == 0) return -1;
	// determine if there is a gap
	for (s = N = 0; s < n; ++s) {
		for (i = 0; i < n_plp[s]; ++i)
			if (plp[s][i].indel != 0) break;
		if (i < n_plp[s]) break;
	}
	if (s == n) return -1; // there is no indel at this position.
	for (s = N = 0; s < n; ++s) N += n_plp[s]; // N is the total number of reads
	{ // find out how many types of indels are present
		int m;
		uint32_t *aux;
		aux = calloc(N + 1, 4);
		m = max_rd_len = 0;
		aux[m++] = MINUS_CONST; // zero indel is always a type
		for (s = 0; s < n; ++s) {
			for (i = 0; i < n_plp[s]; ++i) {
				const bam_pileup1_t *p = plp[s] + i;
				if (p->indel != 0)
					aux[m++] = MINUS_CONST + p->indel;
				j = bam_cigar2qlen(&p->b->core, bam1_cigar(p->b));
				if (j > max_rd_len) max_rd_len = j;
			}
		}
		ks_introsort(uint32_t, m, aux);
		// squeeze out identical types
		for (i = 1, n_types = 1; i < m; ++i)
			if (aux[i] != aux[i-1]) ++n_types;
		assert(n_types > 1); // there must at least one type of non-reference indel
		types = (int*)calloc(n_types, sizeof(int));
		t = 0;
		types[t++] = aux[0] - MINUS_CONST; 
		for (i = 1; i < m; ++i)
			if (aux[i] != aux[i-1])
				types[t++] = aux[i] - MINUS_CONST;
		free(aux);
		for (t = 0; t < n_types; ++t)
			if (types[t] == 0) break;
		ref_type = t; // the index of the reference type (0)
		assert(n_types < 64);
	}
	{ // calculate left and right boundary
		left = pos > INDEL_WINDOW_SIZE? pos - INDEL_WINDOW_SIZE : 0;
		right = pos + INDEL_WINDOW_SIZE;
		if (types[0] < 0) right -= types[0];
		// in case the alignments stand out the reference
		for (i = pos; i < right; ++i)
			if (ref[i] == 0) break;
		right = i;
	}
	{ // the length of the homopolymer run around the current position
		int c = bam_nt16_table[(int)ref[pos + 1]];
		if (c == 15) l_run = 1;
		else {
			for (i = pos + 2; ref[i]; ++i)
				if (bam_nt16_table[(int)ref[i]] != c) break;
			l_run = i;
			for (i = pos; i >= 0; --i)
				if (bam_nt16_table[(int)ref[i]] != c) break;
			l_run -= i + 1;
		}
	}
	// construct the consensus sequence
	max_ins = types[n_types - 1]; // max_ins is at least 0
	if (max_ins > 0) {
		int *inscns_aux = calloc(4 * n_types * max_ins, sizeof(int));
		// count the number of occurrences of each base at each position for each type of insertion
		for (t = 0; t < n_types; ++t) {
			if (types[t] > 0) {
				for (s = 0; s < n; ++s) {
					for (i = 0; i < n_plp[s]; ++i) {
						bam_pileup1_t *p = plp[s] + i;
						if (p->indel == types[t]) {
							uint8_t *seq = bam1_seq(p->b);
							for (k = 1; k <= p->indel; ++k) {
								int c = bam_nt16_nt4_table[bam1_seqi(seq, p->qpos + k)];
								if (c < 4) ++inscns_aux[(t*max_ins+(k-1))*4 + c];
							}
						}
					}
				}
			}
		}
		// use the majority rule to construct the consensus
		inscns = calloc(n_types * max_ins, 1);
		for (t = 0; t < n_types; ++t) {
			for (j = 0; j < types[t]; ++j) {
				int max = 0, max_k = -1, *ia = &inscns_aux[(t*max_ins+j)*4];
				for (k = 0; k < 4; ++k)
					if (ia[k] > max)
						max = ia[k], max_k = k;
				inscns[t*max_ins + j] = max? max_k : 4;
			}
		}
		free(inscns_aux);
	}
	// compute the likelihood given each type of indel for each read
	ref2  = calloc(right - left + max_ins + 2, 1);
	query = calloc(right - left + max_rd_len + max_ins + 2, 1);
	score = calloc(N * n_types, sizeof(int));
	bca->indelreg = 0;
	for (t = 0; t < n_types; ++t) {
		int l, ir;
		ka_param2_t ap = ka_param2_qual;
		ap.band_width = abs(types[t]) + 3;
		// compute indelreg
		if (types[t] == 0) ir = 0;
		else if (types[t] > 0) ir = est_indelreg(pos, ref, types[t], &inscns[t*max_ins]);
		else ir = est_indelreg(pos, ref, -types[t], 0);
		if (ir > bca->indelreg) bca->indelreg = ir;
//		fprintf(stderr, "%d, %d, %d\n", pos, types[t], ir);
		// write ref2
		for (k = 0, j = left; j <= pos; ++j)
			ref2[k++] = bam_nt16_nt4_table[bam_nt16_table[(int)ref[j]]];
		if (types[t] <= 0) j += -types[t];
		else for (l = 0; l < types[t]; ++l)
				 ref2[k++] = inscns[t*max_ins + l];
		if (types[0] < 0) { // mask deleted sequences to avoid a particular error in the model.
			int jj, tmp = types[t] >= 0? -types[0] : -types[0] + types[t];
			for (jj = 0; jj < tmp && j < right && ref[j]; ++jj, ++j)
				ref2[k++] = 4;
		}
		for (; j < right && ref[j]; ++j)
			ref2[k++] = bam_nt16_nt4_table[bam_nt16_table[(int)ref[j]]];
		if (j < right) right = j;
		// align each read to ref2
		for (s = K = 0; s < n; ++s) {
			for (i = 0; i < n_plp[s]; ++i, ++K) {
				bam_pileup1_t *p = plp[s] + i;
				int qbeg, qend, tbeg, tend, sc;
				uint8_t *seq = bam1_seq(p->b);
				// determine the start and end of sequences for alignment
				qbeg = tpos2qpos(&p->b->core, bam1_cigar(p->b), left,  0, &tbeg);
				qend = tpos2qpos(&p->b->core, bam1_cigar(p->b), right, 1, &tend);
				if (types[t] < 0) {
					int l = -types[t];
					tbeg = tbeg - l > left?  tbeg - l : left;
				}
				// write the query sequence
				for (l = qbeg; l < qend; ++l)
					query[l - qbeg] = bam_nt16_nt4_table[bam1_seqi(seq, l)];
				// do alignment; this takes most of computing time for indel calling
				sc = ka_global_score((uint8_t*)ref2 + tbeg - left, tend - tbeg + abs(types[t]),
									 (uint8_t*)query, qend - qbeg, &ap);
				score[K*n_types + t] = -sc;
/*
				for (l = 0; l < tend - tbeg + abs(types[t]); ++l)
					fputc("ACGTN"[(int)ref2[tbeg-left+l]], stderr);
				fputc('\n', stderr);
				for (l = 0; l < qend - qbeg; ++l) fputc("ACGTN"[(int)query[l]], stderr);
				fputc('\n', stderr);
				fprintf(stderr, "pos=%d type=%d read=%d:%d name=%s qbeg=%d tbeg=%d score=%d\n", pos, types[t], s, i, bam1_qname(p->b), qbeg, tbeg, sc);
*/
			}
		}
	}
	free(ref2); free(query);
	{ // compute indelQ
		int *sc, tmp, *sumq;
		sc   = alloca(n_types * sizeof(int));
		sumq = alloca(n_types * sizeof(int));
		memset(sumq, 0, sizeof(int) * n_types);
		for (s = K = 0; s < n; ++s) {
			for (i = 0; i < n_plp[s]; ++i, ++K) {
				bam_pileup1_t *p = plp[s] + i;
				int *sct = &score[K*n_types], indelQ, seqQ;
				for (t = 0; t < n_types; ++t) sc[t] = sct[t]<<6 | t;
				for (t = 1; t < n_types; ++t) // insertion sort
					for (j = t; j > 0 && sc[j] < sc[j-1]; --j)
						tmp = sc[j], sc[j] = sc[j-1], sc[j-1] = tmp;
				/* errmod_cal() assumes that if the call is wrong, the
				 * likelihoods of other events are equal. This is about
				 * right for substitutions, but is not desired for
				 * indels. To reuse errmod_cal(), I have to make
				 * compromise for multi-allelic indels.
				 */
				if ((sc[0]&0x3f) == ref_type) {
					indelQ = (sc[1]>>6) - (sc[0]>>6);
					seqQ = est_seqQ(bca, types[sc[1]&0x3f], l_run);
				} else {
					for (t = 0; t < n_types; ++t) // look for the reference type
						if ((sc[t]&0x3f) == ref_type) break;
					indelQ = (sc[t]>>6) - (sc[0]>>6);
					seqQ = est_seqQ(bca, types[sc[0]&0x3f], l_run);
				}
				p->aux = (sc[0]&0x3f)<<16 | seqQ<<8 | indelQ;
				sumq[sc[0]&0x3f] += indelQ < seqQ? indelQ : seqQ;
//				fprintf(stderr, "pos=%d read=%d:%d name=%s call=%d q=%d\n", pos, s, i, bam1_qname(p->b), types[sc[0]&0x3f], indelQ);
			}
		}
		// determine bca->indel_types[] and bca->inscns
		bca->maxins = max_ins;
		bca->inscns = realloc(bca->inscns, bca->maxins * 4);
		for (t = 0; t < n_types; ++t)
			sumq[t] = sumq[t]<<6 | t;
		for (t = 1; t < n_types; ++t) // insertion sort
			for (j = t; j > 0 && sumq[j] < sumq[j-1]; --j)
				tmp = sumq[j], sumq[j] = sumq[j-1], sumq[j-1] = tmp;
		for (t = 0; t < n_types; ++t) // look for the reference type
			if ((sumq[t]&0x3f) == ref_type) break;
		if (t) { // then move the reference type to the first
			tmp = sumq[t];
			for (; t > 0; --t) sumq[t] = sumq[t-1];
			sumq[0] = tmp;
		}
		for (t = 0; t < 4; ++t) bca->indel_types[t] = B2B_INDEL_NULL;
		for (t = 0; t < 4 && t < n_types; ++t) {
			bca->indel_types[t] = types[sumq[t]&0x3f];
			memcpy(&bca->inscns[t * bca->maxins], &inscns[(sumq[t]&0x3f) * max_ins], bca->maxins);
		}
		// update p->aux
		for (s = K = 0; s < n; ++s) {
			for (i = 0; i < n_plp[s]; ++i, ++K) {
				bam_pileup1_t *p = plp[s] + i;
				int x = types[p->aux>>16&0x3f];
				for (j = 0; j < 4; ++j)
					if (x == bca->indel_types[j]) break;
				p->aux = j<<16 | (j == 4? 0 : (p->aux&0xffff));
//				fprintf(stderr, "pos=%d read=%d:%d name=%s call=%d q=%d\n", pos, s, i, bam1_qname(p->b), p->aux>>16&63, p->aux&0xff);
			}
		}		
	}
	free(score);
	// free
	free(types); free(inscns);
	return 0;
}
