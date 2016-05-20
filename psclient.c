#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <dirent.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/resource.h>

#include "psclient.h"
#include "apierror.h"
#include "rtcp.h"
#include "sdp.h"
#include "debug.h"
#include "log.h"
#include "config.h"
#include "utils.h"

int lock_debug = 0;
int ps_log_level = LOG_DBG;
gboolean ps_log_timestamps = TRUE;
gboolean ps_log_colors = TRUE;

static ps_config * config = NULL;
static char * config_file = NULL;
static char * configs_folder = NULL;
static GHashTable * plugins = NULL;

static char * server_key = NULL;
gchar * ps_get_server_key (void) {
	return server_key;
}

static char * server_pem = NULL;
gchar * ps_get_server_pem (void) {
	return server_pem;
}

static gchar local_ip[INET6_ADDRSTRLEN];
gchar * ps_get_local_ip (void) {
	return local_ip;
}
static gchar * public_ip = NULL;
gchar * ps_get_public_ip (void) {
	return public_ip ? public_ip : local_ip;
}
void ps_set_public_ip (const char * ip) {
	if (ip == NULL || public_ip != NULL) return;
	public_ip = g_strdup(ip);
}

static volatile gint stop = 0;
gint ps_is_stopping(void) {
	return g_atomic_int_get(&stop);
}

/* Transport plugin callback interface */
void ps_transport_incoming_request (ps_transport * plugin, void * transport, json_t * message, json_error_t * error);
void ps_transport_gone (ps_transport * plugin, void * transport);

static ps_transport_callbacks ps_handler = {
	.incoming_request = ps_transport_incoming_request,
	.transport_gone = ps_transport_gone,
};
GThreadPool * tasks = NULL;
void ps_transport_task (gpointer data, gpointer user_data);

/* plugin callback interface */
int ps_plugin_push_event (ps_plugin_session * plugin_session, ps_plugin * plugin, const char * transaction, const char * message, const char * sdp_type, const char * sdp);
json_t * ps_plugin_handle_sdp (ps_plugin_session * plugin_session, ps_plugin * plugin, const char * sdp_type, const char * sdp);
void ps_plugin_relay_rtp (ps_plugin_session * plugin_session, int video, char * buf, int len);
void ps_plugin_relay_rtcp (ps_plugin_session * plugin_session, int video, char * buf, int len);
void ps_plugin_relay_data (ps_plugin_session * plugin_session, char * buf, int len);
void ps_plugin_close_pc (ps_plugin_session * plugin_session);
void ps_plugin_end_session (ps_plugin_session * plugin_session);
static ps_callbacks ps_handler_plugin = {
	.push_event = ps_plugin_push_event,
	.relay_rtp = ps_plugin_relay_rtp,
	.relay_rtcp = ps_plugin_relay_rtcp,
	.relay_data = ps_plugin_relay_data,
	.close_pc = ps_plugin_close_pc,
	.end_session = ps_plugin_end_session,
}; 

/* Gateway sessions */
static GMainContext * sessions_watchdog_context;
static ps_mutex sessions_mutex;
static GHashTable * sessions = NULL, * old_sessions = NULL;

#define SESSION_TIMEOUT  300

static gboolean ps_cleanup_session (gpointer user_data) {
	ps_session * session = (ps_session *) user_data;
	PS_LOG (LOG_DBG, "Cleaning up session %"SCNu64"...\n", session->session_id);
	ps_session_destroy (session->session_id);
	return G_SOURCE_REMOVE;
}

static gboolean ps_check_sessions (gpointer user_data) {
	GMainContext * watchdog_context = (GMainContext *) user_data;
	ps_mutex_lock (&sessions_mutex);
	if (sessions && g_hash_table_size (sessions) > 0) {
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init (&iter, sessions);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			ps_session * session = (ps_session *) value;
			if (!session || session->destroy) {
				continue;
			}
			gint64 now = ps_get_monotonic_time ();
			if ((now - session->last_activity) >= SESSION_TIMEOUT * G_USEC_PER_SEC && !session->timeout) {
				PS_LOG (LOG_INFO, "Timeout expired for session %"SCNu64"...\n", session->session_id);
				if (session->source) {
					json_t * event = json_object ();
					json_object_set_new (event, "janus", json_string("timeout"));
					json_object_set_new (event, "session_id", json_integer(session->session_id));
					session->source->transport->send_message (session->source->instance, event);
					session->source->transport->session_over (session->source->instance, session->session_id, TRUE);
				}
				session->timeout = 1;
				g_hash_table_iter_remove (&iter);
				g_hash_table_insert (old_sessions, GUINT_TO_POINTER (session->session_id), session);
				
				/* Schedule session for deletion */
				GSource * timeout_source = g_timeout_source_new_seconds (3);
				g_source_set_callback (timeout_source, ps_cleanup_session, session, NULL);
				g_source_attach (timeout_source, watchdog_context);
				g_source_unref (timeout_source);
			}
		}
	}
	ps_mutex_unlock (&sessions_mutex);
	
	return G_SOURCE_CONTINUE;
}

static gpointer ps_sessions_watchdog (gpointer user_data) {
	GMainLoop * loop = (GMainLoop *) user_data;
	GMainContext * watchdog_context = g_main_loop_get_context (loop);
	
	GSource * timeout_source;
	timeout_source = g_timeout_source_new_seconds (2);
	
	g_source_set_callback (timeout_source, ps_check_sessions, watchdog_context, NULL);
	g_source_attach (timeout_source, watchdog_context);
	g_source_unref (timeout_source);
	
	PS_LOG (LOG_INFO, "Session watchdog started...\n");
	
	g_main_loop_run (loop);
	return NULL;
}

ps_session * ps_session_create (guint64 session_id) {
	if (session_id == 0) {
		while (session_id == 0) {
			session_id = g_random_int ();
			if (ps_session_find (session_id) != NULL) {
				session_id = 0;
			}
		}
	}
	
	PS_LOG (LOG_INFO, "Creating new session: %"SCNu64"\n",session_id);
	ps_session * session = (ps_session *)g_malloc0(sizeof(ps_session));
	if (session == NULL) {
		PS_LOG (LOG_FATAL, "Memory error!\n");
		return NULL;
	}
	session->session_id = session_id;
	session->source = NULL;
	session->destroy = 0;
	session->timeout = 0;
	session->last_activity = ps_get_monotonic_time();
	ps_mutex_init (&session->mutex);
	ps_mutex_lock (&session->mutex);
	g_hash_table_insert (sessions, GUINT_TO_POINTER (session_id), session);
	ps_mutex_unlock (&session->mutex);
	return session;
}

ps_session * ps_session_find (guint64 session_id) {
	ps_mutex_lock (&sessions_mutex);
	ps_session * session = g_hash_table_lookup (sessions, GUINT_TO_POINTER(session_id));
	ps_mutex_unlock (&sessions_mutex);
	return session;
}

ps_session * ps_session_find_destroyed (guint64 session_id) {
	ps_mutex_lock (&sessions_mutex);
	ps_session * session = g_hash_table_lookup (old_sessions, GUINT_TO_POINTER(session_id));
	ps_mutex_unlock (&sessions_mutex);
	return session;
}

void ps_session_notify_event (guint64 session_id, json_t * event) {
	ps_mutex_lock (&sessions_mutex);
	ps_session * session = sessions ? g_hash_table_lookup (sessions, GUINT_TO_POINTER(session_id)) : NULL;
	if (session != NULL && !session->destroy && session->source != NULL && session->source->transport != NULL) {
		ps_mutex_unlock (&sessions_mutex);
		PS_LOG (LOG_HUGE, "Sending event to %p\n", session->source->instance);
		session->source->transport->send_message(session->source->instance, event);
	} else {
		ps_mutex_unlock (&sessions_mutex);
		json_decref (event);
	}
}

gint ps_session_destroy (guint64 session_id) {
	ps_session * session = ps_session_find_destroyed(session_id);
	if (session == NULL) {
		PS_LOG (LOG_ERR, "Couldn't find session to destroy: %"SCNu64"\n", session_id);
		return -1;
	}
	PS_LOG (LOG_VERB, "Destroying session: %"SCNu64"\n", session_id);
	session->destroy = 1;
	if (session->ice_handles != NULL && g_hash_table_size(session->ice_handles) > 0) {
		GHashTableIter iter;
		gpointer value;
		/* Remove all handles */
		g_hash_table_iter_init(&iter, session->ice_handles);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_ice_handle *handle = value;
			if(!handle || g_atomic_int_get(&stop)) {
				continue;
			}
			janus_ice_handle_destroy(session, handle->handle_id);
			g_hash_table_iter_remove(&iter);
		}
	}
		
	ps_session_free (session);
	return 0;
}

void ps_session_free (ps_session * session) {
	if (session == NULL) return;
	ps_mutex_lock (&session->mutex);
	if (session->ice_handles != NULL) {
		g_hash_table_destroy (session->ice_handles);
		session->ice_handles = NULL;
	}
	if (session->source != NULL) {
		ps_request_destroy (session->source);
		session->source = NULL;
	}
	ps_mutex_unlock (&session->mutex);
	g_free (session);
	session = NULL;
}

/* Requests management */
ps_request * ps_request_new (ps_transport * transport, void * instance, json_t * message) {
	ps_request * request = (ps_request *)g_malloc0(sizeof(ps_request));
	request->transport = transport;
	request->instance = instance;
	request->message = message;
	return request;
}

void ps_request_destroy (ps_request * request) {
	if (request == NULL) return;
	request->transport = NULL;
	request->instance = NULL;
	if (request->message) json_decref (request->message);
	request->message = NULL;
	g_free (request);
}

