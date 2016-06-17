#include <jansson.h>
#include <errno.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include "ps_gstreamer.h"
#include "../apierror.h"
#include "../config.h"
#include "../mutex.h"
#include "../debug.h"
#include "../rtp.h"
#include "../rtcp.h"
#include "../utils.h"

/* plugin information */
#define PS_GSTREAMER_VERSION			1
#define PS_GSTREAMER_VERSION_STRING		"0.0.1"
#define PS_GSTREAMER_DESCRIPTION		"PacketServo WebRTC client"
#define PS_GSTREAMER_NAME				"PS GStreamer WebRTC plugin"
#define PS_GSTREAMER_AUTHOR				"PS"
#define PS_GSTREAMER_PACKAGE			"ps.plugin.gstreamer"

ps_plugin * create(void);
int ps_gstreamer_init (ps_callbacks * callback, const char * config_path);
void ps_gstreamer_destroy (void);
int ps_gstreamer_get_api_compatibility (void);
int ps_gstreamer_get_version (void);
const char * ps_gstreamer_get_version_string (void);
const char * ps_gstreamer_get_description (void);
const char * ps_gstreamer_get_name (void);
const char * ps_gstreamer_get_author (void);
const char * ps_gstreamer_get_package (void);
void ps_gstreamer_create_session (ps_plugin_session * handle, int * error);
struct ps_plugin_result * ps_gstreamer_handle_message(ps_plugin_session * handle, char * transaction, char * message, char * sdp_type, char * sdp);
void ps_gstreamer_setup_media (ps_plugin_session * handle);
void ps_gstreamer_incoming_rtp (ps_plugin_session * handle, int video, char * buf, int len);
void ps_gstreamer_incoming_rtcp (ps_plugin_session * handle, int video, char * buf, int len);
void ps_gstreamer_incoming_data (ps_plugin_session * handle, char * buf, int len);
void ps_gstreamer_hangup_media (ps_plugin_session * handle);
void ps_gstreamer_destroy_session (ps_plugin_session * handle, int * error);
char * ps_gstreamer_query_session (ps_plugin_session * handle);

/* Plugin setup */
static ps_plugin ps_gstreamer_plugin = PS_PLUGIN_INIT (
	.init = ps_gstreamer_init,
	.destroy = ps_gstreamer_destroy,
	
	.get_api_compatibility = ps_gstreamer_get_api_compatibility,
	.get_version = ps_gstreamer_get_version,
	.get_version_string = ps_gstreamer_get_version_string,
	.get_description = ps_gstreamer_get_description,
	.get_name = ps_gstreamer_get_name,
	.get_author = ps_gstreamer_get_author,
	.get_package = ps_gstreamer_get_package,
	
	.create_session = ps_gstreamer_create_session,
	.handle_message = ps_gstreamer_handle_message,
	.setup_media = ps_gstreamer_setup_media,
	.incoming_rtp = ps_gstreamer_incoming_rtp,
	.incoming_rtcp = ps_gstreamer_incoming_rtcp,
	.incoming_data = ps_gstreamer_incoming_data,
	.hangup_media = ps_gstreamer_hangup_media,
	.destroy_session = ps_gstreamer_destroy_session,
	.query_session = ps_gstreamer_query_session,
);

/* plugin creator */
ps_plugin * create (void) {
	PS_LOG (LOG_VERB, "%s created!\n", PS_GSTREAMER_NAME);
	return &ps_gstreamer_plugin;
}

/*configuration*/
static ps_config * config = NULL;
static const char * config_file = NULL;
static ps_mutex config_mutex;

/* something useful */
static volatile gint initialized = 0, stopping = 0;
static ps_callbacks * gateway = NULL;
static GThread * handler_thread;
static GThread * watchdog;
static void * ps_gstreaming_handler (void * data);
static void ps_gstreamer_relay_rtp_packet (gpointer data, gpointer user_data);
static void * ps_gstreamer_relay_thread (void * data);
static gboolean ps_gstreamer_is_keyframe (gint codec, char * buffer, int len);

typedef enum ps_gstreamer_type {
	ps_gstreamer_type_none = 0,
	ps_gstreamer_type_live,
} ps_gstreamer_type;

typedef enum ps_gstreamer_source {
	ps_gstreamer_source_none = 0,
	ps_gstreamer_source_rtp,
} ps_gstreamer_source;

typedef struct ps_gstreamer_rtp_keyframe {
	gboolean enabled;
	GList * latest_keyframe;
	GList * temp_keyframe;
	guint32 temp_ts;
	ps_mutex mutex;
} ps_gstreamer_rtp_keyframe;

typedef struct ps_gstreamer_rtp_source {
	GstElement * vsource, * vfilter, * vencoder, * vrtppay, * vsink; 
	GstElement * asource, * afilter, * aencoder, * artppay, * asink; 
	GstElement * resample, * vconvert;
	GstElement * pipeline;
	GstBus * bus;
	GstCaps * afiltercaps, * vfiltercaps;
	gint64 last_received_video;
	gint64 last_received_audio;
	ps_gstreamer_rtp_keyframe keyframe;
} ps_gstreamer_rtp_source;

#define PS_GSTREAMER_VP8  0
#define PS_GSTREAMER_H264 1
#define PS_GSTREAMER_VP9  2
typedef struct ps_gstreamer_codecs {
	gint audio_pt;
	char * audio_rtpmap;
	char * audio_fmtp;
	gint video_codec;
	gint video_pt;
	char * video_rtpmap;
	char * video_fmtp;
} ps_gstreamer_codecs;

typedef struct ps_gstreamer_mountpoint {
	gint64 id;
	char * name;
	char * description;
	gboolean is_private;
	gboolean enabled;
	gboolean active;
	ps_gstreamer_type streamer_type;
	ps_gstreamer_source streamer_source;
	void * source;
	GDestroyNotify source_destroy;
	ps_gstreamer_codecs codecs;
	GList * listeners;
	gint64 destroyed;
	ps_mutex mutex;
} ps_gstreamer_mountpoint;
GHashTable * mountpoints;
static GList * old_mountpoints;
ps_mutex mountpoints_mutex;

static void ps_gstreamer_mountpoint_free (ps_gstreamer_mountpoint * mp);
ps_gstreamer_mountpoint * ps_gstreamer_create_rtp_source (
		uint64_t id, char *name, char *desc,
		gboolean doaudio, char * asrcname, uint8_t acodec, char *artpmap, char *afmtp,
		gboolean dovideo, char * vsrcname, uint8_t vcodec, char *vrtpmap, char *vfmtp, gboolean bufferkf);

typedef struct ps_gstreamer_message {
	ps_plugin_session * handle;
	char * transaction;
	json_t * message;
	char * sdp_type;
	char * sdp;
} ps_gstreamer_message;
static GAsyncQueue * messages = NULL;
static ps_gstreamer_message exit_message;

static void ps_gstreamer_message_free (ps_gstreamer_message * msg) {
	if (!msg || msg == &exit_message) return;
	msg->handle = NULL;
	g_free (msg->transaction);
	msg->transaction = NULL;
	if (msg->message) json_decref(msg->message);
	msg->message = NULL;
	g_free (msg->sdp_type);
	msg->sdp_type = NULL;
	g_free (msg->sdp);
	msg->sdp = NULL;
	
	g_free (msg);
}

typedef struct ps_gstreamer_context {
	/* Needed to fix seq and ts in case of stream switching */
	uint32_t a_last_ssrc, a_last_ts, a_base_ts, a_base_ts_prev,
			v_last_ssrc, v_last_ts, v_base_ts, v_base_ts_prev;
	uint16_t a_last_seq, a_base_seq, a_base_seq_prev,
			v_last_seq, v_base_seq, v_base_seq_prev;
} ps_gstreamer_context;

typedef struct ps_gstreamer_session {
	ps_plugin_session *handle;
	ps_gstreamer_mountpoint *mountpoint;
	gboolean started;
	gboolean paused;
	ps_gstreamer_context context;
	gboolean stopping;
	volatile gint hangingup;
	gint64 destroyed;	/* Time at which this session was marked as destroyed */
} ps_gstreamer_session;
static GHashTable * sessions;
static GList * old_sessions;
static ps_mutex sessions_mutex;

typedef struct ps_gstreamer_rtp_relay_packet {
	rtp_header * data;
	gint length;
	gint is_video;
	gint is_keyframe;
	uint32_t timestamp;
	uint16_t seq_number;
} ps_gstreamer_rtp_relay_packet;

/* Error codes */
#define PS_GSTREAMER_ERROR_NO_MESSAGE			450
#define PS_GSTREAMER_ERROR_INVALID_JSON			451
#define PS_GSTREAMER_ERROR_INVALID_REQUEST		452
#define PS_GSTREAMER_ERROR_MISSING_ELEMENT		453
#define PS_GSTREAMER_ERROR_INVALID_ELEMENT		454
#define PS_GSTREAMER_ERROR_NO_SUCH_MOUNTPOINT	455
#define PS_GSTREAMER_ERROR_CANT_CREATE			456
#define PS_GSTREAMER_ERROR_UNAUTHORIZED			457
#define PS_GSTREAMER_ERROR_CANT_SWITCH			458
#define PS_GSTREAMER_ERROR_UNKNOWN_ERROR		470

