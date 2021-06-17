#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <map>
#include <set>
#include <vector>
#include <glib.h>
#include <bit-count.h>


#define mono_memory_barrier() 	{}
#define unwind_lock()	{}
#define unwind_unlock()		{}
#define mono_os_mutex_destroy(m)	{}

volatile int got_signal = 0;

static void on_signal(int s)
{
	if (write(2, "on_signal\n", 10) < 0) {
		perror("write");
	}
	got_signal = 1;
}

#ifdef OPTIMIZED
typedef struct {
	guint32 len;
	guint8 data [MONO_ZERO_LEN_ARRAY];
} MonoUnwindInfo;

/* CACHED_UNWIND_INFO_RANGE0_POF2 defines size of very first cached info range,
 * it _must_ be bigger than 2 for proper alignment of secondary array of pointers
 */
#define CACHED_UNWIND_INFO_RANGE0_POF2	10
#define CACHED_UNWIND_INFO_RANGE0_SIZE	(1 << (CACHED_UNWIND_INFO_RANGE0_POF2))

#if defined(__LP64__) || defined(_LP64)
# define CACHED_UNWIND_INFO_RANGES_COUNT	0x35 /* covers 64bit range starting from 1024 */
#else
# define CACHED_UNWIND_INFO_RANGES_COUNT	0x15 /* covers 32bit range starting from 1024 */
#endif

/* Actual cached info regions are here, each cached_info[..] element contains pointer to memory area that
 * has array of guint16-typed hashes in beginning followed by array of pointers to MonoUnwindInfo
 * such structure used to minimize allocations count and also to have layout most friendly to CPU cache:
 * as during linear search hash is compared first then its better to have all hashes as single compact array.
 *
 * cached_info[0] contains CACHED_UNWIND_INFO_RANGE0_SIZE elements, each next - twice more than previous.
 * Size of cached_info array is by one more than needed to hold all possible ranges to simplify loop conditions.
 */
typedef void *CachedUnwindInfoRangePtr;
static CachedUnwindInfoRangePtr cached_info[CACHED_UNWIND_INFO_RANGES_COUNT + 1];

#define CACHED_UNWIND_INFO_HASH(RANGE, INDEX)		( ((guint16 *)(RANGE))[INDEX] )

/* Returned pointer will be properly aligned as .._RANGE0_SIZE is power of 2 and bigger than 4 */
#define CACHED_UNWIND_INFO(RANGE, SIZE, INDEX)		( ( (MonoUnwindInfo **)(&((guint16 *)(RANGE))[SIZE]) )[INDEX] )

/* Very end of all cached indexes - essentially 'global' index of the next-added element */
static guint32 cached_count;

/* Statistics */
static int unwind_info_size;

void
mono_unwind_cleanup (void)
{
	CachedUnwindInfoRangePtr range;
	guint32 i, range_size, range_index;

	mono_os_mutex_destroy (&unwind_mutex);

	range_size = CACHED_UNWIND_INFO_RANGE0_SIZE;
	range_index = 0;

	/* release allocated unwind info-s and corresponding ranges */
	while (cached_info [range_index]) {
		range = cached_info [range_index];
		cached_info [range_index] = NULL;
		for (i = 0; cached_count != 0 && i < range_size; ++i, --cached_count) {
			g_free (CACHED_UNWIND_INFO(range, range_size, i));
		}
		g_free (range);
		range_size<<= 1;
		range_index ++;
	}

	/* make sure debit matches credit */
	g_assert (cached_count == 0);
}

static guint16
hash_unwind_info (guint8 *data, guint32 len)
{
	guint32 i, a;

	for (i = a = 0; i != len; ++i) {
		a ^= (((guint32)data[i]) << (i & 0xf));
	}

	a = (a & 0xffff) ^ (a >> 16);

	return (guint16)a;
}

static void *
unwind_info_malloc(size_t sz)
{
	unwind_info_size += sz;
	return g_malloc(sz);
}