int ps_process_incoming_request (ps_request * request) {
	int ret = -1;
	if (request == NULL) {
		PS_LOG (LOG_ERR, "missing request or payload to process, giving up...\n");
		return ret;
	}
	json_t * root = request->message;
	guint64 session_id = 0, handle_id = 0;
	json_t * s = json_object_get(root, "session_id");
	if (s && json_is_integer(s)) session_id = json_integer_value(s);
	json_t * h = json_object_get(root, "handle_id");
	if (h && json_is_integer(h)) handle_id = json_integer_value(h);
	
	json_t * transaction = json_object_get (root, "transaction");
	if (!transaction) {
		ret = ps_process_error (request, session_id, NULL, JANUS_ERROR_MISSING_MANDATORY_ELEMENT, "Missing mandatory element (transaction)");
		goto jsondone;
	}
	if (!json_is_string(transaction)) {
		ret = ps_process_error (request, session_id, NULL, JANUS_ERROR_INVALID_ELEMENT_TYPE, "Invalid element type (transaction should be a string)");
		goto jsondone;
	}
	const gchar * transaction_text = json_string_value(transaction);
	
	json_t * message = json_object_get (root, "janus");
	if (!message) {
		ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_MISSING_MANDATORY_ELEMENT, "Missing mandatory element (janus)");
		goto jsondone;
	}
	if (!json_is_string(message)) {
		ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_INVALID_ELEMENT_TYPE, "Invalid element type (janus should be a string)");
		goto jsondone;
	}
	const gchar * message_text = json_string_value(message);
	
	if (session_id == 0 && handle_id == 0) {
		if (strcasecmp(message_text, "create")) {
			ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		session_id = 0;
		ps_session * session = ps_session_create(session_id);
		if (session == NULL) {
			ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "Memory Error");
			goto jsondone;
		}
		session_id = session->session_id;
		session->source = ps_request_new (request->transport, request->instance, NULL);
		
		json_t * reply = json_object ();
		json_object_set_new (reply, "janus", json_string("success"));
		json_object_set_new (reply, "transaction", json_string(transaction_text));
		json_t * data = json_object ();
		json_object_set_new (data, "id", json_integer (session_id));
		json_object_set_new (reply, "data", data);
		ret = ps_process_success (request, reply);
		goto jsondone;
	}
	if (session_id < 1) {
		PS_LOG (LOG_ERR, "Invalid session\n");
		ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_SESSION_NOT_FOUND, NULL);
		goto jsondone;
	}
	if (h && handle_id < 1) {
		PS_LOG (LOG_ERR, "Invalid Handle\n");
		ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_SESSION_NOT_FOUND, NULL);
		goto jsondone;
	}
	
	ps_session * session = ps_session_find (session_id);
	if (!session) {
		PS_LOG (LOG_ERR, "Couldn't find session %"SCNu64"...\n", session_id);
		ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_SESSION_NOT_FOUND, "No such session %"SCNu64"", session_id);
		goto jsondone;
	}
	session->last_activity = ps_get_monotonic_time();
	janus_ice_handle * handle = NULL;
	if (handle_id > 0) {
		handle = janus_ice_handle_find (session, handle_id);
		if (!handle) {
			PS_LOG (LOG_ERR, "Couldnt find any handle %"SCNu64" in session %"SCNu64"...\n", handle_id, session_id);
			ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_HANDLE_NOT_FOUND, "No such handle %"SCNu64" in session %"SCNu64"", handle_id, session_id);
			goto jsondone;
		}
	}
	
	if(!strcasecmp(message_text, "keepalive")) {
		/* Just a keep-alive message, reply with an ack */
		PS_LOG (LOG_VERB, "Got a keep-alive on session %"SCNu64"\n", session_id);
		json_t *reply = json_object();
		json_object_set_new (reply, "janus", json_string("ack"));
		json_object_set_new (reply, "session_id", json_integer(session_id));
		json_object_set_new (reply, "transaction", json_string(transaction_text));
		/* Send the success reply */
		ret = ps_process_success (request, reply);
	} else if (!strcasecmp(message_text, "attach")) {
		if(handle != NULL) {
			/* Attach is a session-level command */
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		json_t *plugin = json_object_get(root, "plugin");
		if(!plugin) {
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_MISSING_MANDATORY_ELEMENT, "Missing mandatory element (plugin)");
			goto jsondone;
		}
		if(!json_is_string(plugin)) {
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_ELEMENT_TYPE, "Invalid element type (plugin should be a string)");
			goto jsondone;
		}
		const gchar *plugin_text = json_string_value(plugin);
		ps_plugin *plugin_t = ps_plugin_find(plugin_text);
		if(plugin_t == NULL) {
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_NOT_FOUND, "No such plugin '%s'", plugin_text);
			goto jsondone;
		}
		/* Create handle */
		handle = janus_ice_handle_create(session);
		if(handle == NULL) {
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "Memory error");
			goto jsondone;
		}
		handle_id = handle->handle_id;
		/* Attach to the plugin */
		int error = 0;
		if((error = janus_ice_handle_attach_plugin(session, handle_id, plugin_t)) != 0) {
			/* TODO Make error struct to pass verbose information */
			janus_ice_handle_destroy(session, handle_id);
			ps_mutex_lock(&session->mutex);
			g_hash_table_remove(session->ice_handles, GUINT_TO_POINTER(handle_id));
			ps_mutex_unlock(&session->mutex);

			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_ATTACH, "Couldn't attach to plugin: error '%d'", error);
			goto jsondone;
		}
		/* Prepare JSON reply */
		json_t *reply = json_object();
		json_object_set_new(reply, "janus", json_string("success"));
		json_object_set_new(reply, "session_id", json_integer(session_id));
		json_object_set_new(reply, "transaction", json_string(transaction_text));
		json_t *data = json_object();
		json_object_set_new(data, "id", json_integer(handle_id));
		json_object_set_new(reply, "data", data);
		/* Send the success reply */
		ret = ps_process_success(request, reply);
	} else if (!strcasecmp(message_text, "destroy")) {
		if(handle != NULL) {
			/* Query is a session-level command */
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		/* Schedule the session for deletion */
		session->destroy = 1;
		ps_mutex_lock(&sessions_mutex);
		g_hash_table_remove(sessions, GUINT_TO_POINTER(session->session_id));
		g_hash_table_insert(old_sessions, GUINT_TO_POINTER(session->session_id), session);
		GSource *timeout_source = g_timeout_source_new_seconds(3);
		g_source_set_callback(timeout_source, ps_cleanup_session, session, NULL);
		g_source_attach(timeout_source, sessions_watchdog_context);
		g_source_unref(timeout_source);
		ps_mutex_unlock(&sessions_mutex);
		/* Notify the source that the session has been destroyed */
		if(session->source && session->source->transport)
			session->source->transport->session_over(session->source->instance, session->session_id, FALSE);

		/* Prepare JSON reply */
		json_t *reply = json_object();
		json_object_set_new(reply, "janus", json_string("success"));
		json_object_set_new(reply, "session_id", json_integer(session_id));
		json_object_set_new(reply, "transaction", json_string(transaction_text));
		/* Send the success reply */
		ret = ps_process_success(request, reply);
	} else if (!strcasecmp(message_text, "detach")) {
		if(handle == NULL) {
			/* Query is an handle-level command */
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		if(handle->app == NULL || handle->app_handle == NULL) {
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_DETACH, "No plugin to detach from");
			goto jsondone;
		}
		int error = janus_ice_handle_destroy(session, handle_id);
		ps_mutex_lock(&session->mutex);
		g_hash_table_remove(session->ice_handles, GUINT_TO_POINTER(handle_id));
		ps_mutex_unlock(&session->mutex);

		if(error != 0) {
			/* TODO Make error struct to pass verbose information */
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_DETACH, "Couldn't detach from plugin: error '%d'", error);
			/* TODO Delete handle instance */
			goto jsondone;
		}
		/* Prepare JSON reply */
		json_t *reply = json_object();
		json_object_set_new(reply, "janus", json_string("success"));
		json_object_set_new(reply, "session_id", json_integer(session_id));
		json_object_set_new(reply, "transaction", json_string(transaction_text));
		/* Send the success reply */
		ret = ps_process_success(request, reply);
	} else if (!strcasecmp(message_text, "hangup")) {
		if(handle == NULL) {
			/* Query is an handle-level command */
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		if(handle->app == NULL || handle->app_handle == NULL) {
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_DETACH, "No plugin attached");
			goto jsondone;
		}
		janus_ice_webrtc_hangup(handle);
		/* Prepare JSON reply */
		json_t *reply = json_object();
		json_object_set_new(reply, "janus", json_string("success"));
		json_object_set_new(reply, "session_id", json_integer(session_id));
		json_object_set_new(reply, "transaction", json_string(transaction_text));
		/* Send the success reply */
		ret = ps_process_success(request, reply);
	} else if (!strcasecmp(message_text, "message")) {
		if (handle == NULL) {
			ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		if (handle->app == NULL || handle->app_handle == NULL) {
			ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE, "No plugin to handle this message");
			goto jsondone;
		}
		ps_plugin * plugin_t = (ps_plugin *) handle->app;
		PS_LOG (LOG_VERB, "[%"SCNu64"] there is a message for %s\n", handle->handle_id, plugin_t->get_name());
		json_t * body = json_object_get (root, "body");
		if (body == NULL) {
			ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_MISSING_MANDATORY_ELEMENT, "Missing mandatory element (body)");
			goto jsondone;
		}
		if (!json_is_object(body)) {
			ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_INVALID_JSON_OBJECT, "Invalid body object");
			goto jsondone;
		}
		json_t * jsep = json_object_get (root, "jsep");
		char * jsep_type = NULL;
		char * jsep_sdp = NULL, * jsep_sdp_stripped = NULL;
		if (jsep != NULL) {
			if(!json_is_object(jsep)) {
				ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_JSON_OBJECT, "Invalid jsep object");
				goto jsondone;
			}
			json_t *type = json_object_get(jsep, "type");
			if(!type) {
				ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_MISSING_MANDATORY_ELEMENT, "JSEP error: missing mandatory element (type)");
				goto jsondone;
			}
			if(!json_is_string(type)) {
				ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_ELEMENT_TYPE, "JSEP error: invalid element type (type should be a string)");
				goto jsondone;
			}
			jsep_type = g_strdup(json_string_value(type));
			type = NULL;
			gboolean do_trickle = TRUE;
			json_t *jsep_trickle = json_object_get(jsep, "trickle");
			if(jsep_trickle && !json_is_boolean(jsep_trickle)) {
				ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_ELEMENT_TYPE, "JSEP error: invalid element type (trickle should be a boolean)");
				goto jsondone;
			}
			do_trickle = jsep_trickle ? json_is_true(jsep_trickle) : TRUE;
			
			/* Are we still cleaning up from a previous media session? */
			if(ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_CLEANING)) {
				PS_LOG(LOG_VERB, "[%"SCNu64"] Still cleaning up from a previous media session, let's wait a bit...\n", handle->handle_id);
				gint64 waited = 0;
				while(ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_CLEANING)) {
					g_usleep(100000);
					waited += 100000;
					if(waited >= 3*G_USEC_PER_SEC) {
						PS_LOG(LOG_VERB, "[%"SCNu64"]   -- Waited 3 seconds, that's enough!\n", handle->handle_id);
						break;
					}
				}
			}
			
			/* Check the JSEP type */
			ps_mutex_lock(&handle->mutex);
			int offer = 0;
			if(!strcasecmp(jsep_type, "offer")) {
				offer = 1;
				ps_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
				ps_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_OFFER);
				ps_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER);
			} else if(!strcasecmp(jsep_type, "answer")) {
				ps_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER);
				offer = 0;
			} else {
				/* TODO Handle other message types as well */
				ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_JSEP_UNKNOWN_TYPE, "JSEP error: unknown message type '%s'", jsep_type);
				g_free(jsep_type);
				ps_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
				ps_mutex_unlock(&handle->mutex);
				goto jsondone;
			}
			json_t *sdp = json_object_get(jsep, "sdp");
			if(!sdp) {
				ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_MISSING_MANDATORY_ELEMENT, "JSEP error: missing mandatory element (sdp)");
				g_free(jsep_type);
				ps_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
				ps_mutex_unlock(&handle->mutex);
				goto jsondone;
			}
			if(!json_is_string(sdp)) {
				ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_ELEMENT_TYPE, "JSEP error: invalid element type (sdp should be a string)");
				g_free(jsep_type);
				ps_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
				ps_mutex_unlock(&handle->mutex);
				goto jsondone;
			}
			jsep_sdp = (char *)json_string_value(sdp);
			PS_LOG(LOG_VERB, "[%"SCNu64"] Remote SDP:\n%s", handle->handle_id, jsep_sdp);
			/* Is this valid SDP? */
			int audio = 0, video = 0, data = 0, bundle = 0, rtcpmux = 0, trickle = 0;
			janus_sdp *parsed_sdp = janus_sdp_preparse(jsep_sdp, &audio, &video, &data, &bundle, &rtcpmux, &trickle);
			trickle = trickle && do_trickle;
			if(parsed_sdp == NULL) {
				/* Invalid SDP */
				ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_JSEP_INVALID_SDP, "JSEP error: invalid SDP");
				g_free(jsep_type);
				ps_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
				ps_mutex_unlock(&handle->mutex);
				goto jsondone;
			}
			
			/* FIXME We're only handling single audio/video lines for now... */
			PS_LOG(LOG_VERB, "[%"SCNu64"] Audio %s been negotiated, Video %s been negotiated, SCTP/DataChannels %s been negotiated\n",
			                    handle->handle_id,
			                    audio ? "has" : "has NOT",
			                    video ? "has" : "has NOT",
			                    data ? "have" : "have NOT");
			if(audio > 1) {
				PS_LOG(LOG_WARN, "[%"SCNu64"] More than one audio line? only going to negotiate one...\n", handle->handle_id);
			}
			if(video > 1) {
				PS_LOG(LOG_WARN, "[%"SCNu64"] More than one video line? only going to negotiate one...\n", handle->handle_id);
			}
			if(data > 1) {
				PS_LOG(LOG_WARN, "[%"SCNu64"] More than one data line? only going to negotiate one...\n", handle->handle_id);
			}

			PS_LOG(LOG_VERB, "[%"SCNu64"] The browser: %s BUNDLE, %s rtcp-mux, %s doing Trickle ICE\n", handle->handle_id,
			                    bundle  ? "supports" : "does NOT support",
			                    rtcpmux ? "supports" : "does NOT support",
			                    trickle ? "is"       : "is NOT");
			
			if(!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_READY)
					|| ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT)) {
				/* New session */
				if(offer) {
					/* Setup ICE locally (we received an offer) */
					if(janus_ice_setup_local(handle, offer, audio, video, data, bundle, rtcpmux, trickle) < 0) {
						PS_LOG(LOG_ERR, "Error setting ICE locally\n");
						g_free(jsep_type);
						ps_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
						ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "Error setting ICE locally");
						ps_mutex_unlock(&handle->mutex);
						goto jsondone;
					}
				} else {
					/* Make sure we're waiting for an ANSWER in the first place */
					if(!handle->agent) {
						PS_LOG(LOG_ERR, "Unexpected ANSWER (did we offer?)\n");
						g_free(jsep_type);
						ps_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
						ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_UNEXPECTED_ANSWER, "Unexpected ANSWER (did we offer?)");
						ps_mutex_unlock(&handle->mutex);
						goto jsondone;
					}
				}
				janus_sdp_parse(handle, parsed_sdp);
				janus_sdp_free(parsed_sdp);
				if(!offer) {
					/* Set remote candidates now (we received an answer) */
					if(bundle) {
						ps_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE);
					} else {
						ps_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE);
					}
					if(rtcpmux) {
						ps_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RTCPMUX);
					} else {
						ps_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RTCPMUX);
					}
					if(trickle) {
						ps_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_TRICKLE);
					} else {
						ps_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_TRICKLE);
					}
					if(ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE)) {
						PS_LOG(LOG_HUGE, "[%"SCNu64"]   -- bundle is supported by the browser, getting rid of one of the RTP/RTCP components, if any...\n", handle->handle_id);
						if(audio) {
							/* Get rid of video and data, if present */
							if(handle->streams && handle->video_stream) {
								handle->audio_stream->video_ssrc = handle->video_stream->video_ssrc;
								handle->audio_stream->video_ssrc_peer = handle->video_stream->video_ssrc_peer;
								handle->audio_stream->video_ssrc_peer_rtx = handle->video_stream->video_ssrc_peer_rtx;
								nice_agent_attach_recv(handle->agent, handle->video_stream->stream_id, 1, g_main_loop_get_context (handle->iceloop), NULL, NULL);
								if(!janus_ice_is_rtcpmux_forced())
									nice_agent_attach_recv(handle->agent, handle->video_stream->stream_id, 2, g_main_loop_get_context (handle->iceloop), NULL, NULL);
								nice_agent_remove_stream(handle->agent, handle->video_stream->stream_id);
								janus_ice_stream_free(handle->streams, handle->video_stream);
							}
							handle->video_stream = NULL;
							handle->video_id = 0;
							if(handle->streams && handle->data_stream) {
								nice_agent_attach_recv(handle->agent, handle->data_stream->stream_id, 1, g_main_loop_get_context (handle->iceloop), NULL, NULL);
								nice_agent_remove_stream(handle->agent, handle->data_stream->stream_id);
								janus_ice_stream_free(handle->streams, handle->data_stream);
							}
							handle->data_stream = NULL;
							handle->data_id = 0;
						} else if(video) {
							/* Get rid of data, if present */
							if(handle->streams && handle->data_stream) {
								nice_agent_attach_recv(handle->agent, handle->data_stream->stream_id, 1, g_main_loop_get_context (handle->iceloop), NULL, NULL);
								nice_agent_remove_stream(handle->agent, handle->data_stream->stream_id);
								janus_ice_stream_free(handle->streams, handle->data_stream);
							}
							handle->data_stream = NULL;
							handle->data_id = 0;
						}
					}
					if(ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RTCPMUX) && !janus_ice_is_rtcpmux_forced()) {
						PS_LOG(LOG_HUGE, "[%"SCNu64"]   -- rtcp-mux is supported by the browser, getting rid of RTCP components, if any...\n", handle->handle_id);
						if(handle->audio_stream && handle->audio_stream->components != NULL) {
							nice_agent_attach_recv(handle->agent, handle->audio_id, 2, g_main_loop_get_context (handle->iceloop), NULL, NULL);
							/* Free the component */
							janus_ice_component_free(handle->audio_stream->components, handle->audio_stream->rtcp_component);
							handle->audio_stream->rtcp_component = NULL;
							/* Create a dummy candidate and enforce it as the one to use for this now unneeded component */
							NiceCandidate *c = nice_candidate_new(NICE_CANDIDATE_TYPE_HOST);
							c->component_id = 2;
							c->stream_id = handle->audio_stream->stream_id;
#ifndef HAVE_LIBNICE_TCP
							c->transport = NICE_CANDIDATE_TRANSPORT_UDP;
#endif
							strncpy(c->foundation, "1", NICE_CANDIDATE_MAX_FOUNDATION);
							c->priority = 1;
							nice_address_set_from_string(&c->addr, "127.0.0.1");
							nice_address_set_port(&c->addr, janus_ice_get_rtcpmux_blackhole_port());
							c->username = g_strdup(handle->audio_stream->ruser);
							c->password = g_strdup(handle->audio_stream->rpass);
							if(!nice_agent_set_selected_remote_candidate(handle->agent, handle->audio_stream->stream_id, 2, c)) {
								PS_LOG(LOG_ERR, "[%"SCNu64"] Error forcing dummy candidate on RTCP component of stream %d\n", handle->handle_id, handle->audio_stream->stream_id);
								nice_candidate_free(c);
							}
						}
						if(handle->video_stream && handle->video_stream->components != NULL) {
							nice_agent_attach_recv(handle->agent, handle->video_id, 2, g_main_loop_get_context (handle->iceloop), NULL, NULL);
							/* Free the component */
							janus_ice_component_free(handle->video_stream->components, handle->video_stream->rtcp_component);
							handle->video_stream->rtcp_component = NULL;
							/* Create a dummy candidate and enforce it as the one to use for this now unneeded component */
							NiceCandidate *c = nice_candidate_new(NICE_CANDIDATE_TYPE_HOST);
							c->component_id = 2;
							c->stream_id = handle->video_stream->stream_id;
#ifndef HAVE_LIBNICE_TCP
							c->transport = NICE_CANDIDATE_TRANSPORT_UDP;
#endif
							strncpy(c->foundation, "1", NICE_CANDIDATE_MAX_FOUNDATION);
							c->priority = 1;
							nice_address_set_from_string(&c->addr, "127.0.0.1");
							nice_address_set_port(&c->addr, janus_ice_get_rtcpmux_blackhole_port());
							c->username = g_strdup(handle->video_stream->ruser);
							c->password = g_strdup(handle->video_stream->rpass);
							if(!nice_agent_set_selected_remote_candidate(handle->agent, handle->video_stream->stream_id, 2, c)) {
								PS_LOG(LOG_ERR, "[%"SCNu64"] Error forcing dummy candidate on RTCP component of stream %d\n", handle->handle_id, handle->video_stream->stream_id);
								nice_candidate_free(c);
							}
						}
					}
					/* FIXME Any disabled m-line? */
					if(strstr(jsep_sdp, "m=audio 0")) {
						PS_LOG(LOG_VERB, "[%"SCNu64"] Audio disabled via SDP\n", handle->handle_id);
						if(!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE)
								|| (!video && !data)) {
							PS_LOG(LOG_HUGE, "  -- Marking audio stream as disabled\n");
							janus_ice_stream *stream = g_hash_table_lookup(handle->streams, GUINT_TO_POINTER(handle->audio_id));
							if(stream)
								stream->disabled = TRUE;
						}
					}
					if(strstr(jsep_sdp, "m=video 0")) {
						PS_LOG(LOG_VERB, "[%"SCNu64"] Video disabled via SDP\n", handle->handle_id);
						if(!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE)
								|| (!audio && !data)) {
							PS_LOG(LOG_HUGE, "  -- Marking video stream as disabled\n");
							janus_ice_stream *stream = NULL;
							if(!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE)) {
								stream = g_hash_table_lookup(handle->streams, GUINT_TO_POINTER(handle->video_id));
							} else {
								gint id = handle->audio_id > 0 ? handle->audio_id : handle->video_id;
								stream = g_hash_table_lookup(handle->streams, GUINT_TO_POINTER(id));
							}
							if(stream)
								stream->disabled = TRUE;
						}
					}
					if(strstr(jsep_sdp, "m=application 0 DTLS/SCTP")) {
						PS_LOG(LOG_VERB, "[%"SCNu64"] Data Channel disabled via SDP\n", handle->handle_id);
						if(!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE)
								|| (!audio && !video)) {
							PS_LOG(LOG_HUGE, "  -- Marking data channel stream as disabled\n");
							janus_ice_stream *stream = NULL;
							if(!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE)) {
								stream = g_hash_table_lookup(handle->streams, GUINT_TO_POINTER(handle->data_id));
							} else {
								gint id = handle->audio_id > 0 ? handle->audio_id : (handle->video_id > 0 ? handle->video_id : handle->data_id);
								stream = g_hash_table_lookup(handle->streams, GUINT_TO_POINTER(id));
							}
							if(stream)
								stream->disabled = TRUE;
						}
					}
					
					/* We got our answer */
					ps_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
					/* Any pending trickles? */
					if(handle->pending_trickles) {
						PS_LOG(LOG_VERB, "[%"SCNu64"]   -- Processing %d pending trickle candidates\n", handle->handle_id, g_list_length(handle->pending_trickles));
						GList *temp = NULL;
						while(handle->pending_trickles) {
							temp = g_list_first(handle->pending_trickles);
							handle->pending_trickles = g_list_remove_link(handle->pending_trickles, temp);
							janus_ice_trickle *trickle = (janus_ice_trickle *)temp->data;
							g_list_free(temp);
							if(trickle == NULL)
								continue;
							if((ps_get_monotonic_time() - trickle->received) > 15*G_USEC_PER_SEC) {
								/* FIXME Candidate is too old, discard it */
								janus_ice_trickle_destroy(trickle);
								/* FIXME We should report that */
								continue;
							}
							json_t *candidate = trickle->candidate;
							if(candidate == NULL) {
								janus_ice_trickle_destroy(trickle);
								continue;
							}
							if(json_is_object(candidate)) {
								/* We got a single candidate */
								int error = 0;
								const char *error_string = NULL;
								if((error = janus_ice_trickle_parse(handle, candidate, &error_string)) != 0) {
									/* FIXME We should report the error parsing the trickle candidate */
								}
							} else if(json_is_array(candidate)) {
								/* We got multiple candidates in an array */
								PS_LOG(LOG_VERB, "Got multiple candidates (%zu)\n", json_array_size(candidate));
								if(json_array_size(candidate) > 0) {
									/* Handle remote candidates */
									size_t i = 0;
									for(i=0; i<json_array_size(candidate); i++) {
										json_t *c = json_array_get(candidate, i);
										/* FIXME We don't care if any trickle fails to parse */
										janus_ice_trickle_parse(handle, c, NULL);
									}
								}
							}
							/* Done, free candidate */
							janus_ice_trickle_destroy(trickle);
						}
					}
					/* This was an answer, check if it's time to start ICE */
					if(ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_TRICKLE) &&
							!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALL_TRICKLES)) {
						PS_LOG(LOG_VERB, "[%"SCNu64"]   -- ICE Trickling is supported by the browser, waiting for remote candidates...\n", handle->handle_id);
						ps_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_START);
					} else {
						PS_LOG(LOG_VERB, "[%"SCNu64"] Done! Sending connectivity checks...\n", handle->handle_id);
						if(handle->audio_id > 0) {
							janus_ice_setup_remote_candidates(handle, handle->audio_id, 1);
							if(!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RTCPMUX))	/* http://tools.ietf.org/html/rfc5761#section-5.1.3 */
								janus_ice_setup_remote_candidates(handle, handle->audio_id, 2);
						}
						if(handle->video_id > 0) {
							janus_ice_setup_remote_candidates(handle, handle->video_id, 1);
							if(!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RTCPMUX))	/* http://tools.ietf.org/html/rfc5761#section-5.1.3 */
								janus_ice_setup_remote_candidates(handle, handle->video_id, 2);
						}
						if(handle->data_id > 0) {
							janus_ice_setup_remote_candidates(handle, handle->data_id, 1);
						}
					}
				}
			} else {
				/* TODO Actually handle session updates: for now we ignore them, and just relay them to plugins */
				PS_LOG(LOG_WARN, "[%"SCNu64"] Ignoring negotiation update, we don't support them yet...\n", handle->handle_id);
			}
			handle->remote_sdp = g_strdup (jsep_sdp);
			ps_mutex_unlock (&handle->mutex);
			/* Anonymize SDP */
			jsep_sdp_stripped = janus_sdp_anonymize (jsep_sdp);
			if (jsep_sdp_stripped == NULL) {
				ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_JSEP_INVALID_SDP, "JSEP Error: invalid SDP");
				g_free (jsep_type);
				ps_flags_clear (&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
				goto jsondone;
			}
			sdp = NULL;
			ps_flags_clear (&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
		}
		
		if (handle->app == NULL || handle->app_handle == NULL || ps_plugin_session_is_alive (handle->app_handle)) {
			ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE, "no plugin to handle this message");
			if (jsep_type) g_free (jsep_type);
			if (jsep_sdp_stripped) g_free (jsep_sdp_stripped);
			ps_flags_clear (&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
			goto jsondone;
		}
		
		/* Send the message to the plugin, should eventually free transaction_text, body_text, jsep_type and sdp */
		char * body_text = json_dumps (body, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
		ps_plugin_result * result = plugin_t->handle_message (handle->app_handle, g_strdup((char *) transaction_text), body_text, jsep_type, jsep_sdp_stripped);
		if (result == NULL) {
			ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE, "Plugin didn't give a result");
			goto jsondone;
		}
		if (result->type == PS_PLUGIN_OK) {
			if (result->content == NULL) {
				ret = ps_process_error (request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE, "Plugin didn't provide any content for this synchronous response");
				ps_plugin_result_destroy (result);
				goto jsondone;
			}
			json_error_t error;
			json_t * event = json_loads (result->content, 0, &error);
			if(!event) {
				PS_LOG(LOG_ERR, "[%"SCNu64"] Cannot send response from plugin (JSON error: on line %d: %s)\n", handle->handle_id, error.line, error.text);
				ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE, "Plugin returned an invalid JSON response");
				ps_plugin_result_destroy(result);
				goto jsondone;
			}
			if(!json_is_object(event)) {
				PS_LOG(LOG_ERR, "[%"SCNu64"] Cannot send response from plugin (JSON error: not an object)\n", handle->handle_id);
				json_decref(event);
				ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE, "Plugin returned an invalid JSON response");
				ps_plugin_result_destroy(result);
				goto jsondone;
			}
			/* Prepare JSON response */
			json_t *reply = json_object();
			json_object_set_new(reply, "janus", json_string("success"));
			json_object_set_new(reply, "session_id", json_integer(session->session_id));
			json_object_set_new(reply, "sender", json_integer(handle->handle_id));
			json_object_set_new(reply, "transaction", json_string(transaction_text));
			json_t *plugin_data = json_object();
			json_object_set_new(plugin_data, "plugin", json_string(plugin_t->get_package()));
			json_object_set_new(plugin_data, "data", event);
			json_object_set_new(reply, "plugindata", plugin_data);
			/* Send the success reply */
			ret = ps_process_success(request, reply);
		} else if (result->type == PS_PLUGIN_OK_WAIT) {
			/* The plugin received the request but didn't process it yet, send an ack (asynchronous notifications may follow) */
			json_t *reply = json_object();
			json_object_set_new(reply, "janus", json_string("ack"));
			json_object_set_new(reply, "session_id", json_integer(session_id));
			if(result->content)
				json_object_set_new(reply, "hint", json_string(result->content));
			json_object_set_new(reply, "transaction", json_string(transaction_text));
			/* Send the success reply */
			ret = ps_process_success(request, reply);
		} else {
			/* Something went horribly wrong! */
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE, "%s", result->content ? g_strdup(result->content) : "Plugin returned a severe (unknown) error");
			ps_plugin_result_destroy(result);
			goto jsondone;
		}
		ps_plugin_result_destroy (result);
	} else if (!strcasecmp(message_text, "trickle")) {
		if(handle == NULL) {
			/* Trickle is an handle-level command */
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		if(handle->app == NULL || handle->app_handle == NULL || !ps_plugin_session_is_alive(handle->app_handle)) {
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE, "No plugin to handle this trickle candidate");
			goto jsondone;
		}
		json_t *candidate = json_object_get(root, "candidate");
		json_t *candidates = json_object_get(root, "candidates");
		if(candidate == NULL && candidates == NULL) {
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_MISSING_MANDATORY_ELEMENT, "Missing mandatory element (candidate|candidates)");
			goto jsondone;
		}
		if(candidate != NULL && candidates != NULL) {
			ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_JSON, "Can't have both candidate and candidates");
			goto jsondone;
		}
		ps_mutex_lock(&handle->mutex);
		if(!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_TRICKLE)) {
			/* It looks like this peer supports Trickle, after all */
			PS_LOG(LOG_VERB, "Handle %"SCNu64" supports trickle even if it didn't negotiate it...\n", handle->handle_id);
			ps_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_TRICKLE);
		}
		/* Is there any stream ready? this trickle may get here before the SDP it relates to */
		if(handle->audio_stream == NULL && handle->video_stream == NULL && handle->data_stream == NULL) {
			PS_LOG(LOG_WARN, "[%"SCNu64"] No stream, queueing this trickle as it got here before the SDP...\n", handle->handle_id);
			/* Enqueue this trickle candidate(s), we'll process this later */
			janus_ice_trickle *early_trickle = janus_ice_trickle_new(handle, transaction_text, candidate ? candidate : candidates);
			handle->pending_trickles = g_list_append(handle->pending_trickles, early_trickle);
			/* Send the ack right away, an event will tell the application if the candidate(s) failed */
			goto trickledone;
		}
		/* Is the ICE stack ready already? */
		if(ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER) ||
				!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_OFFER) ||
				!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER)) {
			const char *cause = NULL;
			if(ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER))
				cause = "processing the offer";
			else if(!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER))
				cause = "waiting for the answer";
			else if(!ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_OFFER))
				cause = "waiting for the offer";
			PS_LOG(LOG_VERB, "[%"SCNu64"] Still %s, queueing this trickle to wait until we're done there...\n",
				handle->handle_id, cause);
			/* Enqueue this trickle candidate(s), we'll process this later */
			janus_ice_trickle *early_trickle = janus_ice_trickle_new(handle, transaction_text, candidate ? candidate : candidates);
			handle->pending_trickles = g_list_append(handle->pending_trickles, early_trickle);
			/* Send the ack right away, an event will tell the application if the candidate(s) failed */
			goto trickledone;
		}
		if(candidate != NULL) {
			/* We got a single candidate */
			int error = 0;
			const char *error_string = NULL;
			if((error = janus_ice_trickle_parse(handle, candidate, &error_string)) != 0) {
				ret = ps_process_error(request, session_id, transaction_text, error, "%s", error_string);
				ps_mutex_unlock(&handle->mutex);
				goto jsondone;
			}
		} else {
			/* We got multiple candidates in an array */
			if(!json_is_array(candidates)) {
				ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_ELEMENT_TYPE, "candidates is not an array");
				ps_mutex_unlock(&handle->mutex);
				goto jsondone;
			}
			PS_LOG(LOG_VERB, "Got multiple candidates (%zu)\n", json_array_size(candidates));
			if(json_array_size(candidates) > 0) {
				/* Handle remote candidates */
				size_t i = 0;
				for(i=0; i<json_array_size(candidates); i++) {
					json_t *c = json_array_get(candidates, i);
					/* FIXME We don't care if any trickle fails to parse */
					janus_ice_trickle_parse(handle, c, NULL);
				}
			}
		}

