/* file: ps_websockets.c
 * Author: Chetan NAIK (chetan@packetservo.com)
 * GNU General Publick License v3
 * Implementation of websockets transport
 * utilizing libwebsockets
 */

#include <libwebsockets.h>
#include "ps_websockets.h"
#include "../mutex.h"
#include "../debug.h"

typedef struct ps_websockets_client {
	struct lws * wsi;
	GAsyncQueue * messages;
	char * incoming;
	unsigned char * buffer;
	int buflen;
	int bufpending;
	int bufoffset;
	ps_mutex mutex;
	gint session_timeout:1;
	gint destroy:1;
} ps_websockets_client;

/* Transport Methods */
ps_transport * create(void);
int ps_websockets_init(ps_transport_callbacks * callback);
void ps_websockets_destroy(void);
int ps_websockets_send_message(void *transport, json_t *message);
void ps_websockets_session_created(void *transport, guint64 session_id);
void ps_websockets_session_over(void * transport, guint64 session_id, gboolean timeout);

/* Transport Setup */
static ps_transport ps_websockets_transport = PS_TRANSPORT_INIT (
.init = ps_websockets_init,
.destroy = ps_websockets_destroy,
.send_message = ps_websockets_send_message,
.session_created = ps_websockets_session_created,
.session_over = ps_websockets_session_over,
);

/* Transport creator */
ps_transport * create(void) {
	return &ps_websockets_transport;
}

/* something useful */
static gint initialized = 0, stopping = 0;
static ps_transport_callbacks * gateway = NULL;
static int wsport = 8080; 
static char host[] = "localhost";
static char path[] = "/ws";

/* Websockets thread */
static GThread * wss_thread;
void * ps_websockets_thread(void * data);

static struct lws_context * wss = NULL;

static int ps_websockets_callback (
	struct lws * wsi,
	enum lws_callback_reasons reason,
	void * user, 
	void * in, 
	size_t len
);

static struct lws_protocols wss_protocols[] = {
	{"ps-protocol", ps_websockets_callback, 
	sizeof(ps_websockets_client), 0},
	{NULL, NULL, 0}
};

int ps_websockets_init (ps_transport_callbacks * callback) {
	
	if (g_atomic_int_get(&stopping)) {
		return -1;
	}
	if (callback == NULL) {
		return -1;
	}
	
	gateway = callback;
	struct lws * wsi;
	
	/* Set Log level */
	lws_set_log_level (0, NULL);
	
	/* Prepare context */
	struct lws_context_creation_info info;
	struct lws_client_connect_info cinfo;
	memset(&info, 0, sizeof info);
	memset(&cinfo, 0, sizeof cinfo);
	
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.iface = NULL;
	info.protocols = wss_protocols;
	info.extensions = NULL; //lws_get_internal_extensions();
	info.ssl_cert_filepath = NULL;
	info.ssl_private_key_filepath = NULL;
	info.gid = -1;
	info.uid = -1;
	info.options = 0;
	
	/* Create the websockets context */
	wss = lws_create_context(&info);
	
	cinfo.context = wss;
	cinfo.ssl_connection = 0;
	cinfo.host = host;
	cinfo.address = host;
	cinfo.port = wsport;
	cinfo.path = path;
	cinfo.protocol = "ps-protocol"; //wss_protocols.name;
	
	GError * error = NULL;
	if (wss != NULL) {
		/* Connect to the server */
		wsi = lws_client_connect_via_info (&cinfo);
		/*TODO: somehow, check if the connection is established?*/
		wss_thread = g_thread_try_new("websockets thread", &ps_websockets_thread, wss, &error);
		if (!wss_thread) {
			g_atomic_int_set(&initialized, 0);
			PS_LOG (LOG_ERR,"Got error %d (%s) trying to launch the wss thread...\n", 
			error->code, error->message ? error->message : "??");
			return -1;
		}
	}
	
	/* done */
	g_atomic_int_set(&initialized, 1);
	return 0;
}