/*
 * mono_cache_unwind_info
 *
 *   Save UNWIND_INFO in the unwind info cache and return an id which can be passed
 * to mono_get_cached_unwind_info to get a cached copy of the info.
 * A copy is made of the unwind info.
 * This function is useful for two reasons:
 * - many methods have the same unwind info
 * - MonoJitInfo->unwind_info is an int so it can't store the pointer to the unwind info
 */
guint32
mono_cache_unwind_info (guint8 *unwind_info, guint32 unwind_info_len)
{
	MonoUnwindInfo *info;
	CachedUnwindInfoRangePtr range;
	guint32 i, base_index, range_size, range_index;
	guint16 hash;

	hash = hash_unwind_info (unwind_info, unwind_info_len);

	range_size = CACHED_UNWIND_INFO_RANGE0_SIZE;
	range_index = 0;
	base_index = 0;

	/* First look for match in fully filled cached info ranges
	 * - i.e. in ranges from first to pre-last. This lookup doesn't
	 * need synchronization as these ranges are not modified anymore
	 */
	while ( cached_info [range_index + 1] ) {
		range = cached_info [range_index];
		for (i = 0; i < range_size; ++i) {
			if (CACHED_UNWIND_INFO_HASH(range, i) == hash) {
				info = CACHED_UNWIND_INFO(range, range_size, i);
				if (info->len == unwind_info_len && memcmp (info->data, unwind_info, unwind_info_len) == 0) {
					return base_index + i;
				}
			}
		}
		base_index += range_size;
		range_size <<= 1;
		range_index ++;
	}

	unwind_lock ();

	/* Continue looking for match til ranges end. Note that there may remain more than
	 * single range as while we worked not under lock - extra range(s) could be allocated.
	 */
	while ( ( range = cached_info [range_index] ) != NULL ) {
		for (i = 0; i < range_size && base_index + i < cached_count; ++i) {
			if (CACHED_UNWIND_INFO_HASH(range, i) == hash) {
				info = CACHED_UNWIND_INFO(range, range_size, i);
				if (info->len == unwind_info_len && memcmp (info->data, unwind_info, unwind_info_len) == 0) {
					unwind_unlock ();
					return base_index + i;
				}
			}
		}
		if (i < range_size) {
			/* Not found but still have room in current (last) range:
			 * will store allocated cached info within current range
			 */
			break;
		}
		base_index += range_size;
		range_size <<= 1;
		range_index ++;
	}

	if (!range) { /* need to initialize new range */
		/* ensure no overflows */
		g_assert( range_size != 0 );
		 /* +1 cuz we always need one empty range at the end */
		g_assert( range_index + 1 < sizeof(cached_info) / sizeof(cached_info[0]) );

		/* allocate and add new range */
		range = unwind_info_malloc( (sizeof(guint8 *) + sizeof(guint32)) * range_size );

		cached_info [range_index] = range;

		/* it will be very first element in new range */
		i = 0;
	}

	info = (MonoUnwindInfo *) unwind_info_malloc( sizeof (MonoUnwindInfo) + unwind_info_len );
	info->len = unwind_info_len;
	memcpy(info->data, unwind_info, unwind_info_len);

	CACHED_UNWIND_INFO_HASH(range, i) = hash;
	CACHED_UNWIND_INFO(range, range_size, i) = info;
	/* Ensure things are in memory before they're accounted,
	 * its needed cuz mono_get_cached_unwind_info doesnt use
	 * unwind_(un)lock */
	mono_memory_barrier ();
//	g_assert( base_index + i == cached_count );
	g_assert(cached_count != (guint32)-1);

	cached_count ++;

	unwind_unlock ();
	return base_index + i;
}

/*
 * This function is signal safe.
 */