trickledone:
		ps_mutex_unlock(&handle->mutex);
		/* We reply right away, not to block the web server... */
		json_t *reply = json_object();
		json_object_set_new(reply, "janus", json_string("ack"));
		json_object_set_new(reply, "session_id", json_integer(session_id));
		json_object_set_new(reply, "transaction", json_string(transaction_text));
		/* Send the success reply */
		ret = ps_process_success(request, reply);
	} else {
		ret = ps_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN_REQUEST, "Unknown request '%s'", message_text);
	}
	
	//json_t * reply = json_object();
	//json_object_set_new(reply, "cmd", json_string("will do"));
	//ret = ps_process_success (request, reply);
	//PS_PRINT ("Command received: %s\n", command_text);
	
jsondone:	
	return ret;
}

int ps_process_success (ps_request * request, json_t * payload) {
	if (!request || !payload) return -1;
	return request->transport->send_message(request->instance, payload);
}

int ps_process_error (ps_request * request, uint64_t session_id, const char * transaction, gint error, const char * format, ...) {
	if (!request) return -1;
	gchar * error_string = NULL;
	gchar error_buf[512];
	
	if (format == NULL) {
		error_string = (gchar *) janus_get_api_error(error);
	} else {
		va_list ap;
		va_start(ap, format);
		g_vsnprintf(error_buf, sizeof(error_buf), format, ap);
		va_end(ap);
		error_string = error_buf;
	}
	
	PS_LOG (LOG_VERB, "[%s] returning API error %d (%s)\n", transaction, error, error_string);
	json_t * reply = json_object ();
	json_object_set_new (reply, "janus", json_string("error"));
	if (session_id > 0) json_object_set_new (reply, "session_id", json_integer(session_id));
	if (transaction != NULL) json_object_set_new (reply, "transaction", json_string(transaction));
	
	json_t * error_data = json_object ();
	json_object_set_new (error_data, "code", json_integer(error));
	json_object_set_new (error_data, "reason", json_string(error_string));
	json_object_set_new (reply, "error", error_data);
	return request->transport->send_message (request->instance, reply);	
}

