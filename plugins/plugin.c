#include "ps_gstreamer.h"
#include "../apierror.h"
#include "../debug.h"

ps_plugin_result * ps_plugin_result_new (ps_plugin_result_type type, const char * content) {
	PS_LOG (LOG_HUGE, "Creating plugin result...\n");
	ps_plugin_result * result = (ps_plugin_result *)g_malloc0(sizeof(ps_plugin_result));
	if (result == NULL) return NULL;
	
	result->type = type;
	result->content = content ? g_strdup(content) : NULL;
	return result;
}

void ps_plugin_result_destroy (ps_plugin_result * result) {
	PS_LOG (LOG_HUGE, "Destroying plugin result...\n");
	if (result == NULL) return ;
	g_free (result->content);
	g_free (result);
}