/* Streaming watchdog/garbage collector (sort of) */
void *ps_gstreamer_watchdog(void *data);
void *ps_gstreamer_watchdog(void *data) {
	PS_LOG (LOG_INFO, "Streaming watchdog started\n");
	gint64 now = 0;
	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		ps_mutex_lock(&sessions_mutex);
		/* Iterate on all the sessions */
		now = ps_get_monotonic_time();
		if(old_sessions != NULL) {
			GList *sl = old_sessions;
			PS_LOG(LOG_HUGE, "Checking %d old Streaming sessions...\n", g_list_length(old_sessions));
			while(sl) {
				ps_gstreamer_session *session = (ps_gstreamer_session *)sl->data;
				if(!session) {
					sl = sl->next;
					continue;
				}
				if(now-session->destroyed >= 5*G_USEC_PER_SEC) {
					/* We're lazy and actually get rid of the stuff only after a few seconds */
					PS_LOG(LOG_VERB, "Freeing old Streaming session\n");
					GList *rm = sl->next;
					old_sessions = g_list_delete_link(old_sessions, sl);
					sl = rm;
					session->handle = NULL;
					g_free(session);
					session = NULL;
					continue;
				}
				sl = sl->next;
			}
		}
		ps_mutex_unlock(&sessions_mutex);
		ps_mutex_lock(&mountpoints_mutex);
		// Iterate on all the mountpoints 
		if(old_mountpoints != NULL) {
			GList *sl = old_mountpoints;
			PS_LOG(LOG_HUGE, "Checking %d old Streaming mountpoints...\n", g_list_length(old_mountpoints));
			while(sl) {
				ps_gstreamer_mountpoint *mountpoint = (ps_gstreamer_mountpoint *)sl->data;
				if(!mountpoint) {
					sl = sl->next;
					continue;
				}
				if(now-mountpoint->destroyed >= 5*G_USEC_PER_SEC) {
					PS_LOG(LOG_VERB, "Freeing old Streaming mountpoint\n");
					GList *rm = sl->next;
					old_mountpoints = g_list_delete_link(old_mountpoints, sl);
					sl = rm;
					ps_gstreamer_mountpoint_free(mountpoint);
					mountpoint = NULL;
					continue;
				}
				sl = sl->next;
			}
		}
		ps_mutex_unlock(&mountpoints_mutex);
		g_usleep(500000);
	}
	PS_LOG(LOG_INFO, "Streaming watchdog stopped\n");
	return NULL;
}


int ps_gstreamer_init (ps_callbacks * callback, const char * config_path) {
	if (g_atomic_int_get(&stopping)) return -1;
	//if (callback == NULL || config_path == NULL) return -1;
	if (callback == NULL) return -1;
	
	gboolean is_private = FALSE, doaudio = TRUE, dovideo = TRUE, bufferkf = FALSE;
	uint64_t id = 100;
	char * name = "ps-gstreamer", * desc = "ps-gstreamer";
	char * amcast = NULL, * vmcast = NULL;
	char * asrcname = "audiotestsrc", * vsrcname = "videotestsrc";
	int acodec = 111, vcodec = 100;
	char * artpmap = "opus/48000/2", * vrtpmap = "VP8/90000";
	/* Create mountpoint here */
/*ps_gstreamer_mountpoint * ps_gstreamer_create_rtp_source (
		uint64_t id, char *name, char *desc,
		gboolean doaudio, char * asrcname, uint8_t acodec, char *artpmap, char *afmtp,
		gboolean dovideo, char * vsrcname, uint8_t vcodec, char *vrtpmap, char *vfmtp, gboolean bufferkf); */	
	
	mountpoints = g_hash_table_new (NULL, NULL);
	ps_mutex_init (&mountpoints_mutex);
	ps_gstreamer_mountpoint * mp = NULL;
	if ((mp = ps_gstreamer_create_rtp_source (id, name, desc, 
			  doaudio, asrcname, acodec, artpmap, NULL,
			  dovideo, vsrcname, vcodec, vrtpmap, NULL,
			  bufferkf)) == NULL) {
		PS_LOG (LOG_ERR, "Error creating gstreamer source '%s'\n", name);
	}
	mp->is_private = is_private;
	
	ps_mutex_lock (&mountpoints_mutex);
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init (&iter, mountpoints);
	while (g_hash_table_iter_next (&iter, NULL, &value)) {
		ps_gstreamer_mountpoint * mp = value;
		PS_LOG (LOG_VERB, " ::: [%"SCNu64"][%s] %s\n", mp->id, mp->name, mp->description);
	}
	ps_mutex_unlock (&mountpoints_mutex);
	
	sessions = g_hash_table_new (NULL, NULL);
	ps_mutex_init (&sessions_mutex);
	messages = g_async_queue_new_full ((GDestroyNotify) ps_gstreamer_message_free);
	
	gateway = callback;
	g_atomic_int_set (&initialized, 1);
	
	GError * error = NULL;
	watchdog = g_thread_try_new ("streaming watchdog", &ps_gstreamer_watchdog, NULL, &error);
	if (!watchdog) {
		g_atomic_int_set(&initialized, 0);
		PS_LOG (LOG_ERR, "Got error %d (%s) trying to launch the Streaming watchdog thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	handler_thread = g_thread_try_new ("streaming handler", &ps_gstreaming_handler, NULL, &error);
	if (!handler_thread) {
		g_atomic_int_set(&initialized, 0);
		PS_LOG (LOG_ERR, "Got error %d (%s) trying to launch the Streaming handler thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	PS_LOG (LOG_INFO, "%s initialized\n", PS_GSTREAMER_NAME);
	return 0;
}

void ps_gstreamer_destroy (void) {
	if (!g_atomic_int_get(&initialized)) return;
	g_atomic_int_set(&stopping, 1);
	
	g_async_queue_push (messages, &exit_message);
	
	if (handler_thread != NULL) {
		g_thread_join (handler_thread);
		handler_thread = NULL;
	}
	
	/* remove all mountpoints */
	ps_mutex_lock (&mountpoints_mutex);
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init (&iter, mountpoints);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		ps_gstreamer_mountpoint * mp = value;
		if (!mp->destroyed) {
			mp->destroyed = ps_get_monotonic_time();
			ps_gstreamer_rtp_source * source = mp->source;
			if (source == NULL) {
				PS_LOG (LOG_ERR, "[%s] Invalid RTP source mountpoint!\n", mp->name);
				continue;
			}
			gst_object_unref (source->bus);
			gst_element_set_state (source->pipeline, GST_STATE_NULL);
			if (gst_element_get_state (source->pipeline, NULL, NULL, GST_CLOCK_TIME_NONE) == GST_STATE_CHANGE_FAILURE) {
				PS_LOG (LOG_ERR,"Unable to stop pipeline\n");
			}
			gst_object_unref (GST_OBJECT (source->pipeline));
			ps_gstreamer_mountpoint_free(mp);
			mp = NULL;
			continue;
		}
	}
	ps_mutex_unlock (&mountpoints_mutex);
	if (watchdog != NULL) {
		g_thread_join (watchdog);
		watchdog = NULL;
	}
	usleep(500000);
	ps_mutex_lock(&mountpoints_mutex);
	g_hash_table_destroy(mountpoints);
	ps_mutex_unlock(&mountpoints_mutex);
	ps_mutex_lock(&sessions_mutex);
	g_hash_table_destroy(sessions);
	ps_mutex_unlock(&sessions_mutex);
	g_async_queue_unref(messages);
	messages = NULL;
	sessions = NULL;

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	PS_LOG(LOG_INFO, "%s destroyed!\n", PS_GSTREAMER_NAME);
	
}

int ps_gstreamer_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return PS_PLUGIN_API_VERSION;
}

int ps_gstreamer_get_version(void) {
	return PS_GSTREAMER_VERSION;
}

const char *ps_gstreamer_get_version_string(void) {
	return PS_GSTREAMER_VERSION_STRING;
}

const char *ps_gstreamer_get_description(void) {
	return PS_GSTREAMER_DESCRIPTION;
}

const char *ps_gstreamer_get_name(void) {
	return PS_GSTREAMER_NAME;
}

const char *ps_gstreamer_get_author(void) {
	return PS_GSTREAMER_AUTHOR;
}

const char *ps_gstreamer_get_package(void) {
	return PS_GSTREAMER_PACKAGE;
}

void ps_gstreamer_create_session (ps_plugin_session * handle, int * error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		PS_LOG (LOG_WARN, "plugin not initialized..\n");
		*error = -1;
		return;
	}	
	ps_gstreamer_session *session = (ps_gstreamer_session *)g_malloc0(sizeof(ps_gstreamer_session));
	if(session == NULL) {
		PS_LOG(LOG_FATAL, "Memory error!\n");
		*error = -2;
		return;
	}
	session->handle = handle;
	session->mountpoint = NULL;	/* This will happen later */
	session->started = FALSE;	/* This will happen later */
	session->paused = FALSE;
	session->destroyed = 0;
	g_atomic_int_set(&session->hangingup, 0);
	handle->plugin_handle = session;
	ps_mutex_lock(&sessions_mutex);
	g_hash_table_insert(sessions, handle, session);
	ps_mutex_unlock(&sessions_mutex);

	return;
}

void ps_gstreamer_destroy_session(ps_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}	
	ps_gstreamer_session *session = (ps_gstreamer_session *)handle->plugin_handle; 
	if(!session) {
		PS_LOG(LOG_ERR, "No session associated with this handle...\n");
		*error = -2;
		return;
	}
	PS_LOG(LOG_VERB, "Removing streaming session...\n");
	if(session->mountpoint) {
		ps_mutex_lock(&session->mountpoint->mutex);
		session->mountpoint->listeners = g_list_remove_all(session->mountpoint->listeners, session);
		ps_mutex_unlock(&session->mountpoint->mutex);
	}
	ps_mutex_lock(&sessions_mutex);
	if(!session->destroyed) {
		session->destroyed = ps_get_monotonic_time();
		g_hash_table_remove(sessions, handle);
		/* Cleaning up and removing the session is done in a lazy way */
		old_sessions = g_list_append(old_sessions, session);
	}
	ps_mutex_unlock(&sessions_mutex);
	return;
}

char *ps_gstreamer_query_session(ps_plugin_session *handle) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		return NULL;
	}	
	ps_gstreamer_session *session = (ps_gstreamer_session *)handle->plugin_handle;
	if(!session) {
		PS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return NULL;
	}
	/* What is this user watching, if anything? */
	json_t *info = json_object();
	json_object_set_new(info, "state", json_string(session->mountpoint ? "watching" : "idle"));
	if(session->mountpoint) {
		json_object_set_new(info, "mountpoint_id", json_integer(session->mountpoint->id));
		json_object_set_new(info, "mountpoint_name", session->mountpoint->name ? json_string(session->mountpoint->name) : NULL);
	}
	json_object_set_new(info, "destroyed", json_integer(session->destroyed));
	char *info_text = json_dumps(info, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
	json_decref(info);
	return info_text;
}

