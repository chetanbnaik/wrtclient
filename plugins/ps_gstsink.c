/*
 * 
 * gst-launch-1.0 videotestsrc ! videoscale ! videorate ! videoconvert ! 
 * timeoverlay ! vp8enc ! rtpvp8pay ! udpsink port=5004
 * 
 * gst-launch-1.0 -v udpsrc port=5004 caps="application/x-rtp" ! 
 * rtpvp8depay ! vp8dec ! videoconvert ! autovideosink
 * 
 * 
 * 
 * */
#include <dirent.h>
#include <jansson.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "ps_gstreamer.h"
#include "../apierror.h"
#include "../config.h"
#include "../mutex.h"
#include "../debug.h"
#include "../rtp.h"
#include "../rtcp.h"
#include "../utils.h"

/* plugin information */
#define PS_GSTSINK_VERSION			1
#define PS_GSTSINK_VERSION_STRING		"0.0.1"
#define PS_GSTSINK_DESCRIPTION		"PacketServo WebRTC Sink"
#define PS_GSTSINK_NAME				"PS GstSink WebRTC plugin"
#define PS_GSTSINK_AUTHOR				"PS"
#define PS_GSTSINK_PACKAGE			"ps.plugin.gstsink"

ps_plugin * create(void);
int ps_gstsink_init (ps_callbacks * callback, const char * config_path);
void ps_gstsink_destroy (void);
int ps_gstsink_get_api_compatibility (void);
int ps_gstsink_get_version (void);
const char * ps_gstsink_get_version_string (void);
const char * ps_gstsink_get_description (void);
const char * ps_gstsink_get_name (void);
const char * ps_gstsink_get_author (void);
const char * ps_gstsink_get_package (void);
void ps_gstsink_create_session (ps_plugin_session * handle, int * error);
struct ps_plugin_result * ps_gstsink_handle_message(ps_plugin_session * handle, char * transaction, char * message, char * sdp_type, char * sdp);
void ps_gstsink_setup_media (ps_plugin_session * handle);
void ps_gstsink_incoming_rtp (ps_plugin_session * handle, int video, char * buf, int len);
void ps_gstsink_incoming_rtcp (ps_plugin_session * handle, int video, char * buf, int len);
void ps_gstsink_incoming_data (ps_plugin_session * handle, char * buf, int len);
void ps_gstsink_slow_link (ps_plugin_session * handle, int uplink, int video);
void ps_gstsink_hangup_media (ps_plugin_session * handle);
void ps_gstsink_destroy_session (ps_plugin_session * handle, int * error);
char * ps_gstsink_query_session (ps_plugin_session * handle);

/* Plugin setup */
static ps_plugin ps_gstsink_plugin = PS_PLUGIN_INIT (
	.init = ps_gstsink_init,
	.destroy = ps_gstsink_destroy,
	
	.get_api_compatibility = ps_gstsink_get_api_compatibility,
	.get_version = ps_gstsink_get_version,
	.get_version_string = ps_gstsink_get_version_string,
	.get_description = ps_gstsink_get_description,
	.get_name = ps_gstsink_get_name,
	.get_author = ps_gstsink_get_author,
	.get_package = ps_gstsink_get_package,
	
	.create_session = ps_gstsink_create_session,
	.handle_message = ps_gstsink_handle_message,
	.setup_media = ps_gstsink_setup_media,
	.incoming_rtp = ps_gstsink_incoming_rtp,
	.incoming_rtcp = ps_gstsink_incoming_rtcp,
	.incoming_data = ps_gstsink_incoming_data,
	.slow_link = ps_gstsink_slow_link,
	.hangup_media = ps_gstsink_hangup_media,
	.destroy_session = ps_gstsink_destroy_session,
	.query_session = ps_gstsink_query_session,
);

/* plugin creator */
ps_plugin * create (void) {
	PS_LOG (LOG_VERB, "%s created!\n", PS_GSTSINK_NAME);
	return &ps_gstsink_plugin;
}

/*configuration
static ps_config * config = NULL;
static const char * config_file = NULL;
static ps_mutex config_mutex;*/

/* something useful */
static volatile gint initialized = 0, stopping = 0;
static ps_callbacks * gateway = NULL;
static GThread * handler_thread;
static GThread * watchdog;
static void * ps_gstsink_handler (void * data);

typedef struct ps_gstsink_message {
	ps_plugin_session * handle;
	char * transaction;
	json_t * message;
	char * sdp_type;
	char * sdp;
} ps_gstsink_message;
static GAsyncQueue * messages = NULL;
static ps_gstsink_message exit_message;

typedef struct ps_gstsink_rtp_header_extension {
	uint16_t type;
	uint16_t length;
} ps_gstsink_rtp_header_extension;

typedef struct ps_audio_player {
	GstElement * asource, * afilter, * artpdepay;
	GstElement * adecoder, * aconvert, * display;
	GstElement * apipeline;
	GstCaps * afiltercaps;
	gboolean isaCapsSet;
	GstBus * abus;
	gint64 last_received_audio;
} ps_audio_player;

typedef struct ps_video_packet {
	char * data;
	gint length;
	gint is_video;
} ps_video_packet;
static ps_video_packet eos_vpacket;

typedef struct ps_video_player {
	GstElement * vsource, * vfilter, * vrtpdepay;
	GstElement * vdecoder, * vconvert, * display;
	GstElement * vpipeline;
	GstCaps * vfiltercaps;
	gboolean isvCapsSet;
	GstBus * vbus;
	gint64 last_received_video;
} ps_video_player;

