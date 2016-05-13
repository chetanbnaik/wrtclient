#ifndef __PS_WEBSOCKETS_H__
#define __PS_WEBSOCKETS_H__

#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <glib.h>
#include <jansson.h>

#define PS_TRANSPORT_INIT(...) {	\
	.init = NULL,					\
	.destroy = NULL,				\
	.send_message = NULL,			\
	.session_created = NULL,		\
	.session_over = NULL,			\
	## __VA_ARGS__ }
	
typedef struct ps_transport_callbacks ps_transport_callbacks;
typedef struct ps_transport ps_transport;

struct ps_transport {
	int (* const init)(ps_transport_callbacks * callback);
	void (* const destroy)(void);
	int (* const send_message)(void * transport, json_t * message);
	void (* const session_created)(void * transport, guint64 session_id);
	void (* const session_over)(void * transport, guint64 session_id, gboolean timeout);
};

struct ps_transport_callbacks {
	void (* const incoming_request)(ps_transport * plugin, void * transport, json_t * message, json_error_t * error);
	void (* const transport_gone)(ps_transport * plugin, void * transport);
};

typedef ps_transport* create_t(void);

#endif // __PS_WEBSOCKETS_H__
