#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>

/* desired PI precision in thousands of digits */
static int gDigThPrec;

/* calculation precision in 4-byte words */
static unsigned gWordPrec;

static pthread_mutex_t gMutex;
static pthread_cond_t gCond;
static uint32_t *pi;
static unsigned gInProgressCount;
static unsigned gNextComp;
static unsigned gCompCount;
static unsigned gProgressStep, gNextProgress;
static unsigned gProdPipeCount;

/* To print PI, the calculated value is multiplied several times by 1e9.
 * Products are computed piecewise. Each thread calculates own piece.
 * Overflow value is transferred to next thread.
 */
struct ProdPipeline {
	pthread_t thread;
	struct ProdPipeline *prev;
	unsigned first, prec;		// PI piece to multiply
	uint32_t overflows[2];
	int overflowCount;
};

/* Adds to sum i-th component of Pi
 * Pi is calculated as:
 *		 pi = 0;
 *		 for(i = 0; i < gCompCount; ++i)
 *			  calc_pi_component(pi, i);
 */
static void calc_pi_component(uint32_t *sum, unsigned i)
{
	int firstnz, curnz, p2, j, sumOvl, loc;
	uint32_t dig, dig0, remainders[7], div[7];
	uint64_t dividend, resSum;

	p2 = 29 + 10 * i;
	firstnz = p2 >> 5;
	if( firstnz >= gWordPrec )
		return;
	dig0 = 0x80000000U >> (p2 & 0x1f);

	div[0] = 10 * i + 1;
	div[1] = 2560 * i + 2304;
	div[2] = 32 * i + 8;
	div[3] = 1024 * i + 768;
	div[4] = 40 * i + 12;
	div[5] = 640 * i + 320;
	div[6] = 640 * i + 448;

	for(curnz = firstnz; curnz < gWordPrec; ++curnz) {
		resSum = sum[curnz];
		for(j = 0; j < 7; ++j) {
			if( curnz == firstnz ) {
				if( (i&1) == (j<2) ) {	// dividend is negative
					dividend = ((uint64_t)div[j] << 32) - dig0;
					resSum -= 1LL << 32;
				}else
					dividend = dig0;
			}else
				dividend = ((uint64_t)remainders[j] << 32);
			dig = dividend / div[j];
			remainders[j] = dividend - dig * div[j];
			resSum += dig;
		}
		sum[curnz] = resSum;
		sumOvl = (int32_t)(resSum>>32);
		for(loc = curnz - 1; sumOvl && loc >= 0; --loc ) {
			resSum = (int64_t)sum[loc] + sumOvl;
			sum[loc] = resSum;
			sumOvl = (int32_t)(resSum>>32);
		}
	}
}