typedef struct ps_gstsink_session {
	ps_plugin_session *handle;
	gboolean active;
	gboolean recorder;
	gboolean firefox;
	gboolean play_video;
	gboolean play_audio;
	ps_audio_player * aplayer;
	ps_video_player * vplayer;
	GAsyncQueue * vpackets;
	guint video_remb_startup;
	guint64 video_remb_last;
	guint64 video_bitrate;
	guint video_keyframe_interval;
	guint64 video_keyframe_request_last;
	gint video_fir_seq;
	volatile gint hangingup;
	gint64 destroyed;	/* Time at which this session was marked as destroyed */
} ps_gstsink_session;
static GHashTable * sessions;
static GList * old_sessions;
static ps_mutex sessions_mutex;

static void * ps_gstsink_vplayer_thread (void * data);
void ps_gstsink_send_rtcp_feedback (ps_plugin_session * handle, int video, char * buf, int len);

/* SDP offer/answer templates for the playout */
#define OPUS_PT		111
#define VP8_PT		100
#define sdp_template \
		"v=0\r\n" \
		"o=- %"SCNu64" %"SCNu64" IN IP4 127.0.0.1\r\n"	/* We need current time here */ \
		"s=%s\r\n"							/* Recording playout id */ \
		"t=0 0\r\n" \
		"%s%s"								/* Audio and/or video m-lines */
#define sdp_a_template \
		"m=audio 1 RTP/SAVPF %d\r\n"		/* Opus payload type */ \
		"c=IN IP4 1.1.1.1\r\n" \
		"a=%s\r\n"							/* Media direction */ \
		"a=rtpmap:%d opus/48000/2\r\n"		/* Opus payload type */
#define sdp_v_template \
		"m=video 1 RTP/SAVPF %d\r\n"		/* VP8 payload type */ \
		"c=IN IP4 1.1.1.1\r\n" \
		"a=%s\r\n"							/* Media direction */ \
		"a=rtpmap:%d VP8/90000\r\n"			/* VP8 payload type */ \
		"a=rtcp-fb:%d ccm fir\r\n"			/* VP8 payload type */ \
		"a=rtcp-fb:%d nack\r\n"				/* VP8 payload type */ \
		"a=rtcp-fb:%d nack pli\r\n"			/* VP8 payload type */ \
		"a=rtcp-fb:%d goog-remb\r\n"		/* VP8 payload type */

/*static void ps_video_packet_free (ps_video_packet * pkt) {
	if (!pkt || pkt == &eos_vpacket) return;
	g_free (pkt->data);
	pkt->data = NULL;
	g_free (pkt);
}*/