/* Transport callback interface */
void ps_transport_incoming_request (ps_transport * plugin, void * transport, json_t * message, json_error_t * error) {
	//PS_LOG (LOG_VERB, "Got API request from %s (%p)\n", plugin->get_package(), transport);
	ps_request * request = ps_request_new (plugin, transport, message);
	GError * tperror = NULL;
	g_thread_pool_push (tasks, request, &tperror);
	if (tperror != NULL) {
		PS_LOG (LOG_ERR,"Got error %d (%s) trying to push task in thread pool..\n", tperror->code, tperror->message ? tperror->message : "??");
		ps_request_destroy (request);
	}
}

void ps_transport_gone (ps_transport * plugin, void * transport) {
	PS_LOG (LOG_VERB,"Transport gone (%p)...\n", transport);
	ps_mutex_lock (&sessions_mutex);
	if (sessions && g_hash_table_size (sessions) > 0) {
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init (&iter, sessions);
		while (g_hash_table_iter_next (&iter, NULL, &value)) {
			ps_session * session = (ps_session *) value;
			if (!session || session->destroy || session->timeout || session->last_activity == 0) continue;
			if (session->source && session->source->instance == transport) {
				PS_LOG (LOG_VERB, " -- Marking sesion %"SCNu64" as over \n", session->session_id);
				session->last_activity = 0;
			}
		}
	}
	ps_mutex_unlock (&sessions_mutex);
}

