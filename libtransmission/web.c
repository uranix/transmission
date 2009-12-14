/*
 * This file Copyright (C) 2008-2009 Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#include <curl/curl.h>
#include <event.h>

#include "transmission.h"
#include "net.h"
#include "session.h"
#include "trevent.h"
#include "utils.h"
#include "version.h"
#include "web.h"

enum
{
    DEFAULT_TIMER_MSEC = 1500 /* arbitrary */
};

static tr_bool tr_multi_perform( tr_web * g, int fd );

#if 0
#define dbgmsg(...) \
    do { \
        fprintf( stderr, __VA_ARGS__ ); \
        fprintf( stderr, "\n" ); \
    } while( 0 )
#else
#define dbgmsg( ... ) \
    do { \
        if( tr_deepLoggingIsActive( ) ) \
            tr_deepLog( __FILE__, __LINE__, "web", __VA_ARGS__ ); \
    } while( 0 )
#endif

struct tr_web
{
    tr_bool closing;
    int taskCount;
    long timer_msec;
    CURLM * multi;
    tr_session * session;
    tr_bool haveAddr;
    tr_address addr;
    struct event timer_event;
};

static void
web_close( tr_web * g )
{
    curl_multi_cleanup( g->multi );
    evtimer_del( &g->timer_event );
    tr_free( g );
}

/***
****
***/

struct tr_web_task
{
    unsigned long tag;
    struct evbuffer * response;
    char * url;
    char * range;
    tr_session * session;
    tr_web_done_func * done_func;
    void * done_func_user_data;
};

static void
task_free( struct tr_web_task * task )
{
    evbuffer_free( task->response );
    tr_free( task->range );
    tr_free( task->url );
    tr_free( task );
}

/***
****
***/

static size_t
writeFunc( void * ptr, size_t size, size_t nmemb, void * vtask )
{
    const size_t byteCount = size * nmemb;
    struct tr_web_task * task = vtask;
    evbuffer_add( task->response, ptr, byteCount );
    dbgmsg( "wrote %zu bytes to task %p's buffer", byteCount, task );
    return byteCount;
}

static void
sockoptfunction( void * vtask, curl_socket_t fd, curlsocktype purpose UNUSED )
{
    struct tr_web_task * task = vtask;
    const tr_bool isScrape = strstr( task->url, "scrape" ) != NULL;
    const tr_bool isAnnounce = strstr( task->url, "announce" ) != NULL;

    /* announce and scrape requests have tiny payloads... 
     * which have very small payloads */
    if( isScrape || isAnnounce )
    {
        const int sndbuf = 1024;
        const int rcvbuf = isScrape ? 2048 : 3072;
        setsockopt( fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf) );
        setsockopt( fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf) );
    }
}

static int
getCurlProxyType( tr_proxy_type t )
{
    if( t == TR_PROXY_SOCKS4 ) return CURLPROXY_SOCKS4;
    if( t == TR_PROXY_SOCKS5 ) return CURLPROXY_SOCKS5;
    return CURLPROXY_HTTP;
}

static int
getTimeoutFromURL( const char * url )
{
    if( strstr( url, "scrape" ) != NULL ) return 20;
    if( strstr( url, "announce" ) != NULL ) return 30;
    return 240;
}