static void ps_gstsink_message_free (ps_gstsink_message * msg) {
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

/* Error codes */
#define PS_GSTSINK_ERROR_NO_MESSAGE				411
#define PS_GSTSINK_ERROR_INVALID_JSON			412
#define PS_GSTSINK_ERROR_INVALID_REQUEST		413
#define PS_GSTSINK_ERROR_MISSING_ELEMENT		415
#define PS_GSTSINK_ERROR_INVALID_ELEMENT		414
#define PS_GSTSINK_ERROR_NOT_FOUND				416
#define PS_GSTSINK_ERROR_UNKNOWN_ERROR			499

/* Streaming watchdog/garbage collector (sort of) */
void *ps_gstsink_watchdog(void *data);
void *ps_gstsink_watchdog(void *data) {
	PS_LOG (LOG_INFO, "GstSink watchdog started\n");
	gint64 now = 0;
	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		ps_mutex_lock(&sessions_mutex);
		/* Iterate on all the sessions */
		now = ps_get_monotonic_time();
		if(old_sessions != NULL) {
			GList *sl = old_sessions;
			PS_LOG(LOG_HUGE, "Checking %d old GstSink sessions...\n", g_list_length(old_sessions));
			while(sl) {
				ps_gstsink_session *session = (ps_gstsink_session *)sl->data;
				if(!session) {
					sl = sl->next;
					continue;
				}
				if(now-session->destroyed >= 5*G_USEC_PER_SEC) {
					/* We're lazy and actually get rid of the stuff only after a few seconds */
					PS_LOG(LOG_VERB, "Freeing old GstSink session\n");
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
		g_usleep(500000);
	}
	PS_LOG(LOG_INFO, "GstSink watchdog stopped\n");
	return NULL;
}


int ps_gstsink_init (ps_callbacks * callback, const char * config_path) {
	if (g_atomic_int_get(&stopping)) return -1;
	//if (callback == NULL || config_path == NULL) return -1;
	if (callback == NULL) return -1;
	
	/* Initialize GSTREAMER */
	gst_init (NULL, NULL);
	
	sessions = g_hash_table_new (NULL, NULL);
	ps_mutex_init (&sessions_mutex);
	messages = g_async_queue_new_full ((GDestroyNotify) ps_gstsink_message_free);
	
	gateway = callback;
	g_atomic_int_set (&initialized, 1);
	
	GError * error = NULL;
	watchdog = g_thread_try_new ("streaming watchdog", &ps_gstsink_watchdog, NULL, &error);
	if (!watchdog) {
		g_atomic_int_set(&initialized, 0);
		PS_LOG (LOG_ERR, "Got error %d (%s) trying to launch the GstSink watchdog thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	handler_thread = g_thread_try_new ("streaming handler", &ps_gstsink_handler, NULL, &error);
	if (!handler_thread) {
		g_atomic_int_set(&initialized, 0);
		PS_LOG (LOG_ERR, "Got error %d (%s) trying to launch the GstSink handler thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	PS_LOG (LOG_INFO, "%s initialized\n", PS_GSTSINK_NAME);
	return 0;
}

void ps_gstsink_destroy (void) {
	if (!g_atomic_int_get(&initialized)) return;
	g_atomic_int_set(&stopping, 1);
	
	g_async_queue_push (messages, &exit_message);
	
	if (handler_thread != NULL) {
		g_thread_join (handler_thread);
		handler_thread = NULL;
	}
	
	if (watchdog != NULL) {
		g_thread_join (watchdog);
		watchdog = NULL;
	}
	usleep(500000);
	ps_mutex_lock(&sessions_mutex);
	/* Cleanup session data */
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init (&iter, sessions);
	while (g_hash_table_iter_next (&iter, NULL, &value)) {
		ps_gstsink_session * session = value;
		if (!session->destroyed && session->vplayer != NULL) {
			ps_video_player * player = session->vplayer;
			gst_object_unref (player->vbus);
			gst_element_set_state (player->vpipeline, GST_STATE_NULL);
			if (gst_element_get_state (player->vpipeline, NULL, NULL, GST_CLOCK_TIME_NONE) == GST_STATE_CHANGE_FAILURE) {
				PS_LOG (LOG_ERR, "Unable to stop GSTREAMER video player..!!\n");
			}
			gst_object_unref (GST_OBJECT(player->vpipeline));
		}
		g_hash_table_remove(sessions, session->handle);
		/* Cleaning up and removing the session is done in a lazy way */
		old_sessions = g_list_append(old_sessions, session);
	}
	
	g_hash_table_destroy(sessions);
	ps_mutex_unlock(&sessions_mutex);
	g_async_queue_unref(messages);
	messages = NULL;
	sessions = NULL;

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	PS_LOG(LOG_INFO, "%s destroyed!\n", PS_GSTSINK_NAME);
	
}

int ps_gstsink_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return PS_PLUGIN_API_VERSION;
}

int ps_gstsink_get_version(void) {
	return PS_GSTSINK_VERSION;
}

const char *ps_gstsink_get_version_string(void) {
	return PS_GSTSINK_VERSION_STRING;
}

const char *ps_gstsink_get_description(void) {
	return PS_GSTSINK_DESCRIPTION;
}

const char *ps_gstsink_get_name(void) {
	return PS_GSTSINK_NAME;
}

const char *ps_gstsink_get_author(void) {
	return PS_GSTSINK_AUTHOR;
}

const char *ps_gstsink_get_package(void) {
	return PS_GSTSINK_PACKAGE;
}

void ps_gstsink_create_session (ps_plugin_session * handle, int * error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		PS_LOG (LOG_WARN, "plugin not initialized..\n");
		*error = -1;
		return;
	}	
	ps_gstsink_session *session = (ps_gstsink_session *)g_malloc0(sizeof(ps_gstsink_session));
	if(session == NULL) {
		PS_LOG(LOG_FATAL, "Memory error!\n");
		*error = -2;
		return;
	}
	
	session->handle = handle;
	session->active = FALSE;
	session->recorder = FALSE;
	session->firefox = FALSE;
	session->play_audio = FALSE;
	session->play_video = FALSE;
	session->aplayer = NULL;
	session->vplayer = NULL;
	session->destroyed = 0;
	session->vpackets = NULL;
	g_atomic_int_set(&session->hangingup, 0);
	session->video_remb_startup = 4;
	session->video_remb_last = ps_get_monotonic_time();
	session->video_bitrate = 1024 * 1024;
	session->video_keyframe_request_last = 0;
	session->video_keyframe_interval = 15000;
	session->video_fir_seq = 0;
	handle->plugin_handle = session;
	ps_mutex_lock(&sessions_mutex);
	g_hash_table_insert(sessions, handle, session);
	ps_mutex_unlock(&sessions_mutex);

	return;
}

void ps_gstsink_destroy_session(ps_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}	
	ps_gstsink_session *session = (ps_gstsink_session *)handle->plugin_handle; 
	if(!session) {
		PS_LOG(LOG_ERR, "No session associated with this handle...\n");
		*error = -2;
		return;
	}
	PS_LOG(LOG_VERB, "Removing GstSink session...\n");
	/* Stop the GSTREAMER pipeline here */
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

char *ps_gstsink_query_session(ps_plugin_session *handle) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		return NULL;
	}	
	ps_gstsink_session *session = (ps_gstsink_session *)handle->plugin_handle;
	if(!session) {
		PS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return NULL;
	}
	/* What is this user watching, if anything? */
	json_t *info = json_object();
	json_object_set_new(info, "type", json_string(session->vplayer ? "sinking" : "idle"));
	if(session->vplayer) {
		json_object_set_new(info, "gstsink_id", json_integer(100));
		json_object_set_new(info, "gstsink_name", json_string("PacketServo"));
	}
	json_object_set_new(info, "destroyed", json_integer(session->destroyed));
	char *info_text = json_dumps(info, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
	json_decref(info);
	return info_text;
}

struct ps_plugin_result * ps_gstsink_handle_message (ps_plugin_session * handle, char * transaction, char * message, char * sdp_type, char * sdp) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return ps_plugin_result_new(PS_PLUGIN_ERROR, g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized");
	
	int error_code = 0;
	char error_cause[512];
	json_t * root = NULL;
	json_t * response = NULL;
	
	if (message == NULL) {
		PS_LOG (LOG_ERR, "No message??\n");
		error_code = PS_GSTSINK_ERROR_NO_MESSAGE;
		g_snprintf (error_cause, 512, "%s", "No message");
		goto error;
	}
	PS_LOG (LOG_VERB, "Handling message: %s\n", message);
	ps_gstsink_session * session = (ps_gstsink_session *) handle->plugin_handle;
	
	if(!session) {
		PS_LOG(LOG_ERR, "No session associated with this handle...\n");
		error_code = PS_GSTSINK_ERROR_UNKNOWN_ERROR;
		g_snprintf(error_cause, 512, "%s", "session associated with this handle...");
		goto error;
	}
	if(session->destroyed) {
		PS_LOG(LOG_ERR, "Session has already been destroyed...\n");
		error_code = PS_GSTSINK_ERROR_UNKNOWN_ERROR;
		g_snprintf(error_cause, 512, "%s", "Session has already been destroyed...");
		goto error;
	}
	json_error_t error;
	root = json_loads(message, 0, &error);
	if(!root) {
		PS_LOG(LOG_ERR, "JSON error: on line %d: %s\n", error.line, error.text);
		error_code = PS_GSTSINK_ERROR_INVALID_JSON;
		g_snprintf(error_cause, 512, "JSON error: on line %d: %s", error.line, error.text);
		goto error;
	}
	if(!json_is_object(root)) {
		PS_LOG(LOG_ERR, "JSON error: not an object\n");
		error_code = PS_GSTSINK_ERROR_INVALID_JSON;
		g_snprintf(error_cause, 512, "JSON error: not an object");
		goto error;
	}
	
	json_t * request = json_object_get (root, "request");
	if (!request) {
		PS_LOG (LOG_ERR, "Missing element (request)\n");
		error_code = PS_GSTSINK_ERROR_MISSING_ELEMENT;
		g_snprintf (error_cause, 512, "Missing element (request)");
		goto error;
	}
	if (!json_is_string(request)) {
		PS_LOG (LOG_ERR, "Invalid element (request should be a string)\n");
		error_code = PS_GSTSINK_ERROR_INVALID_ELEMENT;
		g_snprintf (error_cause, 512, "Invalid element (request should be a string)");
		goto error;
	}
	
	const char * request_text = json_string_value (request);
	if (!strcasecmp(request_text, "list")) {
		json_t * list = json_array();
		PS_LOG(LOG_VERB, "Request for list of recordings\n");
		json_t * ml = json_object();
		json_object_set_new(ml, "id", json_integer(100));
		json_object_set_new(ml, "name", json_string("test"));
		json_object_set_new(ml, "date", json_string("06/06/2016"));
		json_object_set_new(ml, "audio", json_string("true"));
		json_object_set_new(ml, "video", json_string("true"));
		json_array_append_new(list, ml);
		response = json_object();
		json_object_set_new(response, "recordplay", json_string("list"));
		json_object_set_new(response, "list", list);
		goto plugin_response;
	} else if(!strcasecmp(request_text, "configure")) {
		json_t *video_bitrate_max = json_object_get(root, "video-bitrate-max");
		if(video_bitrate_max) {
			if(!json_is_integer(video_bitrate_max) || json_integer_value(video_bitrate_max) < 0) {
				PS_LOG(LOG_ERR, "Invalid element (video-bitrate-max should be a positive integer)\n");
				error_code = PS_GSTSINK_ERROR_INVALID_ELEMENT;
				g_snprintf(error_cause, 512, "Invalid element (video-bitrate-max should be a positive integer)");
				goto error;
			}
			session->video_bitrate = json_integer_value(video_bitrate_max);
			PS_LOG(LOG_VERB, "Video bitrate has been set to %"SCNu64"\n", session->video_bitrate);
		}
		json_t *video_keyframe_interval= json_object_get(root, "video-keyframe-interval");
		if(video_keyframe_interval) {
			if(!json_is_integer(video_keyframe_interval) || json_integer_value(video_keyframe_interval) < 0) {
				PS_LOG(LOG_ERR, "Invalid element (video-keyframe-interval should be a positive integer)\n");
				error_code = PS_GSTSINK_ERROR_INVALID_ELEMENT;
				g_snprintf(error_cause, 512, "Invalid element (video-keyframe-interval should be a positive integer)");
				goto error;
			}
			session->video_keyframe_interval = json_integer_value(video_keyframe_interval);
			PS_LOG(LOG_VERB, "Video keyframe interval has been set to %u\n", session->video_keyframe_interval);
		}
		response = json_object();
		json_object_set_new(response, "recordplay", json_string("configure"));
		json_object_set_new(response, "status", json_string("ok"));
		/* Return a success, and also let the client be aware of what changed, to allow crosschecks */
		json_t *settings = json_object();
		json_object_set_new(settings, "video-keyframe-interval", json_integer(session->video_keyframe_interval)); 
		json_object_set_new(settings, "video-bitrate-max", json_integer(session->video_bitrate)); 
		json_object_set_new(response, "settings", settings); 
		goto plugin_response;
	} else if (!strcasecmp(request_text, "start") || !strcasecmp(request_text,"stop")) {
		ps_gstsink_message * msg = g_malloc0(sizeof(ps_gstsink_message));
		if (msg==NULL) {
			PS_LOG (LOG_FATAL, "Memory Error!\n");
			error_code = PS_GSTSINK_ERROR_UNKNOWN_ERROR;
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
		error_code = PS_GSTSINK_ERROR_INVALID_REQUEST;
		g_snprintf (error_cause, 512, "Unknown request '%s'",request_text);
		goto error;
	}

plugin_response:
	{
		if(!response) {
			error_code = PS_GSTSINK_ERROR_UNKNOWN_ERROR;
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
		json_object_set_new(event, "recordplay", json_string("event"));
		json_object_set_new(event, "error_code", json_integer(error_code));
		json_object_set_new(event, "error", json_string(error_cause));
		char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
		json_decref(event);
		ps_plugin_result *result = ps_plugin_result_new(PS_PLUGIN_OK, event_text);
		g_free(event_text);
		return result;
	}
	
}

void ps_gstsink_setup_media (ps_plugin_session * handle) {
	PS_LOG (LOG_INFO, "WebRTC media is now available\n");
	if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) return;
	
	ps_gstsink_session * session = (ps_gstsink_session *)handle->plugin_handle;
	if (!session) {
		PS_LOG(LOG_ERR, "No session associated with this handle!\n");
		return;
	}
	if(session->destroyed)
		return;
	g_atomic_int_set(&session->hangingup, 0);
	
	session->active = TRUE;
	/* As of now, it plays only VP8 stream */
	if (session->play_video) {
		ps_video_player * vsink = (ps_video_player *)g_malloc0(sizeof(ps_video_player));
		if (vsink == NULL) {
			PS_LOG (LOG_FATAL, "Memory error\n");
			/* FIXME: clean up session here, if pipeline fails */
			return;
		}
		vsink->vsource = gst_element_factory_make ("appsrc","vsource");
		vsink->vfiltercaps = NULL;
		vsink->isvCapsSet = FALSE;
		vsink->vrtpdepay = gst_element_factory_make ("rtpvp8depay", "vrtpdepay");
		vsink->vdecoder = gst_element_factory_make ("vp8dec", "vdecoder");
		vsink->vconvert = gst_element_factory_make ("videoconvert", "vconvert");
		vsink->display = gst_element_factory_make ("autovideosink", "display");
		vsink->vpipeline = gst_pipeline_new ("pipeline");
		gst_bin_add_many(GST_BIN(vsink->vpipeline), vsink->vsource, 
			vsink->vrtpdepay, vsink->vdecoder, vsink->vconvert, vsink->display, NULL);
		if (gst_element_link_many (vsink->vsource, vsink->vrtpdepay, vsink->vdecoder, vsink->vconvert, vsink->display, NULL) != TRUE) {
			PS_LOG (LOG_ERR, "Failed to link GSTREAMER elements in video player!!!\n");
			gst_object_unref (GST_OBJECT(vsink->vpipeline));
			g_free (vsink);
			/* FIXME: clean up session here, if pipeline fails */
			return;
		}
		vsink->vbus = gst_pipeline_get_bus (GST_PIPELINE (vsink->vpipeline));
		/*gst_bus_add_signal_watch (vsink->vbus);
		g_signal_connect (vsink->vbus, "message::error", G_CALLBACK()); */
		vsink->last_received_video = ps_get_monotonic_time();
		session->vplayer = vsink;
		session->vpackets = g_async_queue_new ();
		GError * error = NULL;
		g_thread_try_new ("playout", &ps_gstsink_vplayer_thread, session, &error);
		if (error != NULL) {
			PS_LOG (LOG_ERR, "Got error %d (%s) trying to launch the gstreamer thread...\n", error->code, error->message ? error->message : "??");
			gst_object_unref (GST_OBJECT(vsink->vpipeline));
			g_free (vsink);
		}
	}
	
	PS_LOG(LOG_VERB,"Start playing the GST pipeline here\n");
	
	return;
	/* Prepare JSON event 
	json_t *event = json_object();
	json_object_set_new(event, "streaming", json_string("event"));
	json_t *result = json_object();
	json_object_set_new(result, "status", json_string("started"));
	json_object_set_new(event, "result", result);
	char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
	json_decref(event);
	PS_LOG(LOG_VERB, "Pushing event: %s\n", event_text);
	int ret = gateway->push_event(handle, &ps_gstsink_plugin, NULL, event_text, NULL, NULL);
	PS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
	g_free(event_text); */
}

void ps_gstsink_send_rtcp_feedback (ps_plugin_session *handle, int video, char *buf, int len) {
	if(video != 1)
		return;	/* We just do this for video, for now */

	ps_gstsink_session *session = (ps_gstsink_session *)handle->plugin_handle;
	char rtcpbuf[24];

	/* Send a RR+SDES+REMB every five seconds, or ASAP while we are still
	 * ramping up (first 4 RTP packets) */
	gint64 now = ps_get_monotonic_time();
	guint64 elapsed = now - session->video_remb_last;
	gboolean remb_rampup = session->video_remb_startup > 0;

	if(remb_rampup || (elapsed >= 5*G_USEC_PER_SEC)) {
		guint64 bitrate = session->video_bitrate;

		if(remb_rampup) {
			bitrate = bitrate / session->video_remb_startup;
			session->video_remb_startup--;
		}

		/* Send a new REMB back */
		char rtcpbuf[24];
		janus_rtcp_remb((char *)(&rtcpbuf), 24, bitrate);
		gateway->relay_rtcp(handle, video, rtcpbuf, 24);

		session->video_remb_last = now;
	}

	/* Request a keyframe on a regular basis (every session->video_keyframe_interval ms) */
	elapsed = now - session->video_keyframe_request_last;
	guint64 interval = (session->video_keyframe_interval / 1000) * G_USEC_PER_SEC;

	if(elapsed >= interval) {
		/* Send both a FIR and a PLI, just to be sure */
		memset(rtcpbuf, 0, 20);
		janus_rtcp_fir((char *)&rtcpbuf, 20, &session->video_fir_seq);
		gateway->relay_rtcp(handle, video, rtcpbuf, 20);
		memset(rtcpbuf, 0, 12);
		janus_rtcp_pli((char *)&rtcpbuf, 12);
		gateway->relay_rtcp(handle, video, rtcpbuf, 12);
		session->video_keyframe_request_last = now;
	}
}

void ps_gstsink_incoming_rtp (ps_plugin_session * handle, int video, char * buf, int len) {
	if (handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	if (gateway) {
		ps_gstsink_session * session = (ps_gstsink_session *)handle->plugin_handle;
		if(!session) {
			PS_LOG(LOG_ERR, "No session associated with this handle\n");
			return;
		}
		if (session->destroyed) return;
		if (session->active && session->vplayer != NULL) {
			ps_video_packet * pkt = (ps_video_packet *)g_malloc0(sizeof(ps_video_packet));
			if (pkt == NULL) {
				PS_LOG (LOG_FATAL, "Memory error!\n");
				return;
			}
			pkt->data = (char *)g_malloc0(len);
			memcpy(pkt->data, buf, len);
			pkt->length = len;
			pkt->is_video = video;
			if (session->vpackets != NULL)
				g_async_queue_push (session->vpackets, pkt);
		}
		
		return;
	}
}

void ps_gstsink_incoming_rtcp (ps_plugin_session * handle, int video, char * buf, int len) {
	if (handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
		
	uint64_t bw = janus_rtcp_get_remb (buf, len);
	if (bw > 0) {
		PS_LOG (LOG_HUGE, "REMB for this PeerConnection: %"SCNu64"\n", bw);
	}
}

void ps_gstsink_incoming_data (ps_plugin_session * handle, char * buf, int len) {
	return;
}

void ps_gstsink_slow_link(ps_plugin_session *handle, int uplink, int video) {
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized) || !gateway)
		return;

	ps_gstsink_session *session = (ps_gstsink_session *)handle->plugin_handle;
	if(!session || session->destroyed)
		return;

	json_t *event = json_object();
	json_object_set_new(event, "recordplay", json_string("event"));
	json_t *result = json_object();
	json_object_set_new(result, "status", json_string("slow_link"));
	/* What is uplink for the server is downlink for the client, so turn the tables */
	json_object_set_new(result, "current-bitrate", json_integer(session->video_bitrate));
	json_object_set_new(result, "uplink", json_integer(uplink ? 0 : 1));
	json_object_set_new(event, "result", result);
	char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
	json_decref(event);
	json_decref(result);
	event = NULL;
	gateway->push_event(session->handle, &ps_gstsink_plugin, NULL, event_text, NULL, NULL);
	g_free(event_text);
}

void ps_gstsink_hangup_media (ps_plugin_session * handle) {
	PS_LOG(LOG_INFO, "No WebRTC media anymore\n");
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	ps_gstsink_session *session = (ps_gstsink_session *)handle->plugin_handle;	
	if(!session) {
		PS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(session->destroyed)
		return;
	if(g_atomic_int_add(&session->hangingup, 1))
		return;
	
	/* Send an event to the browser and tell it's over */
	json_t *event = json_object();
	json_object_set_new(event, "recordplay", json_string("event"));
	json_object_set_new(event, "result", json_string("done"));
	char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
	json_decref(event);
	PS_LOG(LOG_VERB, "Pushing event: %s\n", event_text);
	int ret = gateway->push_event(handle, &ps_gstsink_plugin, NULL, event_text, NULL, NULL);
	PS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
	g_free(event_text);
	
	g_async_queue_push(session->vpackets, &eos_vpacket);
	
	/* FIXME Simulate a "stop" coming from the browser, Could be a simulated EOS !!
	ps_gstsink_message *msg = g_malloc0(sizeof(ps_gstsink_message));
	msg->handle = handle;
	msg->message = json_loads("{\"request\":\"stop\"}", 0, NULL);
	msg->transaction = NULL;
	msg->sdp_type = NULL;
	msg->sdp = NULL;
	g_async_queue_push(messages, msg);*/
}

static void * ps_gstsink_vplayer_thread (void * data) {
	
	ps_gstsink_session * session = (ps_gstsink_session *) data;
	if (session == NULL) {
		PS_LOG (LOG_ERR, "invalid session!\n");
		g_thread_unref (g_thread_self());
		return NULL; 
	}
	if (session->vplayer == NULL) {
		PS_LOG (LOG_ERR, "Invalid gstreamer pipeline..\n");
		g_thread_unref (g_thread_self());
		return NULL;
	}
	ps_video_player * player = session->vplayer;
	gst_element_set_state (player->vpipeline, GST_STATE_PLAYING);
	if (gst_element_get_state (player->vpipeline, NULL, NULL, 500000000) == GST_STATE_CHANGE_FAILURE) {
		PS_LOG (LOG_ERR, "Unable to play pipeline..!\n");
		session->active = FALSE;
		g_thread_unref (g_thread_self());
		return NULL;
	}
	GstBuffer * feedbuffer;
	GstFlowReturn ret;
	ps_video_packet * packet = NULL;
	PS_LOG (LOG_VERB, "Joining GstSink gstreamer player thread..\n");
	while (!g_atomic_int_get (&stopping) && g_atomic_int_get(&initialized) && !g_atomic_int_get(&session->hangingup)) {
		packet = g_async_queue_pop (session->vpackets);
		if (packet == NULL) continue;
		if (packet == &eos_vpacket) break;
		if (packet->data == NULL) continue;
		rtp_header * rtp = (rtp_header *) packet->data;
		
		if (!player->isvCapsSet) {
			player->vfiltercaps = gst_caps_new_simple ("application/x-rtp",
				"media", G_TYPE_STRING, "video",
				"clock-rate", G_TYPE_INT, 90000,
				"encoding-name", G_TYPE_STRING, "VP8-DRAFT-IETF-01",
				"payload", G_TYPE_INT, 96,
				"ssrc", G_TYPE_UINT, ntohl(rtp->ssrc),
				"timestamp-offset", G_TYPE_UINT, ntohl(rtp->timestamp),
				"seqnum-offset", G_TYPE_INT, ntohs(rtp->seq_number),
				NULL);
			g_object_set (player->vsource, "caps", player->vfiltercaps, NULL);
			gst_caps_unref (player->vfiltercaps);
			player->isvCapsSet = TRUE;
		}
		
		feedbuffer = gst_buffer_new_wrapped (packet->data, packet->length);
		ret = gst_app_src_push_buffer (GST_APP_SRC(player->vsource), feedbuffer);
		if (ret != GST_FLOW_OK) {
			PS_LOG (LOG_WARN, "Incoming rtp packet not pushed!!\n");
		}
		player->last_received_video = ps_get_monotonic_time();
	}

	gst_object_unref (player->vbus);
	gst_element_set_state (player->vpipeline, GST_STATE_NULL);
	if (gst_element_get_state (player->vpipeline, NULL, NULL, GST_CLOCK_TIME_NONE) == GST_STATE_CHANGE_FAILURE) {
		PS_LOG (LOG_ERR, "Unable to stop GSTREAMER video player..!!\n");
	}
	gst_object_unref (GST_OBJECT(player->vpipeline));
	session->play_video = FALSE;

	if (session->vpackets != NULL)
		g_async_queue_unref (session->vpackets);
	
	/* FIXME: Send EOS on the gstreamer pipeline */
	PS_LOG (LOG_VERB, "Leaving GstSink gstreamer player thread..\n");
	g_thread_unref (g_thread_self());
	return NULL;
}

static void * ps_gstsink_handler (void * data) {
	PS_LOG (LOG_VERB, "Joining GstSink handler thread..\n");
	ps_gstsink_message * msg = NULL;
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
			ps_gstsink_message_free(msg);
			continue;
		}
		ps_gstsink_session * session = NULL;
		ps_mutex_lock (&sessions_mutex);
		if (g_hash_table_lookup(sessions, msg->handle) != NULL) {
			session = (ps_gstsink_session *)msg->handle->plugin_handle;
		}
		ps_mutex_unlock (&sessions_mutex);
		if (!session) {
			PS_LOG (LOG_ERR, "no session associated with this handle...\n");
			ps_gstsink_message_free (msg);
			continue;
		}
		if (session->destroyed) {
			ps_gstsink_message_free (msg);
			continue;
		}
		
		error_code = 0;
		root = NULL;
		if (msg->message == NULL) {
			PS_LOG (LOG_ERR, "No message??\n");
			error_code = PS_GSTSINK_ERROR_NO_MESSAGE;
			g_snprintf(error_cause, 512, "%s","No Message??");
			goto error;
		}
		root = msg->message;
		json_t * request = json_object_get(root, "request");
		if (!request) {
			PS_LOG (LOG_ERR, "Missing element (request)\n");
			error_code = PS_GSTSINK_ERROR_MISSING_ELEMENT;
			g_snprintf (error_cause, 512, "Missing element (request)");
			goto error;
		}
		if (!json_is_string(request)) {
			PS_LOG (LOG_ERR, "Invalid element (request must be a string)\n");
			error_code = PS_GSTSINK_ERROR_MISSING_ELEMENT;
			g_snprintf (error_cause, 512, "Invalid element (request must be string)");
			goto error;
		}
		const char * request_text = json_string_value (request);
		json_t * result = NULL;
		//const char * sdp_type = NULL;
		char * sdp = NULL;
		
		if(!strcasecmp(request_text, "start")) {
			if(!msg->sdp) {
				PS_LOG(LOG_ERR, "Missing SDP offer\n");
				error_code = PS_GSTSINK_ERROR_MISSING_ELEMENT;
				g_snprintf(error_cause, 512, "Missing SDP offer");
				goto error;
			}
			
			guint64 id = 100;
			if(strstr(msg->sdp, "m=audio")) {
				PS_LOG (LOG_VERB, "Audio requested\n");
			}
			if(strstr(msg->sdp, "m=video")) {
				PS_LOG (LOG_VERB, "Video requested\n");
			}
			session->recorder = TRUE;
			/* We need to prepare an answer */
			int opus_pt = 0, vp8_pt = 0;
			char * opus_dir = NULL;
			char * vp8_dir = NULL;
			opus_dir = ps_get_opus_dir (msg->sdp);
			vp8_dir = ps_get_vp8_dir (msg->sdp);
			PS_LOG (LOG_VERB, "Audio direction: %s, Video direction: %s\n", opus_dir, vp8_dir);
			opus_pt = ps_get_opus_pt(msg->sdp);
			PS_LOG(LOG_VERB, "Opus payload type is %d\n", opus_pt);
			vp8_pt = ps_get_vp8_pt(msg->sdp);
			PS_LOG(LOG_VERB, "VP8 payload type is %d\n", vp8_pt);
			char sdptemp[1024], audio_mline[256], video_mline[512];
			if(opus_pt > 0 && opus_dir != NULL) {
				if (!strcasecmp(opus_dir, "sendrecv") || !strcasecmp(opus_dir, "sendonly")){
					g_snprintf(audio_mline, 256, sdp_a_template,
						opus_pt,						/* Opus payload type */
						"recvonly",						/* FIXME to check a= line */
						opus_pt); 						/* Opus payload type */
					session->play_audio = TRUE;
				} else {
					g_snprintf(audio_mline, 256, sdp_a_template,
						opus_pt,						/* Opus payload type */
						"inactive",						/* FIXME to check a= line */
						opus_pt); 						/* Opus payload type */
				}
			} else {
				audio_mline[0] = '\0';
			}
			if(vp8_pt > 0 && vp8_dir != NULL) {
				if (!strcasecmp(vp8_dir, "sendrecv") || !strcasecmp(vp8_dir, "sendonly")){
					g_snprintf(video_mline, 512, sdp_v_template,
						vp8_pt,							/* VP8 payload type */
						"recvonly",						/* FIXME to check a= line */
						vp8_pt, 						/* VP8 payload type */
						vp8_pt, 						/* VP8 payload type */
						vp8_pt, 						/* VP8 payload type */
						vp8_pt, 						/* VP8 payload type */
						vp8_pt); 						/* VP8 payload type */
					session->play_video = TRUE;
				} else {
					g_snprintf(video_mline, 512, sdp_v_template,
						vp8_pt,							/* VP8 payload type */
						"inactive",						/* FIXME to check a= line */
						vp8_pt, 						/* VP8 payload type */
						vp8_pt, 						/* VP8 payload type */
						vp8_pt, 						/* VP8 payload type */
						vp8_pt, 						/* VP8 payload type */
						vp8_pt); 						/* VP8 payload type */
				}
			} else {
				video_mline[0] = '\0';
			}
			g_snprintf(sdptemp, 1024, sdp_template,
				ps_get_real_time(),			/* We need current time here */
				ps_get_real_time(),			/* We need current time here */
				"PacketServo",		/* Playout session */
				audio_mline,					/* Audio m-line, if any */
				video_mline);					/* Video m-line, if any */
			sdp = g_strdup(sdptemp);
			PS_LOG(LOG_VERB, "Going to answer this SDP:\n%s\n", sdp);
			/* Done! */
			result = json_object();
			json_object_set_new(result, "status", json_string("recording"));
			json_object_set_new(result, "id", json_integer(id));
		} else if (!strcasecmp(request_text,"stop")) {
			session->active = FALSE;
			/* Set gst pipeline to paused or set to null? */
			/* session->aplayer?; session->vplayer?; */
			session->recorder = FALSE;
			/* Done */
			result = json_object();
			json_object_set_new(result, "status", json_string("stopped"));
		} else {
			PS_LOG(LOG_VERB, "Unknown request '%s'\n", request_text);
			error_code = PS_GSTSINK_ERROR_INVALID_REQUEST;
			g_snprintf(error_cause, 512, "Unknown request '%s'", request_text);
			goto error;
		}
		
		/* Any SDP to handle? */
		if (msg->sdp) {
			session->firefox = strstr(msg->sdp, "Mozilla") ? TRUE : FALSE;
			PS_LOG (LOG_VERB, "This is involving negotiation (%s) as well: \n%s\n", msg->sdp_type, msg->sdp);
		}
		
		/* Prepare JSON event */
		json_t * event = json_object();
		json_object_set_new (event, "recordplay", json_string("event"));
		if (result != NULL) json_object_set_new (event, "result", result);
		char * event_text = json_dumps (event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
		json_decref (event);
		PS_LOG (LOG_VERB, "Pushing event: %s\n",event_text);
		if(!sdp) {
			int ret = gateway->push_event(msg->handle, &ps_gstsink_plugin, msg->transaction, event_text, NULL, NULL);
			PS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
		} else {
			const char *type = session->recorder ? "answer" : "offer";
			/* How long will the gateway take to push the event? */
			g_atomic_int_set(&session->hangingup, 0);
			gint64 start = ps_get_monotonic_time();
			int res = gateway->push_event(msg->handle, &ps_gstsink_plugin, msg->transaction, event_text, type, sdp);
			PS_LOG(LOG_VERB, "  >> Pushing event: %d (took %"SCNu64" us)\n",
				res, ps_get_monotonic_time()-start);
			g_free(sdp);
		}
		
		g_free (event_text);
		ps_gstsink_message_free (msg);
		continue;
		
error:
		{
			/* Prepare JSON error event */
			json_t *event = json_object();
			json_object_set_new(event, "recordplay", json_string("event"));
			json_object_set_new(event, "error_code", json_integer(error_code));
			json_object_set_new(event, "error", json_string(error_cause));
			char *event_text = json_dumps(event, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
			json_decref(event);
			PS_LOG(LOG_VERB, "Pushing event: %s\n", event_text);
			int ret = gateway->push_event(msg->handle, &ps_gstsink_plugin, msg->transaction, event_text, NULL, NULL);
			PS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
			g_free(event_text);
			ps_gstsink_message_free(msg);
		}
		
	}
	g_free(error_cause);
	PS_LOG (LOG_VERB, "Leaving GstSink handler thread..\n");
	return NULL;
}