void ps_websockets_destroy (void) {
	if (!g_atomic_int_get(&initialized)) return;
	
	g_atomic_int_set(&stopping, 1);
	
	if (wss_thread != NULL) {
		g_thread_join(wss_thread);
		wss_thread = NULL;
	}
	
	if (wss !=NULL) {
		lws_context_destroy (wss);
		wss = NULL;
	}
	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	PS_LOG (LOG_INFO,"Websocket destroyed\n");
}

int ps_websockets_send_message (void * transport, json_t * message) {
	if (message == NULL) return -1;
	
	if (transport == NULL) {
		g_free(message);
		return -1;
	}
	
	ps_websockets_client * client = (ps_websockets_client *)transport;
	
	ps_mutex_lock(&client->mutex);
	char * payload = json_dumps (message, JSON_INDENT(3) | JSON_PRESERVE_ORDER);
	g_async_queue_push (client->messages, payload);
	
	lws_callback_on_writable (client->wsi);
	ps_mutex_unlock (&client->mutex);
	json_decref (message);
	
	return 0;
}

void ps_websockets_session_created (void * transport, guint64 session_id) {
	//session created
}

void ps_websockets_session_over (void * transport, guint64 session_id, gboolean timeout){
	if (transport == NULL || !timeout) return;
	ps_websockets_client * client = (ps_websockets_client * )transport;
	ps_mutex_lock (&client->mutex);
	client->session_timeout = 1;
	lws_callback_on_writable (client->wsi);
	ps_mutex_unlock (&client->mutex);
}

void * ps_websockets_thread (void * data) {
	struct lws_context * service = (struct lws_context *)data;
	if (service == NULL) {
		PS_LOG (LOG_ERR,"Invalid service\n");
		return NULL;
	}
	
	while (g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		lws_service (service, 50);
	}
	
	//lws_context_destroy(service);
	PS_LOG (LOG_INFO,"ps_websockets_thread: thread ended\n");
	return NULL;	
}