struct ps_plugin_result * ps_gstreamer_handle_message (ps_plugin_session * handle, char * transaction, char * message, char * sdp_type, char * sdp) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return ps_plugin_result_new(PS_PLUGIN_ERROR, g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized");
	
	int error_code = 0;
	char error_cause[512];
	json_t * root = NULL;
	json_t * response = NULL;
	
	if (message == NULL) {
		PS_LOG (LOG_ERR, "No message??\n");
		error_code = PS_GSTREAMER_ERROR_NO_MESSAGE;
		g_snprintf (error_cause, 512, "%s", "No message");
		goto error;
	}
	PS_LOG (LOG_VERB, "Handling message: %s\n", message);
	ps_gstreamer_session * session = (ps_gstreamer_session *) handle->plugin_handle;
	
	if(!session) {
		PS_LOG(LOG_ERR, "No session associated with this handle...\n");
		error_code = PS_GSTREAMER_ERROR_UNKNOWN_ERROR;
		g_snprintf(error_cause, 512, "%s", "session associated with this handle...");
		goto error;
	}
	if(session->destroyed) {
		PS_LOG(LOG_ERR, "Session has already been destroyed...\n");
		error_code = PS_GSTREAMER_ERROR_UNKNOWN_ERROR;
		g_snprintf(error_cause, 512, "%s", "Session has already been destroyed...");
		goto error;
	}
	json_error_t error;
	root = json_loads(message, 0, &error);
	if(!root) {
		PS_LOG(LOG_ERR, "JSON error: on line %d: %s\n", error.line, error.text);
		error_code = PS_GSTREAMER_ERROR_INVALID_JSON;
		g_snprintf(error_cause, 512, "JSON error: on line %d: %s", error.line, error.text);
		goto error;
	}
	if(!json_is_object(root)) {
		PS_LOG(LOG_ERR, "JSON error: not an object\n");
		error_code = PS_GSTREAMER_ERROR_INVALID_JSON;
		g_snprintf(error_cause, 512, "JSON error: not an object");
		goto error;
	}
	
	json_t * request = json_object_get (root, "request");
	if (!request) {
		PS_LOG (LOG_ERR, "Missing element (request)\n");
		error_code = PS_GSTREAMER_ERROR_MISSING_ELEMENT;
		g_snprintf (error_cause, 512, "Missing element (request)");
		goto error;
	}
	if (!json_is_string(request)) {
		PS_LOG (LOG_ERR, "Invalid element (request should be a string)\n");
		error_code = PS_GSTREAMER_ERROR_INVALID_ELEMENT;
		g_snprintf (error_cause, 512, "Invalid element (request should be a string)");
		goto error;
	}
	
	const char * request_text = json_string_value (request);
	if (!strcasecmp(request_text, "list")) {
		json_t * list = json_array();
		PS_LOG (LOG_VERB, "Request for list of mountpoints \n");
		ps_mutex_lock(&mountpoints_mutex);
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, mountpoints);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			ps_gstreamer_mountpoint *mp = value;
			if(mp->is_private) {
				/* Skip private stream */
				PS_LOG(LOG_VERB, "Skipping private mountpoint '%s'\n", mp->description);
				continue;
			}
			json_t *ml = json_object();
			json_object_set_new(ml, "id", json_integer(mp->id));
			json_object_set_new(ml, "description", json_string(mp->description));
			json_object_set_new(ml, "type", json_string(mp->streamer_type == ps_gstreamer_type_live ? "live" : "on demand"));
			if(mp->streamer_source == ps_gstreamer_source_rtp) {
				ps_gstreamer_rtp_source *source = mp->source;
				gint64 now = ps_get_monotonic_time();
				if(GST_STATE(source->pipeline) == GST_STATE_PLAYING) {
					json_object_set_new(ml, "audio_age_ms", json_integer((now - source->last_received_audio) / 1000));
					json_object_set_new(ml, "video_age_ms", json_integer((now - source->last_received_video) / 1000));
				}
			}
			json_array_append_new(list, ml);
		}
		ps_mutex_unlock(&mountpoints_mutex);
		/* Send info back */
		response = json_object();
		json_object_set_new(response, "streaming", json_string("list"));
		json_object_set_new(response, "list", list);
		goto plugin_response;
	} else if (!strcasecmp(request_text, "info")){
		PS_LOG(LOG_VERB, "Request info on a specific mountpoint\n");
		/* Return info on a specific mountpoint */
		json_t *id = json_object_get(root, "id");
		if(!id) {
			PS_LOG(LOG_ERR, "Missing element (id)\n");
			error_code = PS_GSTREAMER_ERROR_MISSING_ELEMENT;
			g_snprintf(error_cause, 512, "Missing element (id)");
			goto error;
		}
		if(!json_is_integer(id) || json_integer_value(id) < 0) {
			PS_LOG(LOG_ERR, "Invalid element (id should be a positive integer)\n");
			error_code = PS_GSTREAMER_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Invalid element (id should be a positive integer)");
			goto error;
		}
		gint64 id_value = json_integer_value(id);
		ps_mutex_lock(&mountpoints_mutex);
		ps_gstreamer_mountpoint *mp = g_hash_table_lookup(mountpoints, GINT_TO_POINTER(id_value));
		if(mp == NULL) {
			ps_mutex_unlock(&mountpoints_mutex);
			PS_LOG(LOG_VERB, "No such mountpoint/stream %"SCNu64"\n", id_value);
			error_code = PS_GSTREAMER_ERROR_NO_SUCH_MOUNTPOINT;
			g_snprintf(error_cause, 512, "No such mountpoint/stream %"SCNu64"", id_value);
			goto error;
		}
		json_t *ml = json_object();
		json_object_set_new(ml, "id", json_integer(mp->id));
		json_object_set_new(ml, "description", json_string(mp->description));
		json_object_set_new(ml, "type", json_string(mp->streamer_type == ps_gstreamer_type_live ? "live" : "on demand"));
		if(mp->streamer_source == ps_gstreamer_source_rtp) {
			ps_gstreamer_rtp_source *source = mp->source;
			gint64 now = ps_get_monotonic_time();
			if(GST_STATE(source->pipeline) == GST_STATE_PLAYING) {
				json_object_set_new(ml, "audio_age_ms", json_integer((now - source->last_received_audio) / 1000));
				json_object_set_new(ml, "video_age_ms", json_integer((now - source->last_received_video) / 1000));
			}
		}
		ps_mutex_unlock(&mountpoints_mutex);
		/* Send info back */
		response = json_object();
		json_object_set_new(response, "streaming", json_string("info"));
		json_object_set_new(response, "info", ml);
		goto plugin_response;		
	} else if (!strcasecmp(request_text, "create")) {
		json_t * type = json_object_get (root, "type");
		if (!type) {
			PS_LOG (LOG_ERR, "Missing element (type)\n");
			error_code = PS_GSTREAMER_ERROR_MISSING_ELEMENT;
			g_snprintf (error_cause, 512, "Missing element (type)");
			goto error;
		}
		if (!json_is_string(type)) {
			PS_LOG (LOG_ERR, "Invalid element (type should be a string)\n");
			error_code = PS_GSTREAMER_ERROR_INVALID_ELEMENT;
			g_snprintf (error_cause, 512, "Invalid element (type should be a string)");
			goto error;
		}
		const char * type_text = json_string_value(type);
		//ps_gstreamer_mountpoint * mp = NULL;
		if (!strcasecmp(type_text,"rtp")) {
		}
	} else if (!strcasecmp(request_text,"watch") || !strcasecmp(request_text, "start") || !strcasecmp(request_text,"pause") || !strcasecmp(request_text,"stop")) {
		ps_gstreamer_message * msg = g_malloc0(sizeof(ps_gstreamer_message));
		if (msg==NULL) {
			PS_LOG (LOG_FATAL, "Memory Error!\n");
			error_code = PS_GSTREAMER_ERROR_UNKNOWN_ERROR;
			g_snprintf (error_cause, 512, "Memory Error");
			goto error;
		}
		g_free (message);
		msg->handle = handle;
		msg->transaction = transaction;
		msg->message = root;
		msg->sdp_type = sdp_type;
		msg->sdp = sdp;
		
		g_async_queue_push (messages, msg);
		
		return ps_plugin_result_new (PS_PLUGIN_OK_WAIT, NULL);
	} else {
		PS_LOG (LOG_VERB, "Unknown request '%s'\n", request_text);
		error_code = PS_GSTREAMER_ERROR_INVALID_REQUEST;
		g_snprintf (error_cause, 512, "Unknown request '%s'",request_text);
		goto error;
	}

plugin_response:
		{
			if(!response) {
				error_code = PS_GSTREAMER_ERROR_UNKNOWN_ERROR;
				g_snprintf(error_cause, 512, "Invalid response");
				goto error;
			}
			if(root != NULL)
				json_decref(root);
			g_free(transaction);
			g_free(message);
			g_free(sdp_type);
			g_free(sdp);

			char *response_text = json_dumps(response, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
			json_decref(response);
			ps_plugin_result *result = ps_plugin_result_new(PS_PLUGIN_OK, response_text);
			g_free(response_text);
			return result;
		}
		
error:
	{
		if (root != NULL) json_decref(root);
		g_free(transaction);
		g_free(message);
		g_free(sdp_type);
		g_free(sdp);
		
		json_t * event = json_object();
		json_object_set_new(event, "streaming", json_string("event"));
		json_object_set_new(event, "error_code", json_integer(error_code));
		json_object_set_new(event, "error", json_string(error_cause));
		char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
		json_decref(event);
		ps_plugin_result *result = ps_plugin_result_new(PS_PLUGIN_OK, event_text);
		g_free(event_text);
		return result;
	}
	
}

