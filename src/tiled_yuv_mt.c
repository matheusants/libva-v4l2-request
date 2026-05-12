/*
 * Parallel tiled_to_planar dispatcher.
 * Splits tile rows across all online CPU cores (up to MAX_THREADS).
 * Thread pool created once on first call; reused for every plane copy.
 */

#include "tiled_yuv.h"

#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <stdint.h>

#define MAX_THREADS 8

struct tile_work {
	const void   *src;
	void         *dst;
	unsigned int  pitch;
	unsigned int  width;
	unsigned int  height;
	unsigned int  tiles_per_row;
	unsigned int  ty_start;
	unsigned int  ty_end;
};

static int              n_threads;
static pthread_t        g_workers[MAX_THREADS - 1];
static struct tile_work g_tasks[MAX_THREADS];
static sem_t            g_start[MAX_THREADS - 1];
static sem_t            g_done[MAX_THREADS - 1];
static pthread_once_t   g_once  = PTHREAD_ONCE_INIT;
static pthread_mutex_t  g_mutex = PTHREAD_MUTEX_INITIALIZER;

static void run_range(const struct tile_work *t)
{
	unsigned int row_start = t->ty_start * 32;
	unsigned int row_end   = t->ty_end   * 32;

	if (row_start >= t->height)
		return;
	if (row_end > t->height)
		row_end = t->height;

	const char *src_off = (const char *)t->src +
		(size_t)t->ty_start * t->tiles_per_row * 1024;
	char *dst_off = (char *)t->dst + (size_t)row_start * t->pitch;

	tiled_to_planar((void *)src_off, dst_off, t->pitch,
			t->width, row_end - row_start);
}

static void *worker_func(void *arg)
{
	int id = (int)(intptr_t)arg;

	for (;;) {
		sem_wait(&g_start[id]);
		if (!g_tasks[id + 1].src)
			break;
		run_range(&g_tasks[id + 1]);
		sem_post(&g_done[id]);
	}
	return NULL;
}

static void init_pool(void)
{
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

	if (ncpu < 1)  ncpu = 1;
	if (ncpu > MAX_THREADS) ncpu = MAX_THREADS;
	n_threads = (int)ncpu;

	for (int i = 0; i < n_threads - 1; i++) {
		sem_init(&g_start[i], 0, 0);
		sem_init(&g_done[i],  0, 0);
		pthread_create(&g_workers[i], NULL, worker_func,
			       (void *)(intptr_t)i);
	}
}

void tiled_to_planar_mt(const void *src, void *dst, unsigned int pitch,
			unsigned int width, unsigned int height)
{
	pthread_once(&g_once, init_pool);
	pthread_mutex_lock(&g_mutex);

	unsigned int tile_rows     = (height + 31) >> 5;
	unsigned int tiles_per_row = (width  + 31) >> 5;
	int nt = n_threads;

	if ((unsigned int)nt > tile_rows)
		nt = (int)tile_rows;

	for (int i = 0; i < nt; i++) {
		g_tasks[i].src          = src;
		g_tasks[i].dst          = dst;
		g_tasks[i].pitch        = pitch;
		g_tasks[i].width        = width;
		g_tasks[i].height       = height;
		g_tasks[i].tiles_per_row = tiles_per_row;
		g_tasks[i].ty_start     = (tile_rows * (unsigned)i)       / (unsigned)nt;
		g_tasks[i].ty_end       = (tile_rows * (unsigned)(i + 1)) / (unsigned)nt;
	}

	/* signal workers (tasks[1..nt-1]) */
	for (int i = 1; i < nt; i++)
		sem_post(&g_start[i - 1]);

	/* main thread handles task[0] */
	run_range(&g_tasks[0]);

	/* wait for all workers */
	for (int i = 1; i < nt; i++)
		sem_wait(&g_done[i - 1]);

	pthread_mutex_unlock(&g_mutex);
}