void *threadFun(void *arg)
{
	struct ProdPipeline *pp = arg;
	uint32_t *pi_part, overflow;
	uint64_t sum;
	unsigned num;
	int idx;

	pi_part = calloc(gWordPrec, sizeof(uint32_t));
	while( 1 ) {
		pthread_mutex_lock(&gMutex);
		num = gNextComp;
		if( num < gCompCount ) {
			if( ++gNextComp >= gNextProgress ) {
				fputc('.', stderr);
				gNextProgress += gProgressStep;
				if( gNextProgress >= gCompCount )
					fputc('\n', stderr);
			}
		}else{
			// pi += pi_part
			sum = 0;
			for(idx = gWordPrec - 1; idx >= 0; --idx)
				pi[idx] = sum = (sum >> 32) + pi[idx] + pi_part[idx];
			// wait until pi is calculated
			if( --gInProgressCount == 0 )
				pthread_cond_broadcast(&gCond);
			else{
				while( gInProgressCount )
					pthread_cond_wait(&gCond, &gMutex);
			}
		}
		pthread_mutex_unlock(&gMutex);
		if( num == gCompCount )
			break;
		calc_pi_component(pi_part, num);
	}
	free(pi_part);

	for(num = 0; num < gProdPipeCount; ++num) {
		if( pp->prev ) {
			int isReady = 0;
			while( ! isReady ) {
				pthread_mutex_lock(&gMutex);
				isReady = pp->prev->overflowCount > 0;
				if( isReady ) {
					overflow = pp->prev->overflows[0];
					if( --pp->prev->overflowCount )
						pp->prev->overflows[0] = pp->prev->overflows[1];
				}
				pthread_mutex_unlock(&gMutex);
			}
		}else
			overflow = 0;
		sum = (uint64_t)overflow << 32;
		for(idx = pp->first + pp->prec-1; idx >= pp->first; --idx)
			pi[idx] = sum = 1000000000ULL * pi[idx] + (sum >> 32);
		overflow = sum >> 32;
		if( pp->first == 1 ) {
			if( num == 0 )
				printf("%u.", pi[0]);
			if( num < gProdPipeCount - 1 )
				printf("%09u", overflow);
			else{
			   	if( (gDigThPrec * 1000) % 9 ) {
					char buf[10];
					sprintf(buf, "%09u", overflow);
					printf("%.*s", (gDigThPrec * 1000) % 9, buf);
				}
				printf("\n");
			}
		}else{
			int isReady = 0;
			while( ! isReady ) {
				pthread_mutex_lock(&gMutex);
				isReady = pp->overflowCount < 2;
				if( isReady )
					pp->overflows[pp->overflowCount++] = overflow;
				pthread_mutex_unlock(&gMutex);
			}
		}
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	struct ProdPipeline *pp;
	struct timeval tm_beg, tm_end, tm_diff;
	int i, cpuCount;

	if( argc == 1 ) {
		fprintf(stderr, "usage:\n");
		fprintf(stderr, "      pi <thousands of digits>\n\n");
		return 0;
	}
	gDigThPrec = atoi(argv[1]);
	if( gDigThPrec < 1 || gDigThPrec > 10000 ) {
		fprintf(stderr, "  argument out of range\n");
		return 1;
	}
	gWordPrec = 104 * gDigThPrec + 2;
	gCompCount = (32 * gWordPrec + 11) / 10;
	// expression: 2560 * gCompCount + 2304 cannot exceed UINT_MAX
	if( gCompCount > 1677721 ) {
		fprintf(stderr, "precision too big\n");
		return 1;
	}
	gettimeofday(&tm_beg, NULL);
	pthread_mutex_init(&gMutex, NULL);
	pthread_cond_init(&gCond, NULL);
	gProgressStep = gCompCount / 81;
	gNextProgress = gCompCount - 80 * gProgressStep + 1;
	cpuCount = sysconf(_SC_NPROCESSORS_ONLN);
	pp = malloc(cpuCount * sizeof(struct ProdPipeline));
	pi = calloc(gWordPrec, sizeof(uint32_t));
	gInProgressCount = cpuCount;
	gProdPipeCount = gDigThPrec * 1000 / 9 + 1;
	for(i = 0; i < cpuCount; ++i) {
		pp[i].first = 1 + (i * (gWordPrec-1)) / cpuCount;
		pp[i].prec = ((i+1) * (gWordPrec-1)) / cpuCount -
			(i*(gWordPrec-1)) / cpuCount;
		pp[i].prev = i == cpuCount - 1 ? NULL : pp + i + 1;
		pp[i].overflowCount = 0;
		pthread_create(&pp[i].thread, NULL, threadFun, pp + i);
	}
	for(i = 0; i < cpuCount; ++i)
		pthread_join(pp[i].thread, NULL);
	free(pp);
	free(pi);
	gettimeofday(&tm_end, NULL);
	timersub(&tm_end, &tm_beg, &tm_diff);
	fprintf(stderr, "exec time: ");
	if( tm_diff.tv_sec >= 60 ) {
		fprintf(stderr, "%ld min  %ld s\n",
				tm_diff.tv_sec / 60, tm_diff.tv_sec % 60);
	}else
		fprintf(stderr, "%.3f s\n",
				tm_diff.tv_sec + (tm_diff.tv_usec/1000000.0));
	return 0;
}

