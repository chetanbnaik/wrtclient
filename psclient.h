#ifndef __PS_CLIENT_H__
#define __PS_CLIENT_H__

#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <jansson.h>

#include "mutex.h"
#include "dtls.h"
#include "ice.h"
#include "rtcp.h"
#include "sctp.h"
#include "plugins/ps_gstreamer.h"
#include "transport/ps_websockets.h"

#define JANUS_BUFSIZE   8192

typedef struct ps_request ps_request;
typedef struct ps_session {
	guint64 session_id;
	GHashTable * ice_handles;
	gint64 last_activity;
	ps_request * source;
	gint destroy:1;
	gint timeout:1;
	ps_mutex mutex;
} ps_session;

ps_session * ps_session_create (guint64 session_id);
ps_session * ps_session_find (guint64 session_id);
void ps_session_notify_event (guint64 session_id, json_t * event);
ps_session * ps_session_find_destroyed (guint64 session_id);
gint ps_session_destroy (guint64 session_id);
void ps_session_free (ps_session * session);

struct ps_request {
	ps_transport * transport;
	void * instance;
	json_t * message;
};

ps_request * ps_request_new (ps_transport * transport, void * instance, json_t * message);
void ps_request_destroy (ps_request * request);
int ps_process_incoming_request (ps_request * request);
int ps_process_success (ps_request * request, json_t * payload);
int ps_process_error (ps_request * source, uint64_t session_id, const char * transaction, gint error, const char * format, ...) G_GNUC_PRINTF(5,6);

ps_plugin * ps_plugin_find (const gchar * package);

gchar * ps_get_server_pem (void);
gchar * ps_get_server_key (void);
gchar * ps_get_local_ip (void);
gchar * ps_get_public_ip (void);
void ps_set_public_ip (const char * ip);

gint ps_is_stopping(void);
#endif // __PS_CLIENT_H__