void ps_gstreamer_setup_media (ps_plugin_session * handle) {
	PS_LOG (LOG_INFO, "WebRTC media is not available\n");
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) return;
	
	ps_gstreamer_session * session = (ps_gstreamer_session *)handle->plugin_handle;
	if (!session) {
		PS_LOG(LOG_ERR, "No session associated with this handle!\n");
		return;
	}
	if(session->destroyed)
		return;
	g_atomic_int_set(&session->hangingup, 0);
	/* We only start streaming towards this user when we get this event */
	session->context.a_last_ssrc = 0;
	session->context.a_last_ssrc = 0;
	session->context.a_last_ts = 0;
	session->context.a_base_ts = 0;
	session->context.a_base_ts_prev = 0;
	session->context.v_last_ssrc = 0;
	session->context.v_last_ts = 0;
	session->context.v_base_ts = 0;
	session->context.v_base_ts_prev = 0;
	session->context.a_last_seq = 0;
	session->context.a_base_seq = 0;
	session->context.a_base_seq_prev = 0;
	session->context.v_last_seq = 0;
	session->context.v_base_seq = 0;
	session->context.v_base_seq_prev = 0;
	
	ps_gstreamer_mountpoint * mountpoint = session->mountpoint;
	if(mountpoint->streamer_source == ps_gstreamer_source_rtp) {
		ps_gstreamer_rtp_source *source = mountpoint->source;
		if(source->keyframe.enabled) {
			PS_LOG(LOG_HUGE, "Any keyframe to send?\n");
			ps_mutex_lock(&source->keyframe.mutex);
			if(source->keyframe.latest_keyframe != NULL) {
				PS_LOG(LOG_HUGE, "Yep! %d packets\n", g_list_length(source->keyframe.latest_keyframe));
				GList *temp = source->keyframe.latest_keyframe;
				while(temp) {
					ps_gstreamer_relay_rtp_packet(session, temp->data);
					temp = temp->next;
				}
			}
			ps_mutex_unlock(&source->keyframe.mutex);
		}
	}
	session->started = TRUE;
	/* Prepare JSON event */
	json_t *event = json_object();
	json_object_set_new(event, "streaming", json_string("event"));
	json_t *result = json_object();
	json_object_set_new(result, "status", json_string("started"));
	json_object_set_new(event, "result", result);
	char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
	json_decref(event);
	PS_LOG(LOG_VERB, "Pushing event: %s\n", event_text);
	int ret = gateway->push_event(handle, &ps_gstreamer_plugin, NULL, event_text, NULL, NULL);
	PS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
	g_free(event_text);
}

