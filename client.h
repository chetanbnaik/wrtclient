#ifndef __CLIENT_H__
#define __CLIENT_H__

#include <glib.h>
#include <gst/gst.h>
#include <gst/sdp/gstsdpmessage.h>

gboolean bus_msg (GstBus * bus, GstMessage * msg, gpointer pipe);

GArray * create_codecs_array (gchar * codecs[]);

#endif
