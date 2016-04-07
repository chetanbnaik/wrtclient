#include <libwebsockets.h>
#include <lws_config.h>
#include <glib.h>

gboolean wscallback (gpointer data);

gboolean wsprepare (GSource * wssource, gint *timeout_);

gboolean wscheck (GSource * wssource);

gboolean wsdispatch (GSource * wssource, GSourceFunc wscallback, gpointer userdata);

static int websocket_write_back(struct lws *wsi_in, char *str, int str_size_in);

static int ws_service_callback(
                         struct lws *wsi,
                         enum lws_callback_reasons reason, void *user,
                         void *in, size_t len);
