#ifndef __PS_LOG_H_
#define __PS_LOG_H_

#include <stdio.h>
#include <glib.h>

/*! \brief Buffered vprintf
* @param[in] format Format string as defined by glib
* @param[in] args Parameters to insert into formatted string
* \note This output is buffered and may not appear immediately on stdout. */
void ps_vprintf(const char *format, ...) G_GNUC_PRINTF(1, 2);

/*! \brief Log initialization
* \note This should be called before attempting to use the logger. A buffer
* pool and processing thread are created.
* @param daemon Whether the Janus is running as a daemon or not
* @param console Whether the output should be printed on stdout or not
* @param logfile Log file to save the output to, if any
* @returns 0 in case of success, a negative integer otherwise */
int ps_log_init(gboolean daemon, gboolean console, const char *logfile);
/*! \brief Log destruction */
void ps_log_destroy(void);

/*! \brief Method to check whether stdout logging is enabled
 * @returns TRUE if stdout logging is enabled, FALSE otherwise */
gboolean ps_log_is_stdout_enabled(void);
/*! \brief Method to check whether file-based logging is enabled
 * @returns TRUE if file-based logging is enabled, FALSE otherwise */
gboolean ps_log_is_logfile_enabled(void);
/*! \brief Method to get the path to the log file
 * @returns The full path to the log file, or NULL otherwise */
char *ps_log_get_logfile_path(void);

#endif
