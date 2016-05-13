#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

#define THREAD_NAME "log"

typedef struct ps_log_buffer ps_log_buffer;
struct ps_log_buffer {
	size_t allocated;
	ps_log_buffer *next;
	/* str is grown by allocating beyond the struct */
	char str[1];
};

#define INITIAL_BUFSZ		2000

static gboolean ps_log_console = TRUE;
static char *ps_log_filepath = NULL;
static FILE *ps_log_file = NULL;

static gint initialized = 0;
static gint stopping = 0;
static gint poolsz = 0;
static gint maxpoolsz = 32;
/* Buffers over this size will be freed */
static size_t maxbuffersz = 8000;
static GMutex lock;
static GCond cond;
static GThread *printthread = NULL;
static ps_log_buffer *printhead = NULL;
static ps_log_buffer *printtail = NULL;
static ps_log_buffer *bufferpool = NULL;


gboolean ps_log_is_stdout_enabled(void) {
	return ps_log_console;
}

gboolean ps_log_is_logfile_enabled(void) {
	return ps_log_file != NULL;
}

char *ps_log_get_logfile_path(void) {
	return ps_log_filepath;
}


static void ps_log_freebuffers(ps_log_buffer **list) {
	ps_log_buffer *b, *head = *list;

	while (head) {
		b = head;
		head = b->next;
		g_free(b);
	}
	*list = NULL;
}

static ps_log_buffer *ps_log_getbuf(void) {
	ps_log_buffer *b;

	g_mutex_lock(&lock);
	b = bufferpool;
	if (b) {
		bufferpool = b->next;
		b->next = NULL;
	} else {
		poolsz++;
	}
	g_mutex_unlock(&lock);
	if (b == NULL) {
		b = g_malloc(INITIAL_BUFSZ + sizeof(*b));
		b->allocated = INITIAL_BUFSZ;
		b->next = NULL;
	}
	return b;
}

static void *ps_log_thread(void *ctx) {
	ps_log_buffer *head, *b, *tofree = NULL;

	while (!g_atomic_int_get(&stopping)) {
		g_mutex_lock(&lock);
		if (!printhead) {
			g_cond_wait(&cond, &lock);
		}
		head = printhead;
		printhead = printtail = NULL;
		g_mutex_unlock(&lock);

		if (head) {
			for (b = head; b; b = b->next) {
				if(ps_log_console)
					fputs(b->str, stdout);
				if(ps_log_file)
					fputs(b->str, ps_log_file);
			}
			g_mutex_lock(&lock);
			while (head) {
				b = head;
				head = b->next;
				if (poolsz >= maxpoolsz || b->allocated > maxbuffersz) {
					b->next = tofree;
					tofree = b;
					poolsz--;
				} else {
					b->next = bufferpool;
					bufferpool = b;
				}
			}
			g_mutex_unlock(&lock);
			if(ps_log_console)
				fflush(stdout);
			if(ps_log_file)
				fflush(ps_log_file);
			ps_log_freebuffers(&tofree);
		}
	}
	/* print any remaining messages, stdout flushed on exit */
	for (b = printhead; b; b = b->next) {
		if(ps_log_console)
			fputs(b->str, stdout);
		if(ps_log_file)
			fputs(b->str, ps_log_file);
	}
	if(ps_log_console)
		fflush(stdout);
	if(ps_log_file)
		fflush(ps_log_file);
	ps_log_freebuffers(&printhead);
	ps_log_freebuffers(&bufferpool);
	g_mutex_clear(&lock);
	g_cond_clear(&cond);

	if(ps_log_file)
		fclose(ps_log_file);
	ps_log_file = NULL;
	g_free(ps_log_filepath);
	ps_log_filepath = NULL;

	return NULL;
}

void ps_vprintf(const char *format, ...) {
	int len;
	va_list ap, ap2;
	ps_log_buffer *b = ps_log_getbuf();

	va_start(ap, format);
	va_copy(ap2, ap);
	/* first try */
	len = vsnprintf(b->str, b->allocated, format, ap);
	va_end(ap);
	if (len >= (int) b->allocated) {
		/* buffer wasn't big enough */
		b = g_realloc(b, len + 1 + sizeof(*b));
		b->allocated = len + 1;
		vsnprintf(b->str, b->allocated, format, ap2);
	}
	va_end(ap2);

	g_mutex_lock(&lock);
	if (!printhead) {
		printhead = printtail = b;
	} else {
		printtail->next = b;
		printtail = b;
	}
	g_cond_signal(&cond);
	g_mutex_unlock(&lock);
}

int ps_log_init(gboolean daemon, gboolean console, const char *logfile) {
	if (g_atomic_int_get(&initialized)) {
		return 0;
	}
	g_atomic_int_set(&initialized, 1);
	g_mutex_init(&lock);
	g_cond_init(&cond);
	if(console) {
		/* Set stdout to block buffering, see BUFSIZ in stdio.h */
		setvbuf(stdout, NULL, _IOFBF, 0);
	}
	ps_log_console = console;
	if(logfile != NULL) {
		/* Open a log file for writing (and append) */
		ps_log_file = fopen(logfile, "awt");
		if(ps_log_file == NULL) {
			g_print("Error opening log file %s: %s\n", logfile, strerror(errno));
			return -1;
		}
		ps_log_filepath = g_strdup(logfile);
	}
	if(!ps_log_console && logfile == NULL) {
		g_print("WARNING: logging completely disabled!\n");
		g_print("         (no stdout and no logfile, this may not be what you want...)\n");
	}
	if(daemon) {
		/* Replace the standard file descriptors */
		if (freopen("/dev/null", "r", stdin) == NULL) {
			g_print("Error replacing stdin with /dev/null\n");
			return -1;
		}
		if (freopen("/dev/null", "w", stdout) == NULL) {
			g_print("Error replacing stdout with /dev/null\n");
			return -1;
		}
		if (freopen("/dev/null", "w", stderr) == NULL) {
			g_print("Error replacing stderr with /dev/null\n");
			return -1;
		}
	}
	printthread = g_thread_new(THREAD_NAME, &ps_log_thread, NULL);
	return 0;
}

void ps_log_destroy(void) {
	g_atomic_int_set(&stopping, 1);
	g_mutex_lock(&lock);
	/* Signal print thread to print any remaining message */
	g_cond_signal(&cond);
	g_mutex_unlock(&lock);
	g_thread_join(printthread);
}