guint8*
mono_get_cached_unwind_info (guint32 index, guint32 *unwind_info_len)
{
	CachedUnwindInfoRangePtr range;
	MonoUnwindInfo *info;
	guint32 range_index;
	guint32 range_size;

	/* need to deduce range index and index within range from given 'global' index */
	range_index = (sizeof(unsigned int) * 8 -
		leading_zero_bit_count_32 ( (index >> CACHED_UNWIND_INFO_RANGE0_POF2) + 1)) - 1;

	range_size = ( CACHED_UNWIND_INFO_RANGE0_SIZE << range_index );

	index -= ((1 << range_index) - 1) << CACHED_UNWIND_INFO_RANGE0_POF2;

	/* Don't need to do any synchronization cuz cached info data
	 * is never free'd as well as never removed once added
	 */
	range = cached_info [range_index];
	info = CACHED_UNWIND_INFO(range, range_size, index);

	*unwind_info_len = info->len;
	return info->data;
}


#else /******************************************************************************/

typedef struct _GSList GSList;
struct _GSList {
	gpointer data;
	GSList *next;
};


GSList*
g_slist_last (GSList *list)
{
	if (!list)
		return NULL;

	while (list->next)
		list = list->next;

	return list;
}

GSList*
g_slist_concat (GSList *list1, GSList *list2)
{
	if (!list1)
		return list2;

	g_slist_last (list1)->next = list2;
	return list1;
}


GSList*
g_slist_alloc (void)
{
	return g_new0 (GSList, 1);
}

void
g_slist_free_1 (GSList *list)
{
	g_free (list);
}

void
g_slist_free (GSList *list)
{
	while (list) {
		GSList *next = list->next;
		g_slist_free_1 (list);
		list = next;
	}
}


/* This is also a list node constructor. */
GSList*
g_slist_prepend (GSList *list, gpointer data)
{
	GSList *head = g_slist_alloc ();
	head->data = data;
	head->next = list;

	return head;
}

GSList*
g_slist_append (GSList *list, gpointer data)
{
	return g_slist_concat (list, g_slist_prepend (NULL, data));
}

/*
 * Insert the given data in a new node after the current node. 
 * Return new node.
 */
static GSList *
insert_after (GSList *list, gpointer data)
{
	list->next = g_slist_prepend (list->next, data);
	return list->next;
}



typedef struct {
	guint32 len;
	guint8 info [MONO_ZERO_LEN_ARRAY];
} MonoUnwindInfo;

static MonoUnwindInfo **cached_info;
static int cached_info_next, cached_info_size;
static GSList *cached_info_list;
/* Statistics */
static int unwind_info_size;

void
mono_unwind_cleanup (void)
{
	mono_os_mutex_destroy (&unwind_mutex);

	if (!cached_info)
		return;

	for (int i = 0; i < cached_info_next; ++i) {
		MonoUnwindInfo *cached = cached_info [i];

		g_free (cached);
	}
	g_free (cached_info);

	for (GSList *cursor = cached_info_list; cursor != NULL; cursor = cursor->next)
		g_free (cursor->data);

	g_slist_free (cached_info_list);

	cached_info = NULL;
	cached_info_next = cached_info_size = 0;
	cached_info_list = NULL;
}

/*
 * mono_cache_unwind_info
 *
 *   Save UNWIND_INFO in the unwind info cache and return an id which can be passed
 * to mono_get_cached_unwind_info to get a cached copy of the info.
 * A copy is made of the unwind info.
 * This function is useful for two reasons:
 * - many methods have the same unwind info
 * - MonoJitInfo->unwind_info is an int so it can't store the pointer to the unwind info
 */