static void
addTask( void * vtask )
{
    struct tr_web_task * task = vtask;
    const tr_session * session = task->session;

    if( session && session->web )
    {
        CURLMcode mcode;
        CURL * e = curl_easy_init( );
        struct tr_web * web = session->web;
        const long timeout = getTimeoutFromURL( task->url );
        const long verbose = getenv( "TR_CURL_VERBOSE" ) != NULL;
        const char * user_agent = TR_NAME "/" LONG_VERSION_STRING;

        dbgmsg( "adding task #%lu [%s]", task->tag, task->url );

        if( !task->range && session->isProxyEnabled ) {
            curl_easy_setopt( e, CURLOPT_PROXY, session->proxy );
            curl_easy_setopt( e, CURLOPT_PROXYAUTH, CURLAUTH_ANY );
            curl_easy_setopt( e, CURLOPT_PROXYPORT, session->proxyPort );
            curl_easy_setopt( e, CURLOPT_PROXYTYPE,
                                      getCurlProxyType( session->proxyType ) );
        }
        if( !task->range && session->isProxyAuthEnabled ) {
            char * str = tr_strdup_printf( "%s:%s", session->proxyUsername,
                                                    session->proxyPassword );
            curl_easy_setopt( e, CURLOPT_PROXYUSERPWD, str );
            tr_free( str );
        }

        curl_easy_setopt( e, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4 );
        curl_easy_setopt( e, CURLOPT_TIMEOUT, timeout );
        curl_easy_setopt( e, CURLOPT_CONNECTTIMEOUT, timeout-5 );
        curl_easy_setopt( e, CURLOPT_SOCKOPTFUNCTION, sockoptfunction );
        curl_easy_setopt( e, CURLOPT_SOCKOPTDATA, task );
        curl_easy_setopt( e, CURLOPT_WRITEDATA, task );
        curl_easy_setopt( e, CURLOPT_WRITEFUNCTION, writeFunc );
        curl_easy_setopt( e, CURLOPT_DNS_CACHE_TIMEOUT, 1800L );
        curl_easy_setopt( e, CURLOPT_FOLLOWLOCATION, 1L );
        curl_easy_setopt( e, CURLOPT_AUTOREFERER, 1L );
        curl_easy_setopt( e, CURLOPT_FORBID_REUSE, 1L );
        curl_easy_setopt( e, CURLOPT_MAXREDIRS, -1L );
        curl_easy_setopt( e, CURLOPT_NOSIGNAL, 1L );
        curl_easy_setopt( e, CURLOPT_PRIVATE, task );
        curl_easy_setopt( e, CURLOPT_SSL_VERIFYHOST, 0L );
        curl_easy_setopt( e, CURLOPT_SSL_VERIFYPEER, 0L );
        curl_easy_setopt( e, CURLOPT_URL, task->url );
        curl_easy_setopt( e, CURLOPT_USERAGENT, user_agent );
        curl_easy_setopt( e, CURLOPT_VERBOSE, verbose );
        if( web->haveAddr )
            curl_easy_setopt( e, CURLOPT_INTERFACE, tr_ntop_non_ts( &web->addr ) );
        if( task->range )
            curl_easy_setopt( e, CURLOPT_RANGE, task->range );
        else /* don't set encoding on webseeds; it messes up binary data */
            curl_easy_setopt( e, CURLOPT_ENCODING, "" );

        mcode = curl_multi_add_handle( web->multi, e );
        ++web->taskCount;
        /*tr_multi_perform( web, CURL_SOCKET_TIMEOUT );*/
    }
}

/***
****
***/

static void
task_finish( struct tr_web_task * task, long response_code )
{
    dbgmsg( "finished web task %lu; got %ld", task->tag, response_code );

    if( task->done_func != NULL )
        task->done_func( task->session,
                         response_code,
                         EVBUFFER_DATA( task->response ),
                         EVBUFFER_LENGTH( task->response ),
                         task->done_func_user_data );
    task_free( task );
}

static void
remove_finished_tasks( tr_web * g )
{
    CURLMsg * msg;
    int msgs_left;

    while(( msg = curl_multi_info_read( g->multi, &msgs_left ))) {
        if(( msg->msg == CURLMSG_DONE ) && ( msg->easy_handle != NULL )) {
            long code;
            struct tr_web_task * task;
            CURL * e = msg->easy_handle;
            curl_easy_getinfo( e, CURLINFO_PRIVATE, (void*)&task );
            curl_easy_getinfo( e, CURLINFO_RESPONSE_CODE, &code );
            curl_multi_remove_handle( g->multi, e );
            curl_easy_cleanup( e );
            task_finish( task, code );
        }
    }
}

