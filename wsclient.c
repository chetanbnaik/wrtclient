#include "wsclient.h"

gboolean wscallback (gpointer data){
	lws_service (data, 500);
	return TRUE;
}

gboolean wsprepare (GSource * wssource, gint *timeout_){
	*timeout_ = -1;
	return TRUE;
}

gboolean wscheck (GSource * wssource) { return TRUE; }

gboolean wsdispatch (GSource * wssource, GSourceFunc wscallback, gpointer userdata){
	if (wscallback(userdata))
		return TRUE;
	else
		return FALSE;
}