guint32
mono_cache_unwind_info (guint8 *unwind_info, guint32 unwind_info_len)
{
	int i;
	MonoUnwindInfo *info;

	unwind_lock ();

	if (cached_info == NULL) {
		cached_info_size = 16;
		cached_info = g_new0 (MonoUnwindInfo*, cached_info_size);
	}

	for (i = 0; i < cached_info_next; ++i) {
		MonoUnwindInfo *cached = cached_info [i];

		if (cached->len == unwind_info_len && memcmp (cached->info, unwind_info, unwind_info_len) == 0) {
			unwind_unlock ();
			return i;
		}
	}

	info = (MonoUnwindInfo *)g_malloc (sizeof (MonoUnwindInfo) + unwind_info_len);
	info->len = unwind_info_len;
	memcpy (&info->info, unwind_info, unwind_info_len);

	i = cached_info_next;
	
	if (cached_info_next >= cached_info_size) {
		MonoUnwindInfo **new_table;

		/*
		 * Avoid freeing the old table so mono_get_cached_unwind_info ()
		 * doesn't need locks/hazard pointers.
		 */

		new_table = g_new0 (MonoUnwindInfo*, cached_info_size * 2);

		memcpy (new_table, cached_info, cached_info_size * sizeof (MonoUnwindInfo*));

		mono_memory_barrier ();

		cached_info_list = g_slist_prepend (cached_info_list, cached_info);

		cached_info = new_table;

		cached_info_size *= 2;
	}

	cached_info [cached_info_next ++] = info;

	unwind_info_size += sizeof (MonoUnwindInfo) + unwind_info_len;

	unwind_unlock ();
	return i;
}

/*
 * This function is signal safe.
 */
guint8*
mono_get_cached_unwind_info (guint32 index, guint32 *unwind_info_len)
{
	MonoUnwindInfo **table;
	MonoUnwindInfo *info;
	guint8 *data;

	/*
	 * This doesn't need any locks/hazard pointers,
	 * since new tables are copies of the old ones.
	 */
	table = cached_info;

	info = table [index];

	*unwind_info_len = info->len;
	data = info->info;

	return data;
}


#endif


int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	long long iterations = -1;
	if (argc > 1) {
		iterations = atol(argv[1]);
	}
	std::map<std::vector<guint8>, guint32> known;
	srand(123); /* random combinations should reproduce each test run */
	std::vector<guint8> tmp;
	clock_t cl_very_start = clock();
	clock_t cl_start = cl_very_start;
	for (size_t i = 0; !got_signal;) {
		if (iterations == 0) {
			printf("\nTest complete in %llu microseconds\n", (unsigned long long)(clock() - cl_very_start));
			break;
		}else if (iterations > 0) {
			--iterations;
		}
		tmp.resize(1 + rand() % 64);
		for (auto &v : tmp) {
			v = rand() % 0x100;
		}
		guint32 index = mono_cache_unwind_info (&tmp[0], tmp.size());
		auto ir = known.emplace(tmp, index);
		if (!ir.second) {
			assert(ir.first->second == index);
		}
		if ((rand() & 0xffff) == 123) {
			clock_t cl_end = clock();
			unsigned long msec = (cl_end - cl_start) / 1000;
			printf("{%ld@%ld}", known.size(), (msec ? (1000 * known.size()) / msec : -1));
			fflush(stdout);
			for (const auto &k : known) {
				index = mono_cache_unwind_info ((guint8 *)&k.first[0], k.first.size());
				assert(index == k.second);
				guint32 len = 0;
				guint8 *data = mono_get_cached_unwind_info (k.second, &len);
				assert(len == k.first.size());
				assert(memcmp(data, &k.first[0], len) == 0);
			}

			known.clear();
			mono_unwind_cleanup();
			cl_start = clock();

		} else if (i == 1000) {
			printf(".");  fflush(stdout);
			i = 0;
		} else {
			++i;
		}
	}
	printf("\nInvoking mono_unwind_cleanup()...\n");
	cl_start = clock();
	mono_unwind_cleanup();
	printf("mono_unwind_cleanup() complete in %lu microseconds of CPU time, exiting...\n", (unsigned long)(clock() - cl_start));
	printf("Exiting");
	return 0;
}