static void
restart_timer( tr_web * g )
{
    dbgmsg( "adding a timeout for %.1f seconds from now", g->timer_msec/1000.0 );
    evtimer_del( &g->timer_event );
    tr_timerAddMsec( &g->timer_event, g->timer_msec );
}

static tr_bool
tr_multi_perform( tr_web * g, int fd )
{
    CURLMcode mcode;
    tr_bool closed = FALSE;

    dbgmsg( "check_run_count: %d taskCount", g->taskCount );

    /* invoke libcurl's processing */
    do {
        mcode = curl_multi_socket_action( g->multi, fd, 0, &g->taskCount );
    } while( mcode == CURLM_CALL_MULTI_SOCKET );

    remove_finished_tasks( g );

    if(( closed = g->closing && !g->taskCount ))
        web_close( g );
    else
        restart_timer( g );

    return closed;
}

/* libevent says that sock is ready to be processed, so wake up libcurl */
static void
event_cb( int fd, short kind UNUSED, void * g )
{
    tr_multi_perform( g, fd );
}

/* CURLMOPT_SOCKETFUNCTION */
static int
sock_cb( CURL * easy UNUSED, curl_socket_t fd, int action,
         void * vweb, void * vevent )
{
    /*static int num_events = 0;*/
    struct tr_web * web = vweb;
    struct event * io_event = vevent;
    dbgmsg( "sock_cb: action is %d, fd is %d, io_event is %p",
            action, (int)fd, io_event );

    if( action == CURL_POLL_REMOVE )
    {
        if( io_event != NULL )
        {
            event_del( io_event );
            tr_free( io_event );
            curl_multi_assign( web->multi, fd, NULL );
            /*fprintf( stderr, "-1 io_events to %d\n", --num_events );*/
        }
    }
    else if( action & ( EV_READ | EV_WRITE ) )
    {
        const short events = EV_PERSIST
                           | (( action & CURL_POLL_IN ) ? EV_READ : 0 )
                           | (( action & CURL_POLL_OUT ) ? EV_WRITE : 0 );

        if( io_event != NULL )
            event_del( io_event );
        else {
            io_event = tr_new0( struct event, 1 );
            curl_multi_assign( web->multi, fd, io_event );
            /*fprintf( stderr, "+1 io_events to %d\n", ++num_events );*/
        }

        dbgmsg( "enabling (libevent %hd, libcurl %d) polling on io_event %p, fd %d",
                events, action, io_event, fd );
        event_set( io_event, fd, events, event_cb, web );
        event_add( io_event, NULL );
    }

    return 0; /* libcurl documentation: "The callback MUST return 0." */
}

/* libevent says that timer_msec have passed, so wake up libcurl */
static void
libevent_timer_cb( int fd UNUSED, short what UNUSED, void * g )
{
    dbgmsg( "libevent timer is done" );
    tr_multi_perform( g, CURL_SOCKET_TIMEOUT );
}

/* libcurl documentation: "If 0, it means you should proceed immediately
 * without waiting for anything. If it returns -1, there's no timeout at all
 * set ... (but) you must not wait too long (more than a few seconds perhaps)
 * before you call curl_multi_perform() again."  */
static void
multi_timer_cb( CURLM * multi UNUSED, long timer_msec, void * vg )
{
    tr_web * g = vg;
    tr_bool closed = FALSE;

    if( timer_msec < 1 ) {
        if( timer_msec == 0 ) /* call it immediately */
            closed = tr_multi_perform( g, CURL_SOCKET_TIMEOUT );
        timer_msec = DEFAULT_TIMER_MSEC;
    }

    if( !closed ) {
        g->timer_msec = timer_msec;
        restart_timer( g );
    }
}

/****
*****
****/

