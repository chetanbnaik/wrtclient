#ifndef __PS_DEBUG_H__
#define __PS_DEBUG_H__

#include <glib.h>
#include <glib/gprintf.h>
#include "log.h"

extern int ps_log_level;
extern gboolean ps_log_timestamps;
extern gboolean ps_log_colors;


#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"
/* No debugging */
#define LOG_NONE     (0)
/* Fatal error */
#define LOG_FATAL    (1)
/* non-fatal error */
#define LOG_ERR      (2)
/* warnings */
#define LOG_WARN     (3)
/* informational message */
#define LOG_INFO     (4)
/* Verbose message */
#define LOG_VERB     (5)
/* Overly verbose message */
#define LOG_HUGE     (6)
/* Debug message */
#define LOG_DBG      (7)
/* Maximum level of debugging */
#define LOG_MAX LOG_DBG

/* Coloured prefixes for errors and warnings logging. */
static const char *ps_log_prefix[] = {
/* no colors */
	"",
	"[FATAL] ",
	"[ERR] ",
	"[WARN] ",
	"",
	"",
	"",
	"",
/* with colors */
	"",
	ANSI_COLOR_MAGENTA"[FATAL]"ANSI_COLOR_RESET" ",
	ANSI_COLOR_RED"[ERR]"ANSI_COLOR_RESET" ",
	ANSI_COLOR_YELLOW"[WARN]"ANSI_COLOR_RESET" ",
	"",
	"",
	"",
	""
};

/* Simple wrapper to g_print/printf */
#define PS_PRINT ps_vprintf
/* Logger based on different levels, which can either be displayed
 * or not according to the configuration of the gateway.
 * The format must be a string literal. */
#define PS_LOG(level, format, ...) \
do { \
	if (level > LOG_NONE && level <= LOG_MAX && level <= ps_log_level) { \
		char ps_log_ts[64] = ""; \
		char ps_log_src[128] = ""; \
		if (ps_log_timestamps) { \
			struct tm pstmresult; \
			time_t psltime = time(NULL); \
			localtime_r(&psltime, &pstmresult); \
			strftime(ps_log_ts, sizeof(ps_log_ts), \
			         "[%a %b %e %T %Y] ", &pstmresult); \
		} \
		if (level == LOG_FATAL || level == LOG_ERR || level == LOG_DBG) { \
			snprintf(ps_log_src, sizeof(ps_log_src), \
			         "[%s:%s:%d] ", __FILE__, __FUNCTION__, __LINE__); \
		} \
		PS_PRINT("%s%s%s" format, \
			ps_log_ts, \
			ps_log_prefix[level | ((int)ps_log_colors << 3)], \
			ps_log_src, \
			##__VA_ARGS__); \
	} \
} while (0)


#endif
