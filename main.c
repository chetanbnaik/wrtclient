#include "client.h"
#include "wsclient.h"
	
static int websocket_write_back(struct lws *wsi_in, char *str, int str_size_in) 
{
    if (str == NULL || wsi_in == NULL)
        return -1;

    int n;
    int len;
    char *out = NULL;

    if (str_size_in < 1) 
        len = strlen(str);
    else
        len = str_size_in;

    out = (char *)malloc(sizeof(char)*(LWS_SEND_BUFFER_PRE_PADDING + len + LWS_SEND_BUFFER_POST_PADDING));
    //* setup the buffer*/
    memcpy (out + LWS_SEND_BUFFER_PRE_PADDING, str, len );
    //* write out*/
    n = lws_write(wsi_in, out + LWS_SEND_BUFFER_PRE_PADDING, len, LWS_WRITE_TEXT);

    //g_print ("[websocket_write_back] %s\n", str);
    //* free the buffer*/
    free(out);

    return n;
}

static int ws_service_callback(
                         struct lws *wsi,
                         enum lws_callback_reasons reason, void *user,
                         void *in, size_t len)
{

    switch (reason) {

        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            g_print("[Main Service] Connect with server success.\n");
            break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            g_print("[Main Service] Connect with server error.\n");
            break;

        case LWS_CALLBACK_CLOSED:
            g_print("[Main Service] LWS_CALLBACK_CLOSED\n");
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            g_print("[Main Service] Client recvived --> %s\n", (char *)in);
            lws_callback_on_writable (wsi);
            break;
            
        case LWS_CALLBACK_CLIENT_WRITEABLE :
            //g_print("[Main Service] On writeable is called. send byebye message\n");
            websocket_write_back(wsi, "This is libwebsocket client", -1);
            break;

        default:
            break;
    }

    return 0;
}

gint main(int argc, char *argv[])
{
	//LWS declrations
	struct lws_context * wscontext;
	struct lws_context_creation_info info;
	struct lws_client_connect_info cinfo;
	struct lws_protocols protocol;
	char host[] = "localhost";
	int port = 8080;
	char path[] = "/ws";
	struct lws * wsi;
	
	//GStreamer declarations
	GstElement * pipeline, * source, * sink;
	GstBus * bus;
	GstMessage * msg;
	
	GMainContext * context;
	GMainLoop * loop;
	GSource * wssource;
	int id;
	
	GSourceFuncs WSsourceFuncs = {
		wsprepare,
		wscheck,
		wsdispatch,
		NULL
	};
	
	wssource = g_source_new (&WSsourceFuncs, sizeof(GSource));
	context = g_main_context_default ();
	id = g_source_attach (wssource,context);
	
	loop = g_main_loop_new (context,FALSE);
	
	/*GStreamer code*/
	gst_init(&argc, &argv);
	
	source = gst_element_factory_make ("videotestsrc","source");
	sink = gst_element_factory_make ("autovideosink","sink");
	g_object_set (source, "num-buffers", 300, NULL);
	
	pipeline = gst_pipeline_new ("pipeline");
	gst_bin_add_many (GST_BIN (pipeline), source, sink, NULL);
	if (!gst_element_link_many (source, sink, NULL)) {
		g_warning ("failed to link elements\n");
	}
	
	/* Add bus and signals */
	bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
	gst_bus_add_signal_watch (bus);
	g_signal_connect (bus, "message::error", G_CALLBACK (bus_msg), loop);
	g_signal_connect (bus, "message::warning", G_CALLBACK (bus_msg), loop);
	g_signal_connect (bus, "message::eos", G_CALLBACK (bus_msg), loop);
	
	/* LWS code */
	memset(&info, 0, sizeof(info));
	memset(&cinfo, 0, sizeof(cinfo));
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.iface = NULL;
	info.protocols = &protocol;
	info.ssl_cert_filepath = NULL;
	info.ssl_private_key_filepath = NULL;
	info.extensions = NULL;
	info.gid = -1;
	info.uid = -1;
	info.options = 0;
	
	protocol.name = "PSprotocol";
	protocol.callback = &ws_service_callback;
	//protocol.per_session_data_size = sizeof(struct session_data);
	protocol.rx_buffer_size = 0;
	protocol.id = 0;
	protocol.user = NULL;
	
	wscontext = lws_create_context (&info);
	
	cinfo.context = wscontext;
	cinfo.ssl_connection = 0;
	cinfo.host = host;
	cinfo.address = host;
	cinfo.port = port;
	cinfo.path = path;
	cinfo.protocol = protocol.name;
	
	wsi = lws_client_connect_via_info (&cinfo);
	g_source_set_callback (wssource, wscallback, wscontext, NULL);
	
	//gst_element_set_state (pipeline, GST_STATE_PLAYING);
	
	/* run loop */
	g_main_loop_run (loop);
	
	/* Exiting nice and cleanly */
	lws_context_destroy (wscontext);
	
	g_print ("Deleting pipeline...\n");
	g_source_destroy (wssource);
	g_source_unref (wssource);
	
	gst_object_unref (bus);
	gst_element_set_state (pipeline, GST_STATE_NULL);
	
	gst_object_unref (GST_OBJECT (pipeline));
	g_main_loop_unref (loop);
	
	return 0;
}