void
tr_webRun( tr_session         * session,
           const char         * url,
           const char         * range,
           tr_web_done_func     done_func,
           void               * done_func_user_data )
{
    if( session->web != NULL )
    {
        static unsigned long tag = 0;
        struct tr_web_task * task = tr_new0( struct tr_web_task, 1 );
        task->session = session;
        task->url = tr_strdup( url );
        task->range = tr_strdup( range );
        task->done_func = done_func;
        task->done_func_user_data = done_func_user_data;
        task->tag = ++tag;
        task->response = evbuffer_new( );

        tr_runInEventThread( session, addTask, task );
    }
}

void
tr_webSetInterface( tr_web * web, const tr_address * addr )
{
    if(( web->haveAddr = ( addr != NULL )))
        web->addr = *addr;
}

tr_web*
tr_webInit( tr_session * session )
{
    tr_web * web;

    /* try to enable ssl for https support; but if that fails,
     * try a plain vanilla init */
    if( curl_global_init( CURL_GLOBAL_SSL ) )
        curl_global_init( 0 );

    web = tr_new0( struct tr_web, 1 );
    web->session = session;
    web->timer_msec = DEFAULT_TIMER_MSEC; /* overwritten by multi_timer_cb() */
    evtimer_set( &web->timer_event, libevent_timer_cb, web );

    web->multi = curl_multi_init( );
    curl_multi_setopt( web->multi, CURLMOPT_SOCKETDATA, web );
    curl_multi_setopt( web->multi, CURLMOPT_SOCKETFUNCTION, sock_cb );
    curl_multi_setopt( web->multi, CURLMOPT_TIMERDATA, web );
    curl_multi_setopt( web->multi, CURLMOPT_TIMERFUNCTION, multi_timer_cb );

    return web;
}

void
tr_webClose( tr_web ** web_in )
{
    tr_web * web = *web_in;
    *web_in = NULL;
    if( web->taskCount < 1 )
        web_close( web );
    else
        web->closing = 1;
}

/*****
******
******
*****/

const char *
tr_webGetResponseStr( long code )
{
    switch( code )
    {
        case   0: return "No Response";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 305: return "Use Proxy";
        case 306: return "(Unused)";
        case 307: return "Temporary Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Request Entity Too Large";
        case 414: return "Request-URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Requested Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown Error";
    }
}

void
tr_http_escape( struct evbuffer  * out, const char * str, int len, tr_bool escape_slashes )
{
    int i;

    if( ( len < 0 ) && ( str != NULL ) )
        len = strlen( str );

    for( i = 0; i < len; i++ ) {
        switch( str[i] ) {
        case ',': case '-': case '.':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
        case 'a': case 'b': case 'c': case 'd': case 'e':
        case 'f': case 'g': case 'h': case 'i': case 'j':
        case 'k': case 'l': case 'm': case 'n': case 'o':
        case 'p': case 'q': case 'r': case 's': case 't':
        case 'u': case 'v': case 'w': case 'x': case 'y': case 'z':
        case 'A': case 'B': case 'C': case 'D': case 'E':
        case 'F': case 'G': case 'H': case 'I': case 'J':
        case 'K': case 'L': case 'M': case 'N': case 'O':
        case 'P': case 'Q': case 'R': case 'S': case 'T':
        case 'U': case 'V': case 'W': case 'X': case 'Y': case 'Z':
            evbuffer_add( out, &str[i], 1 );
            break;
        case '/':
            if(!escape_slashes) {
                evbuffer_add( out, &str[i], 1 );
                break;
            }
            /* Fall through. */
        default:
            evbuffer_add_printf( out, "%%%02X", (unsigned)(str[i]&0xFF) );
            break;
        }
    }
}

char *
tr_http_unescape( const char * str, int len )
{
    char * tmp = curl_unescape( str, len );
    char * ret = tr_strdup( tmp );
    curl_free( tmp );
    return ret;
}