void ps_transport_task (gpointer data, gpointer user_data) {
	ps_request * request = (ps_request *)data;
	if (request == NULL) {
		PS_LOG (LOG_ERR,"ps_transport_task: Invalid request...\n");
		return;
	}
	ps_process_incoming_request(request);
	ps_request_destroy(request);
}

/* Plugins */
ps_plugin * ps_plugin_find (const gchar * package) {
	if (package != NULL && plugins != NULL) return g_hash_table_lookup (plugins, package);
	return NULL;
}

void ps_plugin_close (gpointer key, gpointer value, gpointer user_data) {
	ps_plugin * plugin = (ps_plugin *) value;
	if (!plugin) return;
	plugin->destroy();
}

/* plugin callback interface */
int ps_plugin_push_event (ps_plugin_session * plugin_session, ps_plugin * plugin, const char * transaction, const char * message, const char * sdp_type, const char * sdp) {
	if (!plugin || !message) return -1;
	if (!plugin_session || plugin_session < (ps_plugin_session *)0x1000 || !ps_plugin_session_is_alive (plugin_session) || plugin_session->stopped) return -2;
	
	janus_ice_handle * ice_handle = (janus_ice_handle *)plugin_session->gateway_handle;
	
	if (!ice_handle || ps_flags_is_set (&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)) return JANUS_ERROR_SESSION_NOT_FOUND;
	
	ps_session * session = ice_handle->session;
	if (!session || session->destroy) return JANUS_ERROR_SESSION_NOT_FOUND;
	
	json_error_t error;
	json_t * plugin_event = json_loads (message, 0, &error);
	if(!plugin_event) {
		PS_LOG(LOG_ERR, "[%"SCNu64"] Cannot push event (JSON error: on line %d: %s)\n", ice_handle->handle_id, error.line, error.text);
		return JANUS_ERROR_INVALID_JSON;
	}
	if(!json_is_object(plugin_event)) {
		PS_LOG(LOG_ERR, "[%"SCNu64"] Cannot push event (JSON error: not an object)\n", ice_handle->handle_id);
		return JANUS_ERROR_INVALID_JSON_OBJECT;
	}
	
	json_t * jsep = NULL;
	if (sdp_type != NULL && sdp != NULL) {
		jsep = ps_plugin_handle_sdp (plugin_session, plugin, sdp_type, sdp);
		if (jsep == NULL) {
			if(ice_handle == NULL || ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP) || ps_flags_is_set (&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT)) {
				PS_LOG(LOG_ERR, "[%"SCNu64"] Cannot push event (handle not available anymore or negotiation stopped)\n", ice_handle->handle_id);
				return JANUS_ERROR_HANDLE_NOT_FOUND;
			} else {
				PS_LOG(LOG_ERR, "[%"SCNu64"] Cannot push event (JSON error: problem with the SDP)\n", ice_handle->handle_id);
				return JANUS_ERROR_JSEP_INVALID_SDP;
			}
		}
	}
	
	/* Prepare JSON event */
	json_t *event = json_object();
	json_object_set_new(event, "janus", json_string("event"));
	json_object_set_new(event, "session_id", json_integer(session->session_id));
	json_object_set_new(event, "sender", json_integer(ice_handle->handle_id));
	if(transaction != NULL)
		json_object_set_new(event, "transaction", json_string(transaction));
	json_t *plugin_data = json_object();
	json_object_set_new(plugin_data, "plugin", json_string(plugin->get_package()));
	json_object_set_new(plugin_data, "data", plugin_event);
	json_object_set_new(event, "plugindata", plugin_data);
	if(jsep != NULL)
		json_object_set_new(event, "jsep", jsep);
	/* Send the event */
	PS_LOG(LOG_VERB, "[%"SCNu64"] Sending event to transport...\n", ice_handle->handle_id);
	ps_session_notify_event(session->session_id, event);
	
	return JANUS_OK;
}

