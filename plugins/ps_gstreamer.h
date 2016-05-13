#ifndef __PS_GSTREAMER_H__
#define __PS_GSTREAMER_H__

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <inttypes.h>

#include <glib.h>
#include <gst/gst.h>

#define PS_PLUGIN_API_VERSION   5

#define PS_PLUGIN_INIT(...) {	\
	.init = NULL,				\
	.destroy = NULL,			\
	.get_api_compatibility = NULL, 	\
	.get_version = NULL,		\
	.get_version_string = NULL,	\
	.get_description = NULL,	\
	.get_name = NULL,			\
	.get_author = NULL,			\
	.get_package = NULL,		\
	.create_session = NULL,		\
	.handle_message = NULL,		\
	.setup_media = NULL,		\
	.incoming_rtp = NULL,		\
	.incoming_rtcp = NULL,		\
	.incoming_data = NULL,		\
	.slow_link = NULL,			\
	.hangup_media = NULL,		\
	.destroy_session = NULL,	\
	.query_session = NULL,		\
	## __VA_ARGS__ }

typedef struct ps_callbacks ps_callbacks;
typedef struct ps_plugin ps_plugin;
typedef struct ps_plugin_session ps_plugin_session;
typedef struct ps_plugin_result ps_plugin_result;

struct ps_plugin_session {
	void * gateway_handle;
	void * plugin_handle;
	int stopped:1;
};

struct ps_plugin {
	int (* const init)(ps_callbacks * callback, const char * config_path);
	void (* const destroy)(void);
	int (* const get_api_compatibility)(void);
	int (*const get_version)(void);
	const char *(* const get_version_string)(void);
	const char *(* const get_description)(void);
	const char *(* const get_name)(void);
	const char *(* const get_author)(void);
	const char *(* const get_package)(void);
	void (* const create_session)(ps_plugin_session * handle, int *error);
	struct ps_plugin_result * (* const handle_message)(ps_plugin_session * handle, char * transaction, char * message, char * sdp_type, char * sdp);
	void (* const setup_media)(ps_plugin_session * handle);
	void (* const incoming_rtp)(ps_plugin_session * handle, int video, char * buf, int len);
	void (* const incoming_rtcp)(ps_plugin_session * handle, int video, char * buf, int len);
	void (* const incoming_data)(ps_plugin_session * handle, char * buf, int len);
	void (* const slow_link)(ps_plugin_session * handle, int uplink, int video);
	void (* const hangup_media)(ps_plugin_session * handle);
	void (* const destroy_session)(ps_plugin_session * handle, int *error);
	char * (* const query_session)(ps_plugin_session * handle);
};

struct ps_callbacks {
	int (* const push_event)(ps_plugin_session * handle, ps_plugin * plugin, const char * transaction, const char * message, const char * sdp_type, const char * sdp);
	void (* const relay_rtp)(ps_plugin_session * handle, int video, char * buf, int len);
	void (* const relay_rtcp)(ps_plugin_session * handle, int video, char * buf, int len);
	void (* const relay_data)(ps_plugin_session * handle, char * buf, int len);
	void (* const close_pc)(ps_plugin_session * handle);
	void (*const end_session)(ps_plugin_session * handle);
};

typedef ps_plugin * create_p(void);

typedef enum ps_plugin_result_type {
	PS_PLUGIN_ERROR = -1,
	PS_PLUGIN_OK,
	PS_PLUGIN_OK_WAIT,
} ps_plugin_result_type;

struct ps_plugin_result {
	ps_plugin_result_type type;
	char * content;
};

ps_plugin_result * ps_plugin_result_new (ps_plugin_result_type type, const char * content);
void ps_plugin_result_destroy (ps_plugin_result * result);

gboolean bus_msg (GstBus * bus, GstMessage * msg, gpointer * pipe);

#endif // __PS_GSTREAMER_H__