void ps_gstreamer_incoming_rtp (ps_plugin_session * handle, int video, char * buf, int len) {
	if (handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	if (gateway) {
		ps_gstreamer_session * session = (ps_gstreamer_session *)handle->plugin_handle;
		if(!session) {
			PS_LOG(LOG_ERR, "No session associated with this handle\n");
			return;
		}
		if (session->destroyed) return;
		PS_LOG(LOG_VERB, "Received %s packet of length %d bytes\n", video ? "video" : "audio", len);
		return;
	}
}

void ps_gstreamer_incoming_rtcp (ps_plugin_session * handle, int video, char * buf, int len) {
	if (handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
		
	uint64_t bw = janus_rtcp_get_remb (buf, len);
	if (bw > 0) {
		PS_LOG (LOG_HUGE, "REMB for this PeerConnection: %"SCNu64"\n", bw);
	}
}

void ps_gstreamer_incoming_data (ps_plugin_session * handle, char * buf, int len) {
	return;
}

void ps_gstreamer_hangup_media (ps_plugin_session * handle) {
	PS_LOG(LOG_INFO, "No WebRTC media anymore\n");
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	ps_gstreamer_session *session = (ps_gstreamer_session *)handle->plugin_handle;	
	if(!session) {
		PS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(session->destroyed)
		return;
	if(g_atomic_int_add(&session->hangingup, 1))
		return;
	/* FIXME Simulate a "stop" coming from the browser */
	ps_gstreamer_message *msg = g_malloc0(sizeof(ps_gstreamer_message));
	msg->handle = handle;
	msg->message = json_loads("{\"request\":\"stop\"}", 0, NULL);
	msg->transaction = NULL;
	msg->sdp_type = NULL;
	msg->sdp = NULL;
	g_async_queue_push(messages, msg);
}

static void * ps_gstreaming_handler (void * data) {
	PS_LOG (LOG_VERB, "Joining streaming handler thread..\n");
	ps_gstreamer_message * msg = NULL;
	int error_code = 0;
	char * error_cause = g_malloc0(1024);
	if (error_cause == NULL) {
		PS_LOG(LOG_FATAL,"Memory error!\n");
		return NULL;
	}
	
	json_t * root = NULL;
	while (g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		msg = g_async_queue_pop(messages);
		if (msg == NULL) continue;
		if (msg == &exit_message) break;
		if (msg->handle == NULL) {
			ps_gstreamer_message_free(msg);
			continue;
		}
		ps_gstreamer_session * session = NULL;
		ps_mutex_lock (&sessions_mutex);
		if (g_hash_table_lookup(sessions, msg->handle) != NULL) {
			session = (ps_gstreamer_session *)msg->handle->plugin_handle;
		}
		ps_mutex_unlock (&sessions_mutex);
		if (!session) {
			PS_LOG (LOG_ERR, "no session associated with this handle...\n");
			ps_gstreamer_message_free (msg);
			continue;
		}
		if (session->destroyed) {
			ps_gstreamer_message_free (msg);
			continue;
		}
		
		error_code = 0;
		root = NULL;
		if (msg->message == NULL) {
			PS_LOG (LOG_ERR, "No message??\n");
			error_code = PS_GSTREAMER_ERROR_NO_MESSAGE;
			g_snprintf(error_cause, 512, "%s","No Message??");
			goto error;
		}
		root = msg->message;
		json_t * request = json_object_get(root, "request");
		if (!request) {
			PS_LOG (LOG_ERR, "Missing element (request)\n");
			error_code = PS_GSTREAMER_ERROR_MISSING_ELEMENT;
			g_snprintf (error_cause, 512, "Missing element (request)");
			goto error;
		}
		if (!json_is_string(request)) {
			PS_LOG (LOG_ERR, "Invalid element (request must be a string)\n");
			error_code = PS_GSTREAMER_ERROR_MISSING_ELEMENT;
			g_snprintf (error_cause, 512, "Invalid element (request must be string)");
			goto error;
		}
		const char * request_text = json_string_value (request);
		json_t * result = NULL;
		const char * sdp_type = NULL;
		char * sdp = NULL;
		
		if (!strcasecmp(request_text, "watch")) {
			json_t * id = json_object_get (root, "id");
			if (!id) {
				PS_LOG (LOG_ERR, "missing element (id)\n");
				error_code = PS_GSTREAMER_ERROR_MISSING_ELEMENT;
				g_snprintf(error_cause, 512, "Missing element (id)");
				goto error;
			}
			if (!json_is_integer(id)) {
				PS_LOG (LOG_ERR, "invalid element (id should be a positive integer)\n");
				error_code = PS_GSTREAMER_ERROR_MISSING_ELEMENT;
				g_snprintf(error_cause, 512, "invalid element (id should be a positive integer)");
				goto error;
			}
			gint64 id_value = json_integer_value(id);
			ps_mutex_lock (&mountpoints_mutex);
			ps_gstreamer_mountpoint * mp = g_hash_table_lookup (mountpoints, GINT_TO_POINTER(id_value));
			if (mp==NULL) {
				ps_mutex_unlock (&mountpoints_mutex);
				PS_LOG (LOG_VERB, "No such mountpoint/stream %"SCNu64"\n", id_value);
				error_code = PS_GSTREAMER_ERROR_NO_SUCH_MOUNTPOINT;
				g_snprintf (error_cause, 512, "No such mountpoint/stream %"SCNu64"", id_value);
				goto error;
			}
			ps_mutex_unlock (&mountpoints_mutex);
			PS_LOG (LOG_VERB, "Request to watch mountpoint/stream %"SCNu64"\n", id_value);
			session->stopping = FALSE;
			session->mountpoint = mp;
			/* TODO check if user is already watching a stream, if the video is active */
			ps_mutex_lock (&mp->mutex);
			mp->listeners = g_list_append(mp->listeners, session);
			ps_mutex_unlock (&mp->mutex);
			sdp_type = "offer";	/* We're always going to do the offer ourselves, never answer */
			char sdptemp[2048];
			memset(sdptemp, 0, 2048);
			gchar buffer[512];
			memset(buffer, 0, 512);
			gint64 sessid = ps_get_real_time();
			gint64 version = sessid;	/* FIXME This needs to be increased when it changes, so time should be ok */
			g_snprintf(buffer, 512,
				"v=0\r\no=%s %"SCNu64" %"SCNu64" IN IP4 127.0.0.1\r\n",
					"-", sessid, version);
			g_strlcat(sdptemp, buffer, 2048);
			g_strlcat(sdptemp, "s=Streaming Test\r\nt=0 0\r\n", 2048);
			if(mp->codecs.audio_pt >= 0) {
				/* Add audio line */
				g_snprintf(buffer, 512,
					"m=audio 1 RTP/SAVPF %d\r\n"
					"c=IN IP4 1.1.1.1\r\n",
					mp->codecs.audio_pt);
				g_strlcat(sdptemp, buffer, 2048);
				if(mp->codecs.audio_rtpmap) {
					g_snprintf(buffer, 512,
						"a=rtpmap:%d %s\r\n",
						mp->codecs.audio_pt, mp->codecs.audio_rtpmap);
					g_strlcat(sdptemp, buffer, 2048);
				}
				if(mp->codecs.audio_fmtp) {
					g_snprintf(buffer, 512,
						"a=fmtp:%d %s\r\n",
						mp->codecs.audio_pt, mp->codecs.audio_fmtp);
					g_strlcat(sdptemp, buffer, 2048);
				}
				g_strlcat(sdptemp, "a=sendonly\r\n", 2048);
			}
			if(mp->codecs.video_pt >= 0) {
				/* Add video line */
				g_snprintf(buffer, 512,
					"m=video 1 RTP/SAVPF %d\r\n"
					"c=IN IP4 1.1.1.1\r\n",
					mp->codecs.video_pt);
				g_strlcat(sdptemp, buffer, 2048);
				if(mp->codecs.video_rtpmap) {
					g_snprintf(buffer, 512,
						"a=rtpmap:%d %s\r\n",
						mp->codecs.video_pt, mp->codecs.video_rtpmap);
					g_strlcat(sdptemp, buffer, 2048);
				}
				if(mp->codecs.video_fmtp) {
					g_snprintf(buffer, 512,
						"a=fmtp:%d %s\r\n",
						mp->codecs.video_pt, mp->codecs.video_fmtp);
					g_strlcat(sdptemp, buffer, 2048);
				}
				g_snprintf(buffer, 512,
					"a=rtcp-fb:%d nack\r\n",
					mp->codecs.video_pt);
				g_strlcat(sdptemp, buffer, 2048);
				g_snprintf(buffer, 512,
					"a=rtcp-fb:%d goog-remb\r\n",
					mp->codecs.video_pt);
				g_strlcat(sdptemp, buffer, 2048);
				g_strlcat(sdptemp, "a=sendonly\r\n", 2048);
			}
			sdp = g_strdup(sdptemp);
			PS_LOG(LOG_VERB, "Going to offer this SDP:\n%s\n", sdp);
			result = json_object();
			json_object_set_new(result, "status", json_string("preparing"));
		} else if (!strcasecmp(request_text,"start")) {
			if(session->mountpoint == NULL) {
				PS_LOG(LOG_VERB, "Can't start: no mountpoint set\n");
				error_code = PS_GSTREAMER_ERROR_NO_SUCH_MOUNTPOINT;
				g_snprintf(error_cause, 512, "Can't start: no mountpoint set");
				goto error;
			}
			PS_LOG(LOG_VERB, "Starting the streaming\n");
			session->paused = FALSE;
			result = json_object();
			/* We wait for the setup_media event to start: on the other hand, it may have already arrived */
			json_object_set_new(result, "status", json_string(session->started ? "started" : "starting"));
		} else if (!strcasecmp(request_text,"pause")) {
			if (session->mountpoint == NULL) {
				PS_LOG(LOG_VERB, "Can't pause: no mountpoint set\n");
				error_code = PS_GSTREAMER_ERROR_NO_SUCH_MOUNTPOINT;
				g_snprintf(error_cause, 512, "Can't pause: no mountpoint set");
				goto error;
			}
			PS_LOG (LOG_VERB, "Pausing the streaming\n");
			session->paused = TRUE;
			result = json_object();
			json_object_set_new(result, "status", json_string("pausing"));
		} else if (!strcasecmp(request_text,"stop")) {
			if(session->stopping || !session->started) {
				/* Been there, done that: ignore */
				ps_gstreamer_message_free(msg);
				continue;
			}
			PS_LOG(LOG_VERB, "Stopping the streaming\n");
			session->stopping = TRUE;
			session->started = FALSE;
			session->paused = FALSE;
			result = json_object();
			json_object_set_new(result, "status", json_string("stopping"));
			if(session->mountpoint) {
				ps_mutex_lock(&session->mountpoint->mutex);
				PS_LOG(LOG_VERB, "  -- Removing the session from the mountpoint listeners\n");
				if(g_list_find(session->mountpoint->listeners, session) != NULL) {
					PS_LOG(LOG_VERB, "  -- -- Found!\n");
				}
				session->mountpoint->listeners = g_list_remove_all(session->mountpoint->listeners, session);
				ps_mutex_unlock(&session->mountpoint->mutex);
			}
			session->mountpoint = NULL;
			/* Tell the core to tear down the PeerConnection, hangup_media will do the rest */
			gateway->close_pc(session->handle);
		} else {
			PS_LOG(LOG_VERB, "Unknown request '%s'\n", request_text);
			error_code = PS_GSTREAMER_ERROR_INVALID_REQUEST;
			g_snprintf(error_cause, 512, "Unknown request '%s'", request_text);
			goto error;
		}
		
		/* Any SDP to handle? */
		if (msg->sdp) {
			PS_LOG (LOG_VERB, "This is involving negotiation (%s) as well: \n%s\n", msg->sdp_type, msg->sdp);
		}
		
		/* Prepare JSON event */
		json_t * event = json_object();
		json_object_set_new (event, "streaming", json_string("event"));
		if (result != NULL) json_object_set_new (event, "result", result);
		char * event_text = json_dumps (event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
		json_decref (event);
		PS_LOG (LOG_VERB, "Pushing event: %s\n",event_text);
		int ret = gateway->push_event(msg->handle, &ps_gstreamer_plugin, msg->transaction, event_text, sdp_type, sdp);
		PS_LOG (LOG_VERB, " >> %d (%s) \n", ret, janus_get_api_error(ret));
		g_free (event_text);
		if (sdp) g_free(sdp);
		ps_gstreamer_message_free (msg);
		continue;
		
error:
		{
			/* Prepare JSON error event */
			json_t *event = json_object();
			json_object_set_new(event, "streaming", json_string("event"));
			json_object_set_new(event, "error_code", json_integer(error_code));
			json_object_set_new(event, "error", json_string(error_cause));
			char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
			json_decref(event);
			PS_LOG(LOG_VERB, "Pushing event: %s\n", event_text);
			int ret = gateway->push_event(msg->handle, &ps_gstreamer_plugin, msg->transaction, event_text, NULL, NULL);
			PS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
			g_free(event_text);
			ps_gstreamer_message_free(msg);
			
		}
		
	}
	g_free(error_cause);
	PS_LOG (LOG_VERB, "Leaving streaming handler thread..\n");
	return NULL;
}

static void ps_gstreamer_rtp_source_free (ps_gstreamer_rtp_source * source) {
	
	ps_mutex_lock (&source->keyframe.mutex);
	GList * temp = NULL;
	while (source->keyframe.latest_keyframe) {
		temp = g_list_first (source->keyframe.latest_keyframe);
		source->keyframe.latest_keyframe = g_list_remove_link (source->keyframe.latest_keyframe, temp);
		ps_gstreamer_rtp_relay_packet * pkt = (ps_gstreamer_rtp_relay_packet *) temp->data;
		g_free(pkt->data);
		g_free(pkt);
		g_list_free(temp);
	}
	source->keyframe.latest_keyframe = NULL;
	while (source->keyframe.temp_keyframe) {
		temp = g_list_first (source->keyframe.temp_keyframe);
		source->keyframe.temp_keyframe = g_list_remove_link (source->keyframe.temp_keyframe, temp);
		ps_gstreamer_rtp_relay_packet * pkt = (ps_gstreamer_rtp_relay_packet *) temp->data;
		g_free(pkt->data);
		g_free(pkt);
		g_list_free(temp);
	}
	source->keyframe.temp_keyframe = NULL;
	ps_mutex_unlock (&source->keyframe.mutex);
	g_free (source);
}

static void ps_gstreamer_mountpoint_free (ps_gstreamer_mountpoint * mp) {
	mp->destroyed = ps_get_monotonic_time();
	
	g_free(mp->name);
	g_free(mp->description);
	ps_mutex_lock(&mp->mutex);
	g_list_free(mp->listeners);
	ps_mutex_unlock(&mp->mutex);

	if(mp->source != NULL && mp->source_destroy != NULL) {
		mp->source_destroy(mp->source);
	}

	g_free(mp->codecs.audio_rtpmap);
	g_free(mp->codecs.audio_fmtp);
	g_free(mp->codecs.video_rtpmap);
	g_free(mp->codecs.video_fmtp);

	g_free(mp);
}

ps_gstreamer_mountpoint * ps_gstreamer_create_rtp_source (
		uint64_t id, char *name, char *desc,
		gboolean doaudio, char * asrcname, uint8_t acodec, char *artpmap, char *afmtp,
		gboolean dovideo, char * vsrcname, uint8_t vcodec, char *vrtpmap, char *vfmtp, gboolean bufferkf) {
	ps_mutex_lock (&mountpoints_mutex);
	ps_gstreamer_mountpoint * live_rtp = g_malloc0 (sizeof(ps_gstreamer_mountpoint));
	if (live_rtp == NULL) {
		PS_LOG (LOG_FATAL, "Memory error\n");
		ps_mutex_unlock (&mountpoints_mutex);
		return NULL;
	}
	live_rtp->id = id;
	live_rtp->name = g_strdup (name);
	live_rtp->description = g_strdup (name);
	live_rtp->enabled = TRUE;
	live_rtp->active = FALSE;
	live_rtp->streamer_type = ps_gstreamer_type_live;
	live_rtp->streamer_source = ps_gstreamer_source_rtp;
	ps_gstreamer_rtp_source * live_rtp_source = g_malloc0 (sizeof(ps_gstreamer_rtp_source));
	if (live_rtp->name == NULL || live_rtp->description == NULL || live_rtp_source == NULL) {
		PS_LOG (LOG_FATAL, "Memory error\n");
		if (live_rtp->name) g_free(live_rtp->name);
		if (live_rtp->description) g_free(live_rtp->description);
		if (live_rtp_source) g_free(live_rtp_source);
		g_free (live_rtp);
		ps_mutex_unlock (&mountpoints_mutex);
		return NULL;
	}
	/* initialize gstreamer */
	gst_init (NULL, NULL);
	live_rtp_source->asource = gst_element_factory_make(asrcname, "asource");
	live_rtp_source->resample = gst_element_factory_make("audioresample", "resample");
	live_rtp_source->afilter = gst_element_factory_make("capsfilter", "afilter");
	live_rtp_source->aencoder = gst_element_factory_make("opusenc", "aencoder");
	live_rtp_source->artppay = gst_element_factory_make("rtpopuspay", "artppay");
	live_rtp_source->asink = gst_element_factory_make("appsink", "asink");
	if (dovideo) {
		live_rtp_source->vsource = gst_element_factory_make(vsrcname, "vsource");
		if (!strcasecmp(vsrcname,"videotestsrc")){
			g_object_set (live_rtp_source->vsource, "pattern", 1, NULL);
		}
		live_rtp_source->vfilter = gst_element_factory_make("capsfilter", "vfilter");
		live_rtp_source->vconvert = gst_element_factory_make ("videoconvert", "vconvert");
		if (strstr(vrtpmap, "vp8") || strstr(vrtpmap, "VP8")) {
			live_rtp_source->vencoder = gst_element_factory_make("vp8enc", "vencoder");
			live_rtp_source->vrtppay = gst_element_factory_make("rtpvp8pay", "vrtppay");
		} else if (strstr(vrtpmap, "h264") || strstr(vrtpmap, "H264")) {
			live_rtp_source->vencoder = gst_element_factory_make("x264enc", "vencoder");
			live_rtp_source->vrtppay = gst_element_factory_make("rtph264pay", "vrtppay");
		}
		live_rtp_source->vsink = gst_element_factory_make("appsink","vsink");
	}
	live_rtp_source->afiltercaps = gst_caps_new_simple ("audio/x-raw",
									"channels", G_TYPE_INT, 1,
									"rate", G_TYPE_INT, 16000,
									NULL);
	live_rtp_source->vfiltercaps = gst_caps_new_simple ("video/x-raw",
									"format", G_TYPE_STRING, "RGB",
									"width", G_TYPE_INT, 320,
									"height", G_TYPE_INT, 240,
									"framerate", GST_TYPE_FRACTION, 30, 1,
									NULL);
	g_object_set (live_rtp_source->afilter, "caps", live_rtp_source->afiltercaps, NULL);
	g_object_set (live_rtp_source->vfilter, "caps", live_rtp_source->vfiltercaps, NULL);
	
	g_object_set (live_rtp_source->asink, "max-buffers", 50, NULL);
	g_object_set (live_rtp_source->vsink, "max-buffers", 50, NULL);
	g_object_set (live_rtp_source->asink, "drop", TRUE, NULL);
	g_object_set (live_rtp_source->vsink, "drop", TRUE, NULL);
	
	gst_caps_unref (live_rtp_source->afiltercaps);
	gst_caps_unref (live_rtp_source->vfiltercaps);
	
	
	live_rtp_source->pipeline = gst_pipeline_new ("pipeline");
	gst_bin_add_many (GST_BIN (live_rtp_source->pipeline), live_rtp_source->asource,
		live_rtp_source->resample,
		live_rtp_source->afilter, live_rtp_source->aencoder, live_rtp_source->artppay,
		live_rtp_source->asink, live_rtp_source->vsource, live_rtp_source->vfilter,
		live_rtp_source->vconvert,
		live_rtp_source->vencoder, live_rtp_source->vrtppay, live_rtp_source->vsink, NULL);
	if ((gst_element_link_many (live_rtp_source->asource, live_rtp_source->resample, live_rtp_source->afilter, live_rtp_source->aencoder, live_rtp_source->artppay, live_rtp_source->asink, NULL) != TRUE ) 
		|| (gst_element_link_many (live_rtp_source->vsource, live_rtp_source->vfilter, live_rtp_source->vconvert, live_rtp_source->vencoder, live_rtp_source->vrtppay, live_rtp_source->vsink, NULL) != TRUE)) 
	{
		PS_LOG (LOG_ERR, "Failed to link GStreamer elements\n");
		if (live_rtp->name) g_free(live_rtp->name);
		if (live_rtp->description) g_free(live_rtp->description);
		gst_object_unref (GST_OBJECT (live_rtp_source->pipeline));
		if (live_rtp_source) g_free(live_rtp_source);
		g_free (live_rtp);
		ps_mutex_unlock (&mountpoints_mutex);
		return NULL;
	}
	live_rtp_source->bus = gst_pipeline_get_bus (GST_PIPELINE (live_rtp_source->pipeline));
	/*gst_bus_add_signal_watch (live_rtp_source->bus);
	g_signal_connect (live_rtp_source->bus, "message::error", G_CALLBACK*/
	
	live_rtp_source->last_received_audio = ps_get_monotonic_time();
	live_rtp_source->last_received_video = ps_get_monotonic_time();
	live_rtp_source->keyframe.enabled = bufferkf;
	live_rtp_source->keyframe.latest_keyframe = NULL;
	live_rtp_source->keyframe.temp_keyframe = NULL;
	live_rtp_source->keyframe.temp_ts = 0;
	ps_mutex_init (&live_rtp_source->keyframe.mutex);
	live_rtp->source = live_rtp_source;
	live_rtp->source_destroy = (GDestroyNotify) ps_gstreamer_rtp_source_free;
	live_rtp->codecs.audio_pt = doaudio ? acodec : -1;
	live_rtp->codecs.audio_rtpmap = doaudio ? g_strdup (artpmap) : NULL;
	live_rtp->codecs.audio_fmtp = doaudio ? (afmtp ? g_strdup (artpmap) : NULL) : NULL;
	live_rtp->codecs.video_codec = -1;
	if(dovideo) {
		if(strstr(vrtpmap, "vp8") || strstr(vrtpmap, "VP8"))
			live_rtp->codecs.video_codec = PS_GSTREAMER_VP8;
		else if(strstr(vrtpmap, "vp9") || strstr(vrtpmap, "VP9"))
			live_rtp->codecs.video_codec = PS_GSTREAMER_VP9;
		else if(strstr(vrtpmap, "h264") || strstr(vrtpmap, "H264"))
			live_rtp->codecs.video_codec = PS_GSTREAMER_H264;
	}
	live_rtp->codecs.video_pt = dovideo ? vcodec : -1;
	live_rtp->codecs.video_rtpmap = dovideo ? g_strdup(vrtpmap) : NULL;
	live_rtp->codecs.video_fmtp = dovideo ? (vfmtp ? g_strdup(vfmtp) : NULL) : NULL;
	live_rtp->listeners = NULL;
	live_rtp->destroyed = 0;
	ps_mutex_init (&live_rtp->mutex);
	g_hash_table_insert (mountpoints, GINT_TO_POINTER (live_rtp->id), live_rtp);
	ps_mutex_unlock (&mountpoints_mutex);
	GError * error = NULL;
	g_thread_try_new (live_rtp->name, &ps_gstreamer_relay_thread, live_rtp, &error);
	if (error != NULL) {
		PS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the RTP thread...\n", error->code, error->message ? error->message : "??");
		if(live_rtp->name)
			g_free(live_rtp->name);
		if(live_rtp->description)
			g_free(live_rtp->description);
		if(live_rtp_source)
			g_free(live_rtp_source);
		g_free(live_rtp);
		return NULL;
	}
	return live_rtp;
}

static void * ps_gstreamer_relay_thread (void * data) {
	PS_LOG (LOG_VERB, "Starting streaming relay thread\n");
	ps_gstreamer_mountpoint * mountpoint = (ps_gstreamer_mountpoint *)data;
	if (!mountpoint) {
		PS_LOG (LOG_ERR, "Invalid mounpoint!\n");
		g_thread_unref (g_thread_self());
		return NULL;
	}
	if (mountpoint->streamer_source != ps_gstreamer_source_rtp) {
		PS_LOG (LOG_ERR, "[%s] Not an RTP source mountpoint!\n", mountpoint->name);
		g_thread_unref (g_thread_self());
		return NULL;
	}
	ps_gstreamer_rtp_source * source = mountpoint->source;
	if (source == NULL) {
		PS_LOG (LOG_ERR, "[%s] Invalid RTP source mountpoint!\n", mountpoint->name);
		g_thread_unref (g_thread_self());
		return NULL;
	}
	char * name = g_strdup (mountpoint->name ? mountpoint->name : "??");
	/* Needed to fix seq and ts */
	uint32_t a_last_ssrc = 0, a_last_ts = 0, a_base_ts = 0, a_base_ts_prev = 0,
			v_last_ssrc = 0, v_last_ts = 0, v_base_ts = 0, v_base_ts_prev = 0;
	uint16_t a_last_seq = 0, a_base_seq = 0, a_base_seq_prev = 0,
			v_last_seq = 0, v_base_seq = 0, v_base_seq_prev = 0;
	
	/* Loop */
	int bytes = 0;
	gst_element_set_state (source->pipeline, GST_STATE_PLAYING);
	if (gst_element_get_state (source->pipeline, NULL, NULL, GST_CLOCK_TIME_NONE) == GST_STATE_CHANGE_FAILURE) {
		PS_LOG (LOG_FATAL,"Unable to play pipeline\n");
		mountpoint->enabled = FALSE;
		g_thread_unref (g_thread_self());
		return NULL;
	}
	
	GstSample * asample, * vsample;
	GstBuffer * abuffer, * vbuffer;
	gpointer aframedata, vframedata;
	gsize afsize, vfsize;
	/*char atempbuffer[1500], vtempbuffer[1500];
	memset(atempbuffer, 0, 1500);
	memset(vtempbuffer, 0, 1500);*/
	char * atempbuffer, * vtempbuffer;
	/* FIXME: memcpy the aframedata & vframedata, and free it locally */
	ps_gstreamer_rtp_relay_packet apacket;
	ps_gstreamer_rtp_relay_packet vpacket;
	while (!g_atomic_int_get (&stopping) && !mountpoint->destroyed) {
		asample = gst_app_sink_pull_sample (GST_APP_SINK (source->asink));
		if (asample != NULL) {
			if (mountpoint->active == FALSE) mountpoint->active = TRUE;
			source->last_received_audio = ps_get_monotonic_time();
			abuffer = gst_sample_get_buffer (asample);
			gst_buffer_extract_dup (abuffer, 0, -1, &aframedata, &afsize);
			
			atempbuffer = (char *)g_malloc0(afsize);
			memcpy(atempbuffer, aframedata, afsize);
			g_free (aframedata);
			
			bytes = afsize; //gst_buffer_get_size (abuffer);
			gst_sample_unref (asample);
			
			if (!mountpoint->enabled) {
				continue;
			}
			rtp_header * artp = (rtp_header *) atempbuffer;
			apacket.data = artp;
			apacket.length = bytes;
			apacket.is_video = 0;
			apacket.is_keyframe = 0;
			if (ntohl(apacket.data->ssrc) != a_last_ssrc) {
				a_last_ssrc = ntohl (apacket.data->ssrc);
				PS_LOG (LOG_INFO, "[%s] New audio stream! (ssrc=%u) \n", name, a_last_ssrc);
				a_base_ts_prev = a_last_ts;
				a_base_ts = ntohl (apacket.data->timestamp);
				a_base_seq_prev = a_last_seq;
				a_base_seq = ntohs (apacket.data->seq_number);
			}
			/* Assuming OPUS with framesize 20 */
			a_last_ts = (ntohl(apacket.data->timestamp)-a_base_ts) + a_base_ts_prev + 960;
			apacket.data->timestamp = htonl(a_last_ts);
			a_last_seq = (ntohl(apacket.data->seq_number)-a_base_seq) + a_base_seq_prev + 1;
			apacket.data->seq_number = htons(a_last_seq);
			apacket.data->type = mountpoint->codecs.audio_pt;
			apacket.timestamp = ntohl (apacket.data->timestamp);
			apacket.seq_number = ntohs (apacket.data->seq_number);
			ps_mutex_lock (&mountpoints_mutex);
			g_list_foreach (mountpoint->listeners, ps_gstreamer_relay_rtp_packet, &apacket);
			ps_mutex_unlock (&mountpoints_mutex);
			g_free (atempbuffer);
			//continue;
		}
		
		vsample = gst_app_sink_pull_sample (GST_APP_SINK (source->vsink));
		if (vsample != NULL) {
			if (mountpoint->active == FALSE) mountpoint->active = TRUE;
			source->last_received_video = ps_get_monotonic_time();
			vbuffer = gst_sample_get_buffer (vsample);
			gst_buffer_extract_dup (vbuffer, 0, -1, &vframedata, &vfsize);
			
			vtempbuffer = (char *)g_malloc0(vfsize);
			memcpy(vtempbuffer, vframedata, vfsize);
			g_free (vframedata);
			
			bytes = vfsize;
			gst_sample_unref (vsample);
			
			rtp_header * vrtp = (rtp_header *) vtempbuffer;
			if (source->keyframe.enabled) {
				if (source->keyframe.temp_ts > 0 && ntohl(vrtp->timestamp) != source->keyframe.temp_ts) {
					PS_LOG(LOG_HUGE, "[%s] ... last part of keyframe received! ts=%"SCNu32", %d packets\n", mountpoint->name, source->keyframe.temp_ts, g_list_length(source->keyframe.temp_keyframe));
					source->keyframe.temp_ts = 0;
					ps_mutex_lock(&source->keyframe.mutex);
					GList * temp = NULL;
					while (source->keyframe.latest_keyframe) {
						temp = g_list_first(source->keyframe.latest_keyframe);
						source->keyframe.latest_keyframe = g_list_remove_link (source->keyframe.latest_keyframe, temp);
						ps_gstreamer_rtp_relay_packet * pkt = (ps_gstreamer_rtp_relay_packet *) temp->data;
						g_free (pkt->data);
						g_free (pkt);
						g_free (temp);
					}
					source->keyframe.latest_keyframe = source->keyframe.temp_keyframe;
					source->keyframe.temp_keyframe = NULL;
					ps_mutex_unlock(&source->keyframe.mutex);
				} else if (ntohl (vrtp->timestamp) == source->keyframe.temp_ts) {
					ps_mutex_lock(&source->keyframe.mutex);
					PS_LOG(LOG_HUGE, "[%s] ... other part of keyframe received! ts=%"SCNu32"\n", mountpoint->name, source->keyframe.temp_ts);
					ps_gstreamer_rtp_relay_packet * pkt = g_malloc0(sizeof(ps_gstreamer_rtp_relay_packet));
					pkt->data = g_malloc0(bytes);
					memcpy (pkt->data, vframedata, bytes);
					pkt->data->ssrc = htons(1);
					pkt->data->type = mountpoint->codecs.video_pt;
					pkt->is_video = 1;
					pkt->is_keyframe = 1;
					pkt->length = bytes;
					pkt->timestamp = source->keyframe.temp_ts;
					pkt->seq_number = ntohs(vrtp->seq_number);
					source->keyframe.temp_keyframe = g_list_append(source->keyframe.temp_keyframe, pkt);
					ps_mutex_unlock(&source->keyframe.mutex);
				} else if (ps_gstreamer_is_keyframe(mountpoint->codecs.video_codec, vframedata, bytes)) {
					source->keyframe.temp_ts = ntohl(vrtp->timestamp);
					PS_LOG (LOG_HUGE, "[%s] New keyframe received! ts=%"SCNu32"\n", mountpoint->name, source->keyframe.temp_ts);
					ps_mutex_lock(&source->keyframe.mutex);
					ps_gstreamer_rtp_relay_packet * pkt = g_malloc0(sizeof(ps_gstreamer_rtp_relay_packet));
					pkt->data = g_malloc0(bytes);
					memcpy (pkt->data, vframedata, bytes);
					pkt->data->ssrc = htons(1);
					pkt->data->type = mountpoint->codecs.video_pt;
					pkt->is_video = 1;
					pkt->is_keyframe = 1;
					pkt->length = bytes;
					pkt->timestamp = source->keyframe.temp_ts;
					pkt->seq_number = ntohs(vrtp->seq_number);
					source->keyframe.temp_keyframe = g_list_append(source->keyframe.temp_keyframe, pkt);
					ps_mutex_unlock(&source->keyframe.mutex);
				}
			}
			
			if (!mountpoint->enabled) {
				continue;
			}
			vpacket.data = vrtp;
			vpacket.length = bytes;
			vpacket.is_video = 1;
			vpacket.is_keyframe = 0;
			if (ntohl(vpacket.data->ssrc) != v_last_ssrc) {
				v_last_ssrc = ntohl (vpacket.data->ssrc);
				PS_LOG(LOG_INFO, "[%s] New video stream! (ssrc=%u)\n", name, v_last_ssrc);
				v_base_ts_prev = v_last_ts;
				v_base_ts = ntohl(vpacket.data->timestamp);
				v_base_seq_prev = v_last_seq;
				v_base_seq = ntohs(vpacket.data->seq_number);
			}
			/* FIXME We're assuming 15fps here... */
			v_last_ts = (ntohl(vpacket.data->timestamp)-v_base_ts)+v_base_ts_prev+4500;	
			vpacket.data->timestamp = htonl(v_last_ts);
			v_last_seq = (ntohs(vpacket.data->seq_number)-v_base_seq)+v_base_seq_prev+1;
			vpacket.data->seq_number = htons(v_last_seq);
			vpacket.data->type = mountpoint->codecs.video_pt;
			vpacket.timestamp = ntohl(vpacket.data->timestamp);
			vpacket.seq_number = ntohs(vpacket.data->seq_number);
			/* Go! */
			ps_mutex_lock(&mountpoint->mutex);
			g_list_foreach(mountpoint->listeners, ps_gstreamer_relay_rtp_packet, &vpacket);
			ps_mutex_unlock(&mountpoint->mutex);
			g_free (vtempbuffer);
			//continue;
		}
		
	}
	
	//gst_element_set_state (source->pipeline, GST_STATE_NULL);
	ps_mutex_lock (&mountpoint->mutex);
	GList * viewer = g_list_first (mountpoint->listeners);
	
	/* Prepare JSON event */
	json_t *event = json_object();
	json_object_set_new(event, "streaming", json_string("event"));
	json_t *result = json_object();
	json_object_set_new(result, "status", json_string("stopped"));
	json_object_set_new(event, "result", result);
	char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
	json_decref(event);
	while(viewer) {
		ps_gstreamer_session *session = (ps_gstreamer_session *)viewer->data;
		if(session != NULL) {
			session->stopping = TRUE;
			session->started = FALSE;
			session->paused = FALSE;
			session->mountpoint = NULL;
			/* Tell the core to tear down the PeerConnection, hangup_media will do the rest */
			gateway->push_event(session->handle, &ps_gstreamer_plugin, NULL, event_text, NULL, NULL);
			gateway->close_pc(session->handle);
		}
		mountpoint->listeners = g_list_remove_all(mountpoint->listeners, session);
		viewer = g_list_first(mountpoint->listeners);
	}
	g_free(event_text);
	ps_mutex_unlock(&mountpoint->mutex);

	PS_LOG(LOG_VERB, "[%s] Leaving streaming relay thread\n", name);
	g_free(name);
	g_thread_unref(g_thread_self());
	return NULL;
}

static void ps_gstreamer_relay_rtp_packet (gpointer data, gpointer user_data) {
	ps_gstreamer_rtp_relay_packet * packet = (ps_gstreamer_rtp_relay_packet *) user_data;
	if (!packet || !packet->data || packet->length < 1) {
		PS_LOG(LOG_ERR, "Invalid Packet...\n");
		return;
	}
	ps_gstreamer_session * session = (ps_gstreamer_session *) data;
	if (!session || !session->handle) {
		PS_LOG(LOG_ERR, "Invalid Session...\n");
		return;
	}
	if (!packet->is_keyframe && (!session->started || session->paused)) {
		PS_LOG(LOG_ERR, "Streaming not started for this Session...\n");
		return;
	}
	
	if(packet->is_video) {
		if(ntohl(packet->data->ssrc) != session->context.v_last_ssrc) {
			session->context.v_last_ssrc = ntohl(packet->data->ssrc);
			session->context.v_base_ts_prev = session->context.v_last_ts;
			session->context.v_base_ts = packet->timestamp;
			session->context.v_base_seq_prev = session->context.v_last_seq;
			session->context.v_base_seq = packet->seq_number;
		}
		/* Compute a coherent timestamp and sequence number */
		session->context.v_last_ts = (packet->timestamp-session->context.v_base_ts)
			+ session->context.v_base_ts_prev+4500;	/* FIXME When switching, we assume 15fps */
		session->context.v_last_seq = (packet->seq_number-session->context.v_base_seq)+session->context.v_base_seq_prev+1;
		/* Update the timestamp and sequence number in the RTP packet, and send it */
		packet->data->timestamp = htonl(session->context.v_last_ts);
		packet->data->seq_number = htons(session->context.v_last_seq);
		if(gateway != NULL)
			gateway->relay_rtp(session->handle, packet->is_video, (char *)packet->data, packet->length);
		/* Restore the timestamp and sequence number to what the publisher set them to */
		packet->data->timestamp = htonl(packet->timestamp);
		packet->data->seq_number = htons(packet->seq_number);
	} else {
		if(ntohl(packet->data->ssrc) != session->context.a_last_ssrc) {
			session->context.a_last_ssrc = ntohl(packet->data->ssrc);
			session->context.a_base_ts_prev = session->context.a_last_ts;
			session->context.a_base_ts = packet->timestamp;
			session->context.a_base_seq_prev = session->context.a_last_seq;
			session->context.a_base_seq = packet->seq_number;
		}
		/* Compute a coherent timestamp and sequence number */
		session->context.a_last_ts = (packet->timestamp-session->context.a_base_ts)
			+ session->context.a_base_ts_prev+960;	/* FIXME When switching, we assume Opus and so a 960 ts step */
		session->context.a_last_seq = (packet->seq_number-session->context.a_base_seq)+session->context.a_base_seq_prev+1;
		/* Update the timestamp and sequence number in the RTP packet, and send it */
		packet->data->timestamp = htonl(session->context.a_last_ts);
		packet->data->seq_number = htons(session->context.a_last_seq);
		if(gateway != NULL)
			gateway->relay_rtp(session->handle, packet->is_video, (char *)packet->data, packet->length);
		/* Restore the timestamp and sequence number to what the publisher set them to */
		packet->data->timestamp = htonl(packet->timestamp);
		packet->data->seq_number = htons(packet->seq_number);
	}

	return;
	
}

/* Helpers to check if frame is a key frame (see post processor code) */
#if defined(__ppc__) || defined(__ppc64__)
	# define swap2(d)  \
	((d&0x000000ff)<<8) |  \
	((d&0x0000ff00)>>8)
#else
	# define swap2(d) d
#endif

static gboolean ps_gstreamer_is_keyframe (gint codec, char * buffer, int len) {
	if(codec == PS_GSTREAMER_VP8) {
		/* VP8 packet */
		if(!buffer || len < 28)
			return FALSE;
		/* Parse RTP header first */
		rtp_header *header = (rtp_header *)buffer;
		guint32 timestamp = ntohl(header->timestamp);
		guint16 seq = ntohs(header->seq_number);
		/*PS_LOG(LOG_HUGE, "Checking if VP8 packet (size=%d, seq=%"SCNu16", ts=%"SCNu32") is a key frame...\n",
			len, seq, timestamp);*/
		uint16_t skip = 0;
		if(header->extension) {
			janus_rtp_header_extension *ext = (janus_rtp_header_extension *)(buffer+12);
			PS_LOG(LOG_HUGE, "  -- RTP extension found (type=%"SCNu16", length=%"SCNu16")\n",
				ntohs(ext->type), ntohs(ext->length));
			skip = 4 + ntohs(ext->length)*4;
		}
		buffer += 12+skip;
		/* Parse VP8 header now */
		uint8_t vp8pd = *buffer;
		uint8_t xbit = (vp8pd & 0x80);
		uint8_t sbit = (vp8pd & 0x10);
		if(xbit) {
			PS_LOG(LOG_HUGE, "  -- X bit is set!\n");
			/* Read the Extended control bits octet */
			buffer++;
			vp8pd = *buffer;
			uint8_t ibit = (vp8pd & 0x80);
			uint8_t lbit = (vp8pd & 0x40);
			uint8_t tbit = (vp8pd & 0x20);
			uint8_t kbit = (vp8pd & 0x10);
			if(ibit) {
				PS_LOG(LOG_HUGE, "  -- I bit is set!\n");
				/* Read the PictureID octet */
				buffer++;
				vp8pd = *buffer;
				uint16_t picid = vp8pd, wholepicid = picid;
				uint8_t mbit = (vp8pd & 0x80);
				if(mbit) {
					PS_LOG(LOG_HUGE, "  -- M bit is set!\n");
					memcpy(&picid, buffer, sizeof(uint16_t));
					wholepicid = ntohs(picid);
					picid = (wholepicid & 0x7FFF);
					buffer++;
				}
				PS_LOG(LOG_HUGE, "  -- -- PictureID: %"SCNu16"\n", picid);
			}
			if(lbit) {
				PS_LOG(LOG_HUGE, "  -- L bit is set!\n");
				/* Read the TL0PICIDX octet */
				buffer++;
				vp8pd = *buffer;
			}
			if(tbit || kbit) {
				PS_LOG(LOG_HUGE, "  -- T/K bit is set!\n");
				/* Read the TID/KEYIDX octet */
				buffer++;
				vp8pd = *buffer;
			}
			buffer++;	/* Now we're in the payload */
			if(sbit) {
				PS_LOG(LOG_HUGE, "  -- S bit is set!\n");
				unsigned long int vp8ph = 0;
				memcpy(&vp8ph, buffer, 4);
				vp8ph = ntohl(vp8ph);
				uint8_t pbit = ((vp8ph & 0x01000000) >> 24);
				if(!pbit) {
					PS_LOG(LOG_HUGE, "  -- P bit is NOT set!\n");
					/* It is a key frame! Get resolution for debugging */
					unsigned char *c = (unsigned char *)buffer+3;
					/* vet via sync code */
					if(c[0]!=0x9d||c[1]!=0x01||c[2]!=0x2a) {
						PS_LOG(LOG_WARN, "First 3-bytes after header not what they're supposed to be?\n");
					} else {
						int vp8w = swap2(*(unsigned short*)(c+3))&0x3fff;
						int vp8ws = swap2(*(unsigned short*)(c+3))>>14;
						int vp8h = swap2(*(unsigned short*)(c+5))&0x3fff;
						int vp8hs = swap2(*(unsigned short*)(c+5))>>14;
						PS_LOG(LOG_HUGE, "Got a VP8 key frame: %dx%d (scale=%dx%d)\n", vp8w, vp8h, vp8ws, vp8hs);
						return TRUE;
					}
				}
			}
		}
		/* If we got here it's not a key frame */
		return FALSE;
	} else if(codec == PS_GSTREAMER_H264) {
		/* Parse RTP header first */
		rtp_header *header = (rtp_header *)buffer;
		guint32 timestamp = ntohl(header->timestamp);
		guint16 seq = ntohs(header->seq_number);
		PS_LOG(LOG_HUGE, "Checking if H264 packet (size=%d, seq=%"SCNu16", ts=%"SCNu32") is a key frame...\n",
			len, seq, timestamp);
		uint16_t skip = 0;
		if(header->extension) {
			janus_rtp_header_extension *ext = (janus_rtp_header_extension *)(buffer+12);
			PS_LOG(LOG_HUGE, "  -- RTP extension found (type=%"SCNu16", length=%"SCNu16")\n",
				ntohs(ext->type), ntohs(ext->length));
			skip = 4 + ntohs(ext->length)*4;
		}
		buffer += 12+skip;
		/* Parse H264 header now */
		uint8_t fragment = *buffer & 0x1F;
		uint8_t nal = *(buffer+1) & 0x1F;
		uint8_t start_bit = *(buffer+1) & 0x80;
		PS_LOG(LOG_HUGE, "Fragment=%d, NAL=%d, Start=%d\n", fragment, nal, start_bit);
		if(fragment == 5 ||
				((fragment == 28 || fragment == 29) && nal == 5 && start_bit == 128)) {
			PS_LOG(LOG_HUGE, "Got an H264 key frame\n");
			return TRUE;
		}
		/* If we got here it's not a key frame */
		return FALSE;
	} else {
		/* FIXME Not a clue */
		return FALSE;
	}
}

gboolean bus_msg (GstBus * bus, GstMessage * msg, gpointer * loop){
	GError * err, * warn;
	gchar * debug_info, * warn_info;
	PS_LOG (LOG_VERB,"Got %s message\n", GST_MESSAGE_TYPE_NAME (msg));
	
	switch (GST_MESSAGE_TYPE (msg)){
		case GST_MESSAGE_ERROR:
			gst_message_parse_error (msg, &err, &debug_info);
			PS_LOG (LOG_VERB,"Error received from element %s: %s\n",
			  GST_OBJECT_NAME (msg->src), err->message);
			PS_LOG (LOG_VERB,"debugging information: %s\n", debug_info ? debug_info : "none");
			g_clear_error (&err);
			g_free (debug_info);
			break;
			
		case GST_MESSAGE_EOS:
			PS_LOG (LOG_VERB,"End-of-stream reached.\n");
			break;
			
		case GST_MESSAGE_WARNING:
			gst_message_parse_warning (msg, &warn, &warn_info);
			PS_LOG (LOG_VERB,"Warning received from element %s: %s\n",
			  GST_OBJECT_NAME (msg->src), warn->message);
			PS_LOG (LOG_VERB,"debugging information: %s\n", warn_info ? warn_info : "none");
			g_clear_error (&err);
			g_free (debug_info);
			break;
			
		default:
			PS_LOG (LOG_VERB,"Unexpected message received on Gst Bus.\n");
	}
	return TRUE;
}