json_t * ps_plugin_handle_sdp (ps_plugin_session * plugin_session, ps_plugin * plugin, const char * sdp_type, const char * sdp) {
	if (!plugin_session || plugin_session < (ps_plugin_session *)0x1000 || !ps_plugin_session_is_alive (plugin_session) || plugin_session->stopped || plugin == NULL || sdp_type == NULL || sdp == NULL) {
		PS_LOG (LOG_ERR, "Invalid arguments\n");
		return NULL;
	}
	
	janus_ice_handle * ice_handle = (janus_ice_handle *) plugin_session->gateway_handle;
	if (ice_handle == NULL) {
		PS_LOG (LOG_ERR, "Invalid ICE handle\n");
		return NULL;
	}
	int offer = 0;
	if (!strcasecmp (sdp_type, "offer")) {
		offer = 1;
		ps_flags_set (&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_OFFER);
		ps_flags_clear (&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER);
	} else if (!strcasecmp (sdp_type, "answer")) {
		ps_flags_set (&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER);
	} else {
		PS_LOG (LOG_ERR, "Unknown type '%s'\n", sdp_type);
		return NULL;
	}
	
	/* Is this valid SDP? */
	int audio = 0, video = 0, data = 0, bundle = 0, rtcpmux = 0, trickle = 0;
	janus_sdp *parsed_sdp = janus_sdp_preparse(sdp, &audio, &video, &data, &bundle, &rtcpmux, &trickle);
	if(parsed_sdp == NULL) {
		PS_LOG(LOG_ERR, "[%"SCNu64"] Couldn't parse SDP...\n", ice_handle->handle_id);
		return NULL;
	}
	janus_sdp_free(parsed_sdp);
	gboolean updating = FALSE;
	if(offer) {
		/* We still don't have a local ICE setup */
		PS_LOG(LOG_VERB, "[%"SCNu64"] Audio %s been negotiated\n", ice_handle->handle_id, audio ? "has" : "has NOT");
		if(audio > 1) {
			PS_LOG(LOG_ERR, "[%"SCNu64"] More than one audio line? only going to negotiate one...\n", ice_handle->handle_id);
		}
		PS_LOG(LOG_VERB, "[%"SCNu64"] Video %s been negotiated\n", ice_handle->handle_id, video ? "has" : "has NOT");
		if(video > 1) {
			PS_LOG(LOG_ERR, "[%"SCNu64"] More than one video line? only going to negotiate one...\n", ice_handle->handle_id);
		}
		PS_LOG(LOG_VERB, "[%"SCNu64"] SCTP/DataChannels %s been negotiated\n", ice_handle->handle_id, data ? "have" : "have NOT");
		if(data > 1) {
			PS_LOG(LOG_ERR, "[%"SCNu64"] More than one data line? only going to negotiate one...\n", ice_handle->handle_id);
		}
		/* Are we still cleaning up from a previous media session? */
		if(ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_CLEANING)) {
			PS_LOG(LOG_VERB, "[%"SCNu64"] Still cleaning up from a previous media session, let's wait a bit...\n", ice_handle->handle_id);
			gint64 waited = 0;
			while(ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_CLEANING)) {
				PS_LOG(LOG_VERB, "[%"SCNu64"] Still cleaning up from a previous media session, let's wait a bit...\n", ice_handle->handle_id);
				g_usleep(100000);
				waited += 100000;
				if(waited >= 3*G_USEC_PER_SEC) {
					PS_LOG(LOG_VERB, "[%"SCNu64"]   -- Waited 3 seconds, that's enough!\n", ice_handle->handle_id);
					break;
				}
			}
		}
		if(ice_handle->agent == NULL) {
			/* Process SDP in order to setup ICE locally (this is going to result in an answer from the browser) */
			if(janus_ice_setup_local(ice_handle, 0, audio, video, data, bundle, rtcpmux, trickle) < 0) {
				PS_LOG(LOG_ERR, "[%"SCNu64"] Error setting ICE locally\n", ice_handle->handle_id);
				return NULL;
			}
		} else {
			updating = TRUE;
			PS_LOG(LOG_INFO, "[%"SCNu64"] Updating existing session\n", ice_handle->handle_id);
		}
	}
	if(!updating) {
		/* Wait for candidates-done callback */
		while(ice_handle->cdone < ice_handle->streams_num) {
			if(ice_handle == NULL || ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)
					|| ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT)) {
				PS_LOG(LOG_WARN, "[%"SCNu64"] Handle detached or PC closed, giving up...!\n", ice_handle ? ice_handle->handle_id : 0);
				return NULL;
			}
			PS_LOG(LOG_VERB, "[%"SCNu64"] Waiting for candidates-done callback...\n", ice_handle->handle_id);
			g_usleep(100000);
			if(ice_handle->cdone < 0) {
				PS_LOG(LOG_ERR, "[%"SCNu64"] Error gathering candidates!\n", ice_handle->handle_id);
				return NULL;
			}
		}
	}
	/* Anonymize SDP */
	char *sdp_stripped = janus_sdp_anonymize(sdp);
	if(sdp_stripped == NULL) {
		/* Invalid SDP */
		PS_LOG(LOG_ERR, "[%"SCNu64"] Invalid SDP\n", ice_handle->handle_id);
		return NULL;
	}
	/* Add our details */
	char *sdp_merged = janus_sdp_merge(ice_handle, sdp_stripped);
	if(sdp_merged == NULL) {
		/* Couldn't merge SDP */
		PS_LOG(LOG_ERR, "[%"SCNu64"] Error merging SDP\n", ice_handle->handle_id);
		g_free(sdp_stripped);
		return NULL;
	}
	/* FIXME Any disabled m-line? */
	if(strstr(sdp_merged, "m=audio 0")) {
		PS_LOG(LOG_VERB, "[%"SCNu64"] Audio disabled via SDP\n", ice_handle->handle_id);
		if(!ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE)
				|| (!video && !data)) {
			PS_LOG(LOG_VERB, "[%"SCNu64"]   -- Marking audio stream as disabled\n", ice_handle->handle_id);
			janus_ice_stream *stream = g_hash_table_lookup(ice_handle->streams, GUINT_TO_POINTER(ice_handle->audio_id));
			if(stream)
				stream->disabled = TRUE;
		}
	}
	if(strstr(sdp_merged, "m=video 0")) {
		PS_LOG(LOG_VERB, "[%"SCNu64"] Video disabled via SDP\n", ice_handle->handle_id);
		if(!ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE)
				|| (!audio && !data)) {
			PS_LOG(LOG_VERB, "[%"SCNu64"]   -- Marking video stream as disabled\n", ice_handle->handle_id);
			janus_ice_stream *stream = NULL;
			if(!ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE)) {
				stream = g_hash_table_lookup(ice_handle->streams, GUINT_TO_POINTER(ice_handle->video_id));
			} else {
				gint id = ice_handle->audio_id > 0 ? ice_handle->audio_id : ice_handle->video_id;
				stream = g_hash_table_lookup(ice_handle->streams, GUINT_TO_POINTER(id));
			}
			if(stream)
				stream->disabled = TRUE;
		}
	}
	if(strstr(sdp_merged, "m=application 0 DTLS/SCTP")) {
		PS_LOG(LOG_VERB, "[%"SCNu64"] Data Channel disabled via SDP\n", ice_handle->handle_id);
		if(!ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE)
				|| (!audio && !video)) {
			PS_LOG(LOG_VERB, "[%"SCNu64"]   -- Marking data channel stream as disabled\n", ice_handle->handle_id);
			janus_ice_stream *stream = NULL;
			if(!ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE)) {
				stream = g_hash_table_lookup(ice_handle->streams, GUINT_TO_POINTER(ice_handle->data_id));
			} else {
				gint id = ice_handle->audio_id > 0 ? ice_handle->audio_id : (ice_handle->video_id > 0 ? ice_handle->video_id : ice_handle->data_id);
				stream = g_hash_table_lookup(ice_handle->streams, GUINT_TO_POINTER(id));
			}
			if(stream)
				stream->disabled = TRUE;
		}
	}
	
	if(!updating) {
		if(offer) {
			/* We set the flag to wait for an answer before handling trickle candidates */
			ps_flags_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
		} else {
			PS_LOG(LOG_VERB, "[%"SCNu64"] Done! Ready to setup remote candidates and send connectivity checks...\n", ice_handle->handle_id);
			if(ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_BUNDLE) && audio && video) {
				PS_LOG(LOG_VERB, "[%"SCNu64"]   -- bundle is supported by the browser, getting rid of one of the RTP/RTCP components, if any...\n", ice_handle->handle_id);
				if(audio) {
					/* Get rid of video and data, if present */
					if(ice_handle->streams && ice_handle->video_stream) {
						ice_handle->audio_stream->video_ssrc = ice_handle->video_stream->video_ssrc;
						ice_handle->audio_stream->video_ssrc_peer = ice_handle->video_stream->video_ssrc_peer;
						ice_handle->audio_stream->video_ssrc_peer_rtx = ice_handle->video_stream->video_ssrc_peer_rtx;
						nice_agent_attach_recv(ice_handle->agent, ice_handle->video_stream->stream_id, 1, g_main_loop_get_context (ice_handle->iceloop), NULL, NULL);
						if(!janus_ice_is_rtcpmux_forced())
							nice_agent_attach_recv(ice_handle->agent, ice_handle->video_stream->stream_id, 2, g_main_loop_get_context (ice_handle->iceloop), NULL, NULL);
						nice_agent_remove_stream(ice_handle->agent, ice_handle->video_stream->stream_id);
						janus_ice_stream_free(ice_handle->streams, ice_handle->video_stream);
					}
					ice_handle->video_stream = NULL;
					ice_handle->video_id = 0;
					if(ice_handle->streams && ice_handle->data_stream) {
						nice_agent_attach_recv(ice_handle->agent, ice_handle->data_stream->stream_id, 1, g_main_loop_get_context (ice_handle->iceloop), NULL, NULL);
						nice_agent_remove_stream(ice_handle->agent, ice_handle->data_stream->stream_id);
						janus_ice_stream_free(ice_handle->streams, ice_handle->data_stream);
					}
					ice_handle->data_stream = NULL;
					ice_handle->data_id = 0;
				} else if(video) {
					/* Get rid of data, if present */
					if(ice_handle->streams && ice_handle->data_stream) {
						nice_agent_attach_recv(ice_handle->agent, ice_handle->data_stream->stream_id, 1, g_main_loop_get_context (ice_handle->iceloop), NULL, NULL);
						nice_agent_remove_stream(ice_handle->agent, ice_handle->data_stream->stream_id);
						janus_ice_stream_free(ice_handle->streams, ice_handle->data_stream);
					}
					ice_handle->data_stream = NULL;
					ice_handle->data_id = 0;
				}
			}
			if(ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RTCPMUX) && !janus_ice_is_rtcpmux_forced()) {
				PS_LOG(LOG_VERB, "[%"SCNu64"]   -- rtcp-mux is supported by the browser, getting rid of RTCP components, if any...\n", ice_handle->handle_id);
				if(ice_handle->audio_stream && ice_handle->audio_stream->rtcp_component && ice_handle->audio_stream->components != NULL) {
					nice_agent_attach_recv(ice_handle->agent, ice_handle->audio_id, 2, g_main_loop_get_context (ice_handle->iceloop), NULL, NULL);
					/* Free the component */
					janus_ice_component_free(ice_handle->audio_stream->components, ice_handle->audio_stream->rtcp_component);
					ice_handle->audio_stream->rtcp_component = NULL;
					/* Create a dummy candidate and enforce it as the one to use for this now unneeded component */
					NiceCandidate *c = nice_candidate_new(NICE_CANDIDATE_TYPE_HOST);
					c->component_id = 2;
					c->stream_id = ice_handle->audio_stream->stream_id;
#ifndef HAVE_LIBNICE_TCP
					c->transport = NICE_CANDIDATE_TRANSPORT_UDP;
#endif
					strncpy(c->foundation, "1", NICE_CANDIDATE_MAX_FOUNDATION);
					c->priority = 1;
					nice_address_set_from_string(&c->addr, "127.0.0.1");
					nice_address_set_port(&c->addr, janus_ice_get_rtcpmux_blackhole_port());
					c->username = g_strdup(ice_handle->audio_stream->ruser);
					c->password = g_strdup(ice_handle->audio_stream->rpass);
					if(!nice_agent_set_selected_remote_candidate(ice_handle->agent, ice_handle->audio_stream->stream_id, 2, c)) {
						PS_LOG(LOG_ERR, "[%"SCNu64"] Error forcing dummy candidate on RTCP component of stream %d\n", ice_handle->handle_id, ice_handle->audio_stream->stream_id);
						nice_candidate_free(c);
					}
				}
				if(ice_handle->video_stream && ice_handle->video_stream->rtcp_component && ice_handle->video_stream->components != NULL) {
					nice_agent_attach_recv(ice_handle->agent, ice_handle->video_id, 2, g_main_loop_get_context (ice_handle->iceloop), NULL, NULL);
					/* Free the component */
					janus_ice_component_free(ice_handle->video_stream->components, ice_handle->video_stream->rtcp_component);
					ice_handle->video_stream->rtcp_component = NULL;
					/* Create a dummy candidate and enforce it as the one to use for this now unneeded component */
					NiceCandidate *c = nice_candidate_new(NICE_CANDIDATE_TYPE_HOST);
					c->component_id = 2;
					c->stream_id = ice_handle->video_stream->stream_id;
#ifndef HAVE_LIBNICE_TCP
					c->transport = NICE_CANDIDATE_TRANSPORT_UDP;
#endif
					strncpy(c->foundation, "1", NICE_CANDIDATE_MAX_FOUNDATION);
					c->priority = 1;
					nice_address_set_from_string(&c->addr, "127.0.0.1");
					nice_address_set_port(&c->addr, janus_ice_get_rtcpmux_blackhole_port());
					c->username = g_strdup(ice_handle->video_stream->ruser);
					c->password = g_strdup(ice_handle->video_stream->rpass);
					if(!nice_agent_set_selected_remote_candidate(ice_handle->agent, ice_handle->video_stream->stream_id, 2, c)) {
						PS_LOG(LOG_ERR, "[%"SCNu64"] Error forcing dummy candidate on RTCP component of stream %d\n", ice_handle->handle_id, ice_handle->video_stream->stream_id);
						nice_candidate_free(c);
					}
				}
			}
			ps_mutex_lock(&ice_handle->mutex);
			/* We got our answer */
			ps_flags_clear(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
			/* Any pending trickles? */
			if(ice_handle->pending_trickles) {
				PS_LOG(LOG_VERB, "[%"SCNu64"]   -- Processing %d pending trickle candidates\n", ice_handle->handle_id, g_list_length(ice_handle->pending_trickles));
				GList *temp = NULL;
				while(ice_handle->pending_trickles) {
					temp = g_list_first(ice_handle->pending_trickles);
					ice_handle->pending_trickles = g_list_remove_link(ice_handle->pending_trickles, temp);
					janus_ice_trickle *trickle = (janus_ice_trickle *)temp->data;
					g_list_free(temp);
					if(trickle == NULL)
						continue;
					if((ps_get_monotonic_time() - trickle->received) > 15*G_USEC_PER_SEC) {
						/* FIXME Candidate is too old, discard it */
						janus_ice_trickle_destroy(trickle);
						/* FIXME We should report that */
						continue;
					}
					json_t *candidate = trickle->candidate;
					if(candidate == NULL) {
						janus_ice_trickle_destroy(trickle);
						continue;
					}
					if(json_is_object(candidate)) {
						/* We got a single candidate */
						int error = 0;
						const char *error_string = NULL;
						if((error = janus_ice_trickle_parse(ice_handle, candidate, &error_string)) != 0) {
							/* FIXME We should report the error parsing the trickle candidate */
						}
					} else if(json_is_array(candidate)) {
						/* We got multiple candidates in an array */
						PS_LOG(LOG_VERB, "[%"SCNu64"] Got multiple candidates (%zu)\n", ice_handle->handle_id, json_array_size(candidate));
						if(json_array_size(candidate) > 0) {
							/* Handle remote candidates */
							size_t i = 0;
							for(i=0; i<json_array_size(candidate); i++) {
								json_t *c = json_array_get(candidate, i);
								/* FIXME We don't care if any trickle fails to parse */
								janus_ice_trickle_parse(ice_handle, c, NULL);
							}
						}
					}
					/* Done, free candidate */
					janus_ice_trickle_destroy(trickle);
				}
			}
			/* This was an answer, check if it's time to start ICE */
			if(ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_TRICKLE) &&
					!ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALL_TRICKLES)) {
				PS_LOG(LOG_VERB, "[%"SCNu64"]   -- ICE Trickling is supported by the browser, waiting for remote candidates...\n", ice_handle->handle_id);
				ps_flags_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_START);
			} else {
				PS_LOG(LOG_VERB, "[%"SCNu64"] Done! Sending connectivity checks...\n", ice_handle->handle_id);
				if(ice_handle->audio_id > 0) {
					janus_ice_setup_remote_candidates(ice_handle, ice_handle->audio_id, 1);
					if(!ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RTCPMUX))	/* http://tools.ietf.org/html/rfc5761#section-5.1.3 */
						janus_ice_setup_remote_candidates(ice_handle, ice_handle->audio_id, 2);
				}
				if(ice_handle->video_id > 0) {
					janus_ice_setup_remote_candidates(ice_handle, ice_handle->video_id, 1);
					if(!ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RTCPMUX))	/* http://tools.ietf.org/html/rfc5761#section-5.1.3 */
						janus_ice_setup_remote_candidates(ice_handle, ice_handle->video_id, 2);
				}
				if(ice_handle->data_id > 0) {
					janus_ice_setup_remote_candidates(ice_handle, ice_handle->data_id, 1);
				}
			}
			ps_mutex_unlock(&ice_handle->mutex);
		}
	}
	
	/* Prepare JSON event */
	json_t *jsep = json_object();
	json_object_set_new(jsep, "type", json_string(sdp_type));
	json_object_set_new(jsep, "sdp", json_string(sdp_merged));
	g_free(sdp_stripped);
	//~ g_free(sdp_merged);
	ice_handle->local_sdp = sdp_merged;
	return jsep;
}