static int ps_websockets_callback (
	struct lws * wsi,
	enum lws_callback_reasons reason,
	void * user, void * in, size_t len) 
{
	ps_websockets_client * wsclient = (ps_websockets_client *) user;
	
	switch (reason) {
		case LWS_CALLBACK_CLIENT_ESTABLISHED: {
			wsclient->wsi = wsi;
			wsclient->messages = g_async_queue_new ();
			wsclient->buffer = NULL;
			wsclient->buflen = 0;
			wsclient->bufpending = 0;
			wsclient->bufoffset = 0;
			wsclient->session_timeout = 0;
			wsclient->destroy = 0;
			ps_mutex_init(&wsclient->mutex);
			
			lws_callback_on_writable(wsi);
			PS_LOG (LOG_INFO,"LWS_CALLBACK_CLIENT_ESTABLISHED\n");
			return 0;
		}

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
			PS_LOG (LOG_ERR,"LWS_CALLBACK_CLIENT_CONNECTION_ERROR\n");
			return 0;
		}

        case LWS_CALLBACK_CLOSED: {
			
			PS_LOG (LOG_INFO,"LWS_CALLBACK_CLOSED: Websocket connection closed \n");
			if (wsclient != NULL) {
				gateway->transport_gone (&ps_websockets_transport, wsclient);
				
				ps_mutex_lock (&wsclient->mutex);
				wsclient->destroy = 1;
				wsclient->wsi = NULL;
				
				/* Remove message queue */
				if (wsclient->messages != NULL) {
					char * response = NULL;
					while ((response = g_async_queue_try_pop(wsclient->messages)) != NULL ) {
						g_free (response);
					}
					g_async_queue_unref (wsclient->messages);
				}
				g_free (wsclient->incoming);
				wsclient->incoming = NULL;
				g_free (wsclient->buffer);
				wsclient->buffer = NULL;
				wsclient->buflen = 0;
				wsclient->bufpending = 0;
				wsclient->bufoffset = 0;
				
				ps_mutex_unlock(&wsclient->mutex);
			}
			return 0;
		}

        case LWS_CALLBACK_CLIENT_RECEIVE: {
			if (wsclient == NULL || wsclient->wsi == NULL) {
				PS_LOG (LOG_ERR,"LWS_CALLBACK_CLIENT_RECEIVE: Invalid websocket client instance\n");
				return -1;
			}
			
			const size_t remaining = lws_remaining_packet_payload (wsi);
			
			if (wsclient->incoming == NULL) {
				wsclient->incoming = g_malloc0 (len+1);
				memcpy(wsclient->incoming, in, len);
				wsclient->incoming[len] = '\0';
				PS_LOG (LOG_VERB,"LWS_CALLBACK_CLIENT_RECEIVE: %s\n",wsclient->incoming);
			} else {
				size_t offset = strlen (wsclient->incoming);
				wsclient->incoming = g_realloc (wsclient->incoming, offset+len+1);
				wsclient->incoming[offset+len] = '\0';
				PS_LOG (LOG_VERB,"LWS_CALLBACK_CLIENT_RECEIVE: %s\n",wsclient->incoming+offset);
			}
			
			if (remaining > 0 || !lws_is_final_fragment (wsi)) {
				PS_LOG (LOG_VERB,"LWS_CALLBACK_CLIENT_RECEIVE: Waiting for more fragments\n");
				return 0;
			}
			
			json_error_t error;
			json_t * root = json_loads (wsclient->incoming, 0, &error);
			g_free (wsclient->incoming);
			wsclient->incoming = NULL;
			
			gateway->incoming_request(&ps_websockets_transport, wsclient, root, &error);
			return 0;
		}
            
        case LWS_CALLBACK_CLIENT_WRITEABLE : {
			if (wsclient == NULL || wsclient->wsi == NULL) {
				PS_LOG (LOG_ERR,"LWS_CALLBACK_CLIENT_WRITEABLE: Invalid websocket client instance\n");
				return -1;
			}
			if (!wsclient->destroy && !g_atomic_int_get(&stopping)) {
				
				ps_mutex_lock (&wsclient->mutex);
				
				/* Check if a pending/partial write to complete first */
				if (wsclient->buffer && wsclient->bufpending > 0 && wsclient->bufoffset > 0 && !wsclient->destroy && !g_atomic_int_get(&stopping)) {
					
					int sent = lws_write (wsi, wsclient->buffer + wsclient->bufoffset,
					wsclient->bufpending, LWS_WRITE_TEXT);
					
					if (sent > -1 && sent < wsclient->bufpending) {
						wsclient->bufpending -= sent;
						wsclient->bufoffset += sent;
					} else {
						wsclient->bufpending = 0;
						wsclient->bufoffset = 0;
					}
					
					lws_callback_on_writable (wsi);
					ps_mutex_unlock(&wsclient->mutex);
					return 0;
				}
				
				/* shoot all pending messages */
				char * response = g_async_queue_try_pop (wsclient->messages);
				
				if (response && !wsclient->destroy && !g_atomic_int_get(&stopping)) {
					
					int buflen = LWS_SEND_BUFFER_PRE_PADDING + strlen(response) + LWS_SEND_BUFFER_POST_PADDING;
					
					if (wsclient->buffer == NULL) {
						wsclient->buflen = buflen;
						wsclient->buffer = g_malloc0 (buflen);
					} else if (buflen > wsclient->buflen) {
						wsclient->buflen = buflen;
						wsclient->buffer = g_realloc (wsclient->buffer, buflen);
					}
					memcpy(wsclient->buffer + LWS_SEND_BUFFER_PRE_PADDING, response, strlen(response));
					int sent = lws_write (wsi, wsclient->buffer + LWS_SEND_BUFFER_PRE_PADDING, strlen(response), LWS_WRITE_TEXT);
					
					if (sent > -1 && sent < (int)strlen(response)) {
						wsclient->bufpending = strlen(response) - sent;
						wsclient->bufoffset = LWS_SEND_BUFFER_PRE_PADDING + sent;
					}
					
					g_free (response);
					lws_callback_on_writable (wsi);
					ps_mutex_unlock(&wsclient->mutex);
					return 0;
				}
				ps_mutex_unlock(&wsclient->mutex);
			}
			return 0;
			
		}

        default:
            break;
	}
	
	return 0;
}