void ps_plugin_relay_rtp (ps_plugin_session * plugin_session, int video, char * buf, int len) {
	if((plugin_session < (ps_plugin_session *)0x1000) || plugin_session->stopped || buf == NULL || len < 1)
		return;
	janus_ice_handle *handle = (janus_ice_handle *)plugin_session->gateway_handle;
	if(!handle || ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)
			|| ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT))
		return;
	
	janus_ice_relay_rtp(handle, video, buf, len);
}

void ps_plugin_relay_rtcp (ps_plugin_session * plugin_session, int video, char * buf, int len) {
	if((plugin_session < (ps_plugin_session *)0x1000) || plugin_session->stopped || buf == NULL || len < 1)
		return;
	janus_ice_handle *handle = (janus_ice_handle *)plugin_session->gateway_handle;
	if(!handle || ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)
			|| ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT))
		return;
	
	janus_ice_relay_rtcp(handle, video, buf, len);
}

void ps_plugin_relay_data (ps_plugin_session * plugin_session, char * buf, int len) {
	if((plugin_session < (ps_plugin_session *)0x1000) || plugin_session->stopped || buf == NULL || len < 1)
		return;
	janus_ice_handle *handle = (janus_ice_handle *)plugin_session->gateway_handle;
	if(!handle || ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)
			|| ps_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT))
		return;

	janus_ice_relay_data(handle, buf, len);
}

void ps_plugin_close_pc (ps_plugin_session * plugin_session) {
	/* A plugin asked to get rid of a PeerConnection */
	if((plugin_session < (ps_plugin_session *)0x1000) || !ps_plugin_session_is_alive(plugin_session) || plugin_session->stopped)
		return;
	janus_ice_handle *ice_handle = (janus_ice_handle *)plugin_session->gateway_handle;
	if(!ice_handle)
		return;
	if(ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)
			|| ps_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT))
		return;
	ps_session *session = (ps_session *)ice_handle->session;
	if(!session)
		return;
		
	PS_LOG(LOG_VERB, "[%"SCNu64"] Plugin asked to hangup PeerConnection: sending alert\n", ice_handle->handle_id);
	/* Send an alert on all the DTLS connections */
	janus_ice_webrtc_hangup(ice_handle);
}

void ps_plugin_end_session (ps_plugin_session * plugin_session) {
	/* A plugin asked to get rid of a handle */
	if((plugin_session < (ps_plugin_session *)0x1000) || !ps_plugin_session_is_alive(plugin_session) || plugin_session->stopped)
		return;
	janus_ice_handle *ice_handle = (janus_ice_handle *)plugin_session->gateway_handle;
	if(!ice_handle)
		return;
	ps_session *session = (ps_session *)ice_handle->session;
	if(!session)
		return;
	/* Destroy the handle */
	janus_ice_handle_destroy(session, ice_handle->handle_id);
	ps_mutex_lock(&session->mutex);
	g_hash_table_remove(session->ice_handles, GUINT_TO_POINTER(ice_handle->handle_id));
	ps_mutex_unlock(&session->mutex);
}

static void ps_detect_local_ip (gchar * buf, size_t buflen) {
	PS_LOG (LOG_VERB, "Autodetecting local IP...\n");
	struct sockaddr_in addr;
	socklen_t len;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd == -1) goto error;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(1);
	inet_pton(AF_INET, "1.2.3.4", &addr.sin_addr.s_addr);
	if (connect(fd, (const struct sockaddr*) &addr, sizeof(addr)) < 0)
			goto error;
	len = sizeof(addr);
	if (getsockname(fd, (struct sockaddr*) &addr, &len) < 0)
			goto error;
	if (getnameinfo((const struct sockaddr*) &addr, sizeof(addr),
					buf, buflen,
					NULL, 0, NI_NUMERICHOST) != 0)
			goto error;
	close(fd);
	return;
	
error:
	if (fd != -1) close (fd);
	PS_LOG (LOG_VERB, "Couldn't find any address! Using 127.0.0.1 as local IP\n");
	g_strlcpy(buf, "127.0.0.1", buflen);
}

/* Signal Handler */
static void ps_handle_signal(int signum) {
	switch (g_atomic_int_get(&stop)) {
		case 0:
			PS_PRINT ("Exiting... wait!!\n");
			break;
		case 1:
			PS_PRINT ("Don't be in hurry, freeing resources...\n");
			break;
		default:
			PS_PRINT ("Aborting immediately...\n");
			break;
	}
	g_atomic_int_inc(&stop);
	
	if (g_atomic_int_get(&stop) > 2) exit(1);
}

static void ps_termination_handler (void) {
	ps_log_destroy ();
}

gint main (int argc, char * argv[]) {
	
	/* to allow core dumps */
	struct rlimit core_limits;
	core_limits.rlim_cur = core_limits.rlim_max = RLIM_INFINITY;
	setrlimit(RLIMIT_CORE, &core_limits);
	
	char file[255];
	g_snprintf(file, 255, "./ps.cfg");
	config_file = g_strdup(file);
	
	if ((config = ps_config_parse(config_file)) == NULL) {
		g_print("Error reading configuration from %s\n", config_file);
		exit(1);
	}
	
	gboolean use_stdout = TRUE;
	ps_config_item * item = ps_config_get_item_drilldown(config, "general", "console_ouput");
	if (item && item->value && !ps_is_true(item->value)) use_stdout = FALSE;
	
	gboolean daemonize = FALSE;
	const char *logfile = NULL;
	
	logfile = "pslog.log";
	item = ps_config_get_item_drilldown(config, "general", "log_output");
	if (item && item->value) logfile = item->value;
	
	if (ps_log_init(daemonize, use_stdout, logfile) < 0) {
		PS_LOG (LOG_ERR, "Unable to initialize the log file or console output\n");
		exit(1);
	}
	
	PS_PRINT ("Hello!\n");
	signal(SIGINT, ps_handle_signal);
	signal(SIGTERM, ps_handle_signal);
	atexit(ps_termination_handler);
	
	ps_detect_local_ip (local_ip, sizeof(local_ip));
	PS_LOG (LOG_INFO, "Local IP is %s\n",local_ip);
	
	char * stun_server = NULL, * turn_server = NULL;
	uint16_t stun_port = 0, turn_port = 0;
	uint16_t rtp_min_port = 0, rtp_max_port = 0;
	char * turn_type = NULL, * turn_user = NULL, * turn_pwd = NULL;
	const char * nat_1_1_mapping = NULL;
	gboolean ice_lite = FALSE, ice_tcp = FALSE, ipv6 = FALSE;
	gboolean nice_debug = FALSE;
	
	if (nat_1_1_mapping != NULL) {
		PS_LOG (LOG_INFO, "NAT 1:1 mapping enabled..\n");
		ps_set_public_ip (nat_1_1_mapping);
		janus_ice_enable_nat_1_1();
	}
	
	/* Initialize the ice stack */
	janus_ice_init(ice_lite, ice_tcp, ipv6, rtp_min_port, rtp_max_port);
	if (janus_ice_set_stun_server(stun_server, stun_port) < 0) {
		PS_LOG (LOG_FATAL, "Invalid STUN server %s:%u\n", stun_server, stun_port);
		exit(1);
	}
	if (janus_ice_set_turn_server(turn_server, turn_port, turn_type, turn_user, turn_pwd) < 0) {
		PS_LOG (LOG_FATAL, "Invalid TURN server %s:%u\n", turn_server, turn_port);
		exit(1);
	}
	
	if (stun_server == NULL && turn_server == NULL) {
		gboolean private_address = FALSE;
		const char * test_ip = local_ip;
		struct sockaddr_in addr;
		if (inet_pton(AF_INET, test_ip, &addr) > 0) {
			unsigned short int ip[4];
			sscanf (test_ip, "%hu.%hu.%hu.%hu", &ip[0], &ip[1], &ip[2], &ip[3]);
			if (ip[0] == 10) {
				private_address = TRUE;
			} else if (ip[0] == 172 && (ip[1] >= 16 && ip[1] <= 31)) {
				private_address = TRUE;
			} else if (ip[0] == 192 && ip[1] == 168) {
				private_address = TRUE;
			}
		}
		if (private_address) {
			PS_LOG (LOG_WARN, "Client working on private address (%s), but didn't specify any STUN server\n", test_ip);
		}
	}
	
	gboolean force_bundle = FALSE, force_rtcpmux = FALSE;
	janus_ice_force_bundle(force_bundle);
	janus_ice_force_rtcpmux(force_rtcpmux);
	
	item = ps_config_get_item_drilldown(config, "general", "debug_level");
	if (item && item->value) {
		int temp_level = atoi(item->value);
		if (temp_level == 0 && strcmp(item->value, "0")) {
			PS_PRINT ("Invalid debug level %s (configuration), using default (info=4)\n", item->value);
		} else {
			ps_log_level = temp_level;
			if (ps_log_level < LOG_NONE) ps_log_level = 0;
			else if (ps_log_level > LOG_MAX) ps_log_level = LOG_MAX;
		}
	}
	
	/* OpenSSL stuff */
	item = ps_config_get_item_drilldown (config, "certificates", "cert_pem");
	if (!item || ! item->value) {
		PS_LOG (LOG_FATAL, "No certificate/key found...\n");
		exit(1);
	}
	server_pem = (char *) item->value;
	
	item = ps_config_get_item_drilldown (config, "certificates", "cert_key");
	if (!item || ! item->value) {
		PS_LOG (LOG_FATAL, "No certificate/key found...\n");
		exit(1);
	}
	server_key = (char *) item->value;
	PS_LOG (LOG_VERB, "using certificates: \n\t%s\n\t%s\n", server_pem, server_key);
	SSL_library_init ();
	SSL_load_error_strings ();
	OpenSSL_add_all_algorithms ();
	if (janus_dtls_srtp_init (server_pem, server_key) < 0) {
		PS_LOG (LOG_FATAL, "Error initializing DTLS-SRTP..\n");
		exit(1);
	}
	/* Initialize SCTP data channels */
	if (janus_sctp_init () < 0) {
		PS_LOG (LOG_FATAL, "Error initializing SCTP data channels..\n");
		exit(1);
	}
	
	/* Initialize Sofia-SDP */
	if (janus_sdp_init () < 0) {
		PS_LOG (LOG_FATAL, "Error initializing Sofia-SDP..\n");
		exit(1);
	}
	
	plugins = g_hash_table_new (g_str_hash, g_str_equal);
	/* Load plugin */
	void * plugin = dlopen ("libpsgstreamer.so",RTLD_LAZY);
	if (!plugin) {
		PS_LOG (LOG_ERR, "Couldn't load libpsgstreamer.so: %s\n", dlerror());
		exit(1);
	}
	create_p *createp = (create_p*) dlsym(plugin, "create");
	const char * dlsym_errorp = dlerror();
	if (dlsym_errorp) {
		PS_LOG (LOG_ERR, "Couldn't load 'create' from libpsgstreamer.so: %s\n", dlsym_errorp);
		exit(1);
	}
	
	ps_plugin * ps_plugin = createp();
	ps_plugin->init (&ps_handler_plugin, configs_folder);
	g_hash_table_insert (plugins, (gpointer)ps_plugin->get_package(), ps_plugin);
	
	/* threadpool to handle incoming requests from transport */
	GError * error = NULL;
	tasks = g_thread_pool_new (ps_transport_task, NULL, -1, FALSE, &error);
	if (error != NULL) {
		PS_LOG (LOG_ERR,"Got error %d (%s) trying to start request pool task thread", error->code, error->message ? error->message : "??");
		exit(1);
	}
	
	void * transport = dlopen ("libpswebsockets.so",RTLD_LAZY);
	if (!transport) {
		PS_LOG (LOG_ERR,"Couldn't load libpswebsockets.so: %s\n", dlerror());
		exit(1);
	}
	create_t *createt = (create_t*) dlsym(transport, "create");
	const char * dlsym_errort = dlerror();
	if (dlsym_errort) {
		PS_LOG (LOG_ERR, "Couldn't load 'create' from libpswebsockets.so: %s\n", dlsym_errort);
		exit(1);
	}
	
	ps_transport * ps_transport = createt();
	ps_transport->init(&ps_handler);
	
	/* Sessions */
	sessions = g_hash_table_new (NULL, NULL);
	old_sessions = g_hash_table_new (NULL, NULL);
	ps_mutex_init (&sessions_mutex);
	
	sessions_watchdog_context = g_main_context_new ();
	GMainLoop * loop = g_main_loop_new (sessions_watchdog_context, FALSE);
	error = NULL;
	GThread * watchdog = g_thread_try_new ("watchdog", &ps_sessions_watchdog, loop, &error);
	if (error != NULL) {
		PS_LOG (LOG_ERR,"Got error %d (%s) trying to start sessions", error->code, error->message ? error->message : "??");
		exit(1);
	}
	
	while (!g_atomic_int_get(&stop)) {
		usleep (250000);
	}
	//g_usleep(5000000);
	
	/* Done */
	PS_LOG (LOG_INFO, "Shutting down...\n");
	g_main_loop_quit (loop);
	g_thread_join (watchdog);
	watchdog = NULL;
	g_main_loop_unref (loop);
	g_main_context_unref (sessions_watchdog_context);
	
	ps_transport->destroy();
	g_thread_pool_free (tasks, FALSE, FALSE);
	
	PS_LOG (LOG_INFO, "Destroying sessions...\n");
	if (sessions != NULL) g_hash_table_destroy(sessions);
	if (old_sessions != NULL) g_hash_table_destroy(old_sessions);
	
	PS_LOG (LOG_INFO, "Freeing crypto resources..\n");
	SSL_CTX_free (janus_dtls_get_ssl_ctx());
	EVP_cleanup ();
	ERR_free_strings ();
	
	PS_LOG (LOG_INFO, "Cleaning SDP structures..\n");
	janus_sdp_deinit ();
	
	PS_LOG (LOG_INFO, "De-initializing SCTP..\n");
	janus_sctp_deinit ();
	janus_ice_deinit ();
	
	PS_LOG (LOG_INFO, "Closing plugins..\n");
	if (plugins != NULL) {
		g_hash_table_foreach (plugins, ps_plugin_close, NULL);
		g_hash_table_destroy (plugins);
	}
	PS_PRINT ("Bye!\n");
	
	exit(0);
}



