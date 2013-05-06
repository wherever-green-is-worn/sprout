/**
 * @file faketransport_tcp.cpp
 *
 * Copyright (C) 2013  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by post at
 * Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 */

#include "faketransport_tcp.hpp"
#include <pjsip/sip_endpoint.h>
#include <pjsip/sip_errno.h>
#include <pj/compat/socket.h>
#include <pj/addr_resolv.h>
#include <pj/activesock.h>
#include <pj/assert.h>
#include <pj/lock.h>
#include <pj/log.h>
#include <pj/os.h>
#include <pj/pool.h>
#include <pj/string.h>

#define THIS_FILE	"faketransport_tcp.c"

// Use TCP constants.
#define PJSIP_TRANSPORT_FAKE_TCP PJSIP_TRANSPORT_TCP
#define PJSIP_TRANSPORT_FAKE_TCP6 PJSIP_TRANSPORT_TCP6
#define PJSIP_FAKE_TCP_TRANSPORT_BACKLOG PJSIP_TCP_TRANSPORT_BACKLOG
#define PJSIP_FAKE_TCP_KEEP_ALIVE_DATA PJSIP_TCP_KEEP_ALIVE_DATA
#define PJSIP_FAKE_TCP_KEEP_ALIVE_INTERVAL PJSIP_TCP_KEEP_ALIVE_INTERVAL

#define MAX_ASYNC_CNT	16
#define POOL_LIS_INIT	512
#define POOL_LIS_INC	512
#define POOL_TP_INIT	512
#define POOL_TP_INC	512

struct fake_tcp_listener;
struct fake_tcp_transport;


/*
 * This is the FAKE_TCP listener, which is a "descendant" of pjsip_tpfactory (the
 * SIP transport factory).
 */
struct fake_tcp_listener
{
    pjsip_tpfactory	     factory;
    pj_bool_t		     is_registered;
    pjsip_endpoint	    *endpt;
    pjsip_tpmgr		    *tpmgr;
    pj_activesock_t	    *asock;
    pj_sockaddr		     bound_addr;
    pj_qos_type		     qos_type;
    pj_qos_params	     qos_params;
};


/*
 * This structure is used to keep delayed transmit operation in a list.
 * A delayed transmission occurs when application sends tx_data when
 * the FAKE_TCP connect/establishment is still in progress. These delayed
 * transmission will be "flushed" once the socket is connected (either
 * successfully or with errors).
 */
struct delayed_tdata
{
    PJ_DECL_LIST_MEMBER(struct delayed_tdata);
    pjsip_tx_data_op_key    *tdata_op_key;
    pj_time_val              timeout;
};


/*
 * This structure describes the FAKE_TCP transport, and it's descendant of
 * pjsip_transport.
 */
struct fake_tcp_transport
{
    pjsip_transport	     base;
    pj_bool_t		     is_server;

    /* Do not save listener instance in the transport, because
     * listener might be destroyed during transport's lifetime.
     * See http://trac.pjsip.org/repos/ticket/491
    struct fake_tcp_listener	    *listener;
     */

    pj_bool_t		     is_registered;
    pj_bool_t		     is_closing;
    pj_status_t		     close_reason;
    pj_sock_t		     sock;
    pj_activesock_t	    *asock;
    pj_bool_t		     has_pending_connect;

    /* Connect timer. */
    pj_timer_entry           connect_timer;

    /* Keep-alive timer. */
    pj_timer_entry	     ka_timer;
    pj_time_val		     last_activity;
    pjsip_tx_data_op_key     ka_op_key;
    pj_str_t		     ka_pkt;

    /* FAKE_TCP transport can only have  one rdata!
     * Otherwise chunks of incoming PDU may be received on different
     * buffer.
     */
    pjsip_rx_data	     rdata;

    /* Pending transmission list. */
    struct delayed_tdata     delayed_list;
};


/****************************************************************************
 * PROTOTYPES
 */

/* This callback is called when pending accept() operation completes. */
static pj_bool_t on_accept_complete(pj_activesock_t *asock,
				    pj_sock_t newsock,
				    const pj_sockaddr_t *src_addr,
				    int src_addr_len);

/* This callback is called by transport manager to destroy listener */
static pj_status_t lis_destroy(pjsip_tpfactory *factory);

/* This callback is called by transport manager to create transport */
static pj_status_t lis_create_transport(pjsip_tpfactory *factory,
					pjsip_tpmgr *mgr,
					pjsip_endpoint *endpt,
					const pj_sockaddr *rem_addr,
					int addr_len,
					pjsip_transport **transport);

/* Common function to create and initialize transport */
static pj_status_t fake_tcp_create(struct fake_tcp_listener *listener,
			      pj_pool_t *pool,
			      pj_sock_t sock, pj_bool_t is_server,
			      const pj_sockaddr *local,
			      const pj_sockaddr *remote,
			      struct fake_tcp_transport **p_fake_tcp);


static void fake_tcp_perror(const char *sender, const char *title,
		       pj_status_t status)
{
    char errmsg[PJ_ERR_MSG_SIZE];

    pj_strerror(status, errmsg, sizeof(errmsg));

    PJ_LOG(1,(sender, "%s: %s [code=%d]", title, errmsg, status));
}


static void sockaddr_to_host_port( pj_pool_t *pool,
				   pjsip_host_port *host_port,
				   const pj_sockaddr *addr )
{
    host_port->host.ptr = (char*) pj_pool_alloc(pool, PJ_INET6_ADDRSTRLEN+4);
    pj_sockaddr_print(addr, host_port->host.ptr, PJ_INET6_ADDRSTRLEN+4, 0);
    host_port->host.slen = pj_ansi_strlen(host_port->host.ptr);
    host_port->port = pj_sockaddr_get_port(addr);
}


void fake_tcp_init_shutdown(struct fake_tcp_transport *fake_tcp, pj_status_t status)
{
    pjsip_tp_state_callback state_cb;

    if (fake_tcp->close_reason == PJ_SUCCESS)
	fake_tcp->close_reason = status;

    if (fake_tcp->base.is_shutdown)
	return;

    /* Prevent immediate transport destroy by application, as transport
     * state notification callback may be stacked and transport instance
     * must remain valid at any point in the callback.
     */
    pjsip_transport_add_ref(&fake_tcp->base);

    /* Notify application of transport disconnected state */
    state_cb = pjsip_tpmgr_get_state_cb(fake_tcp->base.tpmgr);
    if (state_cb) {
	pjsip_transport_state_info state_info;

	pj_bzero(&state_info, sizeof(state_info));
	state_info.status = fake_tcp->close_reason;
	(*state_cb)(&fake_tcp->base, PJSIP_TP_STATE_DISCONNECTED, &state_info);
    }

    /* We can not destroy the transport since high level objects may
     * still keep reference to this transport. So we can only
     * instruct transport manager to gracefully start the shutdown
     * procedure for this transport.
     */
    pjsip_transport_shutdown(&fake_tcp->base);

    /* Now, it is ok to destroy the transport. */
    pjsip_transport_dec_ref(&fake_tcp->base);
}


/*
 * Initialize pjsip_fake_tcp_transport_cfg structure with default values.
 */
PJ_DEF(void) pjsip_fake_tcp_transport_cfg_default(pjsip_fake_tcp_transport_cfg *cfg,
					     int af)
{
    pj_bzero(cfg, sizeof(*cfg));
    cfg->af = af;
    pj_sockaddr_init(cfg->af, &cfg->bind_addr, NULL, 0);
    cfg->async_cnt = 1;
}


/****************************************************************************
 * The FAKE_TCP listener/transport factory.
 */

/*
 * This is the public API to create, initialize, register, and start the
 * FAKE_TCP listener.
 */
PJ_DEF(pj_status_t) pjsip_fake_tcp_transport_start3(
					pjsip_endpoint *endpt,
					const pjsip_fake_tcp_transport_cfg *cfg,
					pjsip_tpfactory **p_factory
					)
{
    pj_pool_t *pool;
    pj_sock_t sock = PJ_INVALID_SOCKET;
    struct fake_tcp_listener *listener;
    pj_activesock_cfg asock_cfg;
    pj_activesock_cb listener_cb;
    pj_sockaddr *listener_addr;
    int addr_len;
    pj_status_t status;

    /* Sanity check */
    PJ_ASSERT_RETURN(endpt && cfg->async_cnt, PJ_EINVAL);

    /* Verify that address given in a_name (if any) is valid */
    if (cfg->addr_name.host.slen) {
	pj_sockaddr tmp;

	status = pj_sockaddr_init(cfg->af, &tmp, &cfg->addr_name.host,
				  (pj_uint16_t)cfg->addr_name.port);
	if (status != PJ_SUCCESS || !pj_sockaddr_has_addr(&tmp) ||
	    (cfg->af==pj_AF_INET() &&
	     tmp.ipv4.sin_addr.s_addr==PJ_INADDR_NONE))
	{
	    /* Invalid address */
	    return PJ_EINVAL;
	}
    }

    pool = pjsip_endpt_create_pool(endpt, "fake_tcplis", POOL_LIS_INIT,
				   POOL_LIS_INC);
    PJ_ASSERT_RETURN(pool, PJ_ENOMEM);


    listener = PJ_POOL_ZALLOC_T(pool, struct fake_tcp_listener);
    listener->factory.pool = pool;
    listener->factory.type = cfg->af==pj_AF_INET() ? PJSIP_TRANSPORT_FAKE_TCP :
						     PJSIP_TRANSPORT_FAKE_TCP6;
    listener->factory.type_name = (char*)
		pjsip_transport_get_type_name(listener->factory.type);
    listener->factory.flag =
	pjsip_transport_get_flag_from_type(listener->factory.type);
    listener->qos_type = cfg->qos_type;
    pj_memcpy(&listener->qos_params, &cfg->qos_params,
	      sizeof(cfg->qos_params));

    pj_ansi_strcpy(listener->factory.obj_name, "fake_tcplis");
    if (listener->factory.type==PJSIP_TRANSPORT_FAKE_TCP6)
	pj_ansi_strcat(listener->factory.obj_name, "6");

    status = pj_lock_create_recursive_mutex(pool, listener->factory.obj_name,
					    &listener->factory.lock);
    if (status != PJ_SUCCESS)
	goto on_error;


    /* Create socket */
    status = pj_sock_socket(cfg->af, pj_SOCK_STREAM(), 0, &sock);
    if (status != PJ_SUCCESS)
	goto on_error;

    /* Apply QoS, if specified */
    status = pj_sock_apply_qos2(sock, cfg->qos_type, &cfg->qos_params,
				2, listener->factory.obj_name,
				"SIP FAKE_TCP listener socket");

    /* Bind address may be different than factory.local_addr because
     * factory.local_addr will be resolved below.
     */
    pj_sockaddr_cp(&listener->bound_addr, &cfg->bind_addr);

    /* Bind socket */
    listener_addr = &listener->factory.local_addr;
    pj_sockaddr_cp(listener_addr, &cfg->bind_addr);

    status = pj_sock_bind(sock, listener_addr,
			  pj_sockaddr_get_len(listener_addr));
    if (status != PJ_SUCCESS)
	goto on_error;

    /* Retrieve the bound address */
    addr_len = pj_sockaddr_get_len(listener_addr);
    status = pj_sock_getsockname(sock, listener_addr, &addr_len);
    if (status != PJ_SUCCESS)
	goto on_error;

    /* If published host/IP is specified, then use that address as the
     * listener advertised address.
     */
    if (cfg->addr_name.host.slen) {
	/* Copy the address */
	listener->factory.addr_name = cfg->addr_name;
	pj_strdup(listener->factory.pool, &listener->factory.addr_name.host,
		  &cfg->addr_name.host);
	listener->factory.addr_name.port = cfg->addr_name.port;

    } else {
	/* No published address is given, use the bound address */

	/* If the address returns 0.0.0.0, use the default
	 * interface address as the transport's address.
	 */
	if (!pj_sockaddr_has_addr(listener_addr)) {
	    pj_sockaddr hostip;

	    status = pj_gethostip(listener->bound_addr.addr.sa_family,
	                          &hostip);
	    if (status != PJ_SUCCESS)
		goto on_error;

	    pj_sockaddr_copy_addr(listener_addr, &hostip);
	}

	/* Save the address name */
	sockaddr_to_host_port(listener->factory.pool,
			      &listener->factory.addr_name,
			      listener_addr);
    }

    /* If port is zero, get the bound port */
    if (listener->factory.addr_name.port == 0) {
	listener->factory.addr_name.port = pj_sockaddr_get_port(listener_addr);
    }

    pj_ansi_snprintf(listener->factory.obj_name,
		     sizeof(listener->factory.obj_name),
		     "fake_tcplis:%d",  listener->factory.addr_name.port);


    /* Start listening to the address */
    status = pj_sock_listen(sock, PJSIP_FAKE_TCP_TRANSPORT_BACKLOG);
    if (status != PJ_SUCCESS)
	goto on_error;


    /* Create active socket */
    pj_activesock_cfg_default(&asock_cfg);
    if (cfg->async_cnt > MAX_ASYNC_CNT)
	asock_cfg.async_cnt = MAX_ASYNC_CNT;
    else
	asock_cfg.async_cnt = cfg->async_cnt;

    pj_bzero(&listener_cb, sizeof(listener_cb));
    listener_cb.on_accept_complete = &on_accept_complete;
    status = pj_activesock_create(pool, sock, pj_SOCK_STREAM(), &asock_cfg,
				  pjsip_endpt_get_ioqueue(endpt),
				  &listener_cb, listener,
				  &listener->asock);

    /* Register to transport manager */
    listener->endpt = endpt;
    listener->tpmgr = pjsip_endpt_get_tpmgr(endpt);
    listener->factory.create_transport = lis_create_transport;
    listener->factory.destroy = lis_destroy;
    listener->is_registered = PJ_TRUE;
    status = pjsip_tpmgr_register_tpfactory(listener->tpmgr,
					    &listener->factory);
    if (status != PJ_SUCCESS) {
	listener->is_registered = PJ_FALSE;
	goto on_error;
    }

    /* Start pending accept() operations */
    status = pj_activesock_start_accept(listener->asock, pool);
    if (status != PJ_SUCCESS)
	goto on_error;

    PJ_LOG(4,(listener->factory.obj_name,
	     "SIP FAKE_TCP listener ready for incoming connections at %.*s:%d",
	     (int)listener->factory.addr_name.host.slen,
	     listener->factory.addr_name.host.ptr,
	     listener->factory.addr_name.port));

    /* Return the pointer to user */
    if (p_factory) *p_factory = &listener->factory;

    return PJ_SUCCESS;

on_error:
    if (listener->asock==NULL && sock!=PJ_INVALID_SOCKET)
	pj_sock_close(sock);
    lis_destroy(&listener->factory);
    return status;
}


/* This callback is called by transport manager to destroy listener */
static pj_status_t lis_destroy(pjsip_tpfactory *factory)
{
    struct fake_tcp_listener *listener = (struct fake_tcp_listener *)factory;

    if (listener->is_registered) {
	pjsip_tpmgr_unregister_tpfactory(listener->tpmgr, &listener->factory);
	listener->is_registered = PJ_FALSE;
    }

    if (listener->asock) {
	pj_activesock_close(listener->asock);
	listener->asock = NULL;
    }

    if (listener->factory.lock) {
	pj_lock_destroy(listener->factory.lock);
	listener->factory.lock = NULL;
    }

    if (listener->factory.pool) {
	pj_pool_t *pool = listener->factory.pool;

	PJ_LOG(4,(listener->factory.obj_name,  "SIP FAKE_TCP listener destroyed"));

	listener->factory.pool = NULL;
	pj_pool_release(pool);
    }

    return PJ_SUCCESS;
}


/***************************************************************************/
/*
 * FAKE_TCP Transport
 */

/*
 * Prototypes.
 */
/* Called by transport manager to send message */
static pj_status_t fake_tcp_send_msg(pjsip_transport *transport,
				pjsip_tx_data *tdata,
				const pj_sockaddr_t *rem_addr,
				int addr_len,
				void *token,
				pjsip_transport_callback callback);

/* Called by transport manager to shutdown */
static pj_status_t fake_tcp_shutdown(pjsip_transport *transport);

/* Called by transport manager to destroy transport */
static pj_status_t fake_tcp_destroy_transport(pjsip_transport *transport);

/* Utility to destroy transport */
static pj_status_t fake_tcp_destroy(pjsip_transport *transport,
			       pj_status_t reason);

/* Callback on incoming data */
static pj_bool_t on_data_read(pj_activesock_t *asock,
			      void *data,
			      pj_size_t size,
			      pj_status_t status,
			      pj_size_t *remainder);

/* Callback when packet is sent */
static pj_bool_t on_data_sent(pj_activesock_t *asock,
			      pj_ioqueue_op_key_t *send_key,
			      pj_ssize_t sent);

/* Callback when connect completes */
static pj_bool_t on_connect_complete(pj_activesock_t *asock,
				     pj_status_t status);

/* FAKE_TCP keep-alive timer callback */
static void fake_tcp_keep_alive_timer(pj_timer_heap_t *th, pj_timer_entry *e);

/* FAKE_TCP connect timer callback */
static void fake_tcp_connect_timer(pj_timer_heap_t *th, pj_timer_entry *e);

/*
 * Common function to create FAKE_TCP transport, called when pending accept() and
 * pending connect() complete.
 */
static pj_status_t fake_tcp_create( struct fake_tcp_listener *listener,
			       pj_pool_t *pool,
			       pj_sock_t sock, pj_bool_t is_server,
			       const pj_sockaddr *local,
			       const pj_sockaddr *remote,
			       struct fake_tcp_transport **p_fake_tcp)
{
    struct fake_tcp_transport *fake_tcp;
    pj_ioqueue_t *ioqueue;
    pj_activesock_cfg asock_cfg;
    pj_activesock_cb fake_tcp_callback;
    const pj_str_t ka_pkt = PJSIP_FAKE_TCP_KEEP_ALIVE_DATA;
    char print_addr[PJ_INET6_ADDRSTRLEN+10];
    pj_status_t status;


    PJ_ASSERT_RETURN(sock != PJ_INVALID_SOCKET, PJ_EINVAL);


    if (pool == NULL) {
	pool = pjsip_endpt_create_pool(listener->endpt, "fake_tcp",
				       POOL_TP_INIT, POOL_TP_INC);
	PJ_ASSERT_RETURN(pool != NULL, PJ_ENOMEM);
    }

    /*
     * Create and initialize basic transport structure.
     */
    fake_tcp = PJ_POOL_ZALLOC_T(pool, struct fake_tcp_transport);
    fake_tcp->is_server = is_server;
    fake_tcp->sock = sock;
    /*fake_tcp->listener = listener;*/
    pj_list_init(&fake_tcp->delayed_list);
    fake_tcp->base.pool = pool;

    pj_ansi_snprintf(fake_tcp->base.obj_name, PJ_MAX_OBJ_NAME,
		     (is_server ? "fake_tcps%p" :"fake_tcpc%p"), fake_tcp);

    status = pj_atomic_create(pool, 0, &fake_tcp->base.ref_cnt);
    if (status != PJ_SUCCESS) {
	goto on_error;
    }

    status = pj_lock_create_recursive_mutex(pool, "fake_tcp", &fake_tcp->base.lock);
    if (status != PJ_SUCCESS) {
	goto on_error;
    }

    fake_tcp->base.key.type = listener->factory.type;
    pj_sockaddr_cp(&fake_tcp->base.key.rem_addr, remote);
    fake_tcp->base.type_name = (char*)
      pjsip_transport_get_type_name((pjsip_transport_type_e)fake_tcp->base.key.type);
    fake_tcp->base.flag = pjsip_transport_get_flag_from_type((pjsip_transport_type_e)fake_tcp->base.key.type);

    fake_tcp->base.info = (char*) pj_pool_alloc(pool, 64);
    pj_ansi_snprintf(fake_tcp->base.info, 64, "%s to %s",
                     fake_tcp->base.type_name,
                     pj_sockaddr_print(remote, print_addr,
                                       sizeof(print_addr), 3));

    fake_tcp->base.addr_len = pj_sockaddr_get_len(remote);
    pj_sockaddr_cp(&fake_tcp->base.local_addr, local);
    sockaddr_to_host_port(pool, &fake_tcp->base.local_name, local);
    sockaddr_to_host_port(pool, &fake_tcp->base.remote_name, remote);
    fake_tcp->base.dir = is_server? PJSIP_TP_DIR_INCOMING : PJSIP_TP_DIR_OUTGOING;

    fake_tcp->base.endpt = listener->endpt;
    fake_tcp->base.tpmgr = listener->tpmgr;
    fake_tcp->base.send_msg = &fake_tcp_send_msg;
    fake_tcp->base.do_shutdown = &fake_tcp_shutdown;
    fake_tcp->base.destroy = &fake_tcp_destroy_transport;

    /* Create active socket */
    pj_activesock_cfg_default(&asock_cfg);
    asock_cfg.async_cnt = 1;

    pj_bzero(&fake_tcp_callback, sizeof(fake_tcp_callback));
    fake_tcp_callback.on_data_read = &on_data_read;
    fake_tcp_callback.on_data_sent = &on_data_sent;
    fake_tcp_callback.on_connect_complete = &on_connect_complete;

    ioqueue = pjsip_endpt_get_ioqueue(listener->endpt);
    status = pj_activesock_create(pool, sock, pj_SOCK_STREAM(), &asock_cfg,
				  ioqueue, &fake_tcp_callback, fake_tcp, &fake_tcp->asock);
    if (status != PJ_SUCCESS) {
	goto on_error;
    }

    /* Register transport to transport manager */
    status = pjsip_transport_register(listener->tpmgr, &fake_tcp->base);
    if (status != PJ_SUCCESS) {
	goto on_error;
    }

    fake_tcp->is_registered = PJ_TRUE;

    /* Initialize connect timer. */
    fake_tcp->connect_timer.user_data = (void*)fake_tcp;
    fake_tcp->connect_timer.cb = &fake_tcp_connect_timer;
    fake_tcp->connect_timer.id = PJ_FALSE;

    /* Initialize keep-alive timer */
    fake_tcp->ka_timer.user_data = (void*)fake_tcp;
    fake_tcp->ka_timer.cb = &fake_tcp_keep_alive_timer;
    pj_ioqueue_op_key_init(&fake_tcp->ka_op_key.key, sizeof(pj_ioqueue_op_key_t));
    pj_strdup(fake_tcp->base.pool, &fake_tcp->ka_pkt, &ka_pkt);

    /* Done setting up basic transport. */
    *p_fake_tcp = fake_tcp;

    PJ_LOG(4,(fake_tcp->base.obj_name, "FAKE_TCP %s transport created",
	      (fake_tcp->is_server ? "server" : "client")));

    return PJ_SUCCESS;

on_error:
    fake_tcp_destroy(&fake_tcp->base, status);
    return status;
}


/* Flush all delayed transmision once the socket is connected. */
static void fake_tcp_flush_pending_tx(struct fake_tcp_transport *fake_tcp)
{
    pj_time_val now;

    pj_gettickcount(&now);
    pj_lock_acquire(fake_tcp->base.lock);
    while (!pj_list_empty(&fake_tcp->delayed_list)) {
	struct delayed_tdata *pending_tx;
	pjsip_tx_data *tdata;
	pj_ioqueue_op_key_t *op_key;
	pj_ssize_t size;
	pj_status_t status;

	pending_tx = fake_tcp->delayed_list.next;
	pj_list_erase(pending_tx);

	tdata = pending_tx->tdata_op_key->tdata;
	op_key = (pj_ioqueue_op_key_t*)pending_tx->tdata_op_key;

        if (pending_tx->timeout.sec > 0 &&
            PJ_TIME_VAL_GT(now, pending_tx->timeout))
        {
            continue;
        }

	/* send! */
	size = tdata->buf.cur - tdata->buf.start;
//	status = pj_activesock_send(fake_tcp->asock, op_key, tdata->buf.start,
//				    &size, 0);
        status = PJ_SUCCESS;  // drop on floor!
	if (status != PJ_EPENDING) {
            pj_lock_release(fake_tcp->base.lock);
	    on_data_sent(fake_tcp->asock, op_key, size);
            pj_lock_acquire(fake_tcp->base.lock);
	}

    }
    pj_lock_release(fake_tcp->base.lock);
}


/* Called by transport manager to destroy transport */
static pj_status_t fake_tcp_destroy_transport(pjsip_transport *transport)
{
    struct fake_tcp_transport *fake_tcp = (struct fake_tcp_transport*)transport;

    /* Transport would have been unregistered by now since this callback
     * is called by transport manager.
     */
    fake_tcp->is_registered = PJ_FALSE;

    return fake_tcp_destroy(transport, fake_tcp->close_reason);
}


/* Destroy FAKE_TCP transport */
static pj_status_t fake_tcp_destroy(pjsip_transport *transport,
			       pj_status_t reason)
{
    struct fake_tcp_transport *fake_tcp = (struct fake_tcp_transport*)transport;

    if (fake_tcp->close_reason == 0)
	fake_tcp->close_reason = reason;

    if (fake_tcp->is_registered) {
	fake_tcp->is_registered = PJ_FALSE;
	pjsip_transport_destroy(transport);

	/* pjsip_transport_destroy will recursively call this function
	 * again.
	 */
	return PJ_SUCCESS;
    }

    /* Mark transport as closing */
    fake_tcp->is_closing = PJ_TRUE;

    /* Stop connect timer if it is still running. */
    if (fake_tcp->connect_timer.id) {
        pjsip_endpt_cancel_timer(fake_tcp->base.endpt, &fake_tcp->connect_timer);
        fake_tcp->connect_timer.id = PJ_FALSE;
    }
    /* Stop keep-alive timer. */
    if (fake_tcp->ka_timer.id) {
	pjsip_endpt_cancel_timer(fake_tcp->base.endpt, &fake_tcp->ka_timer);
	fake_tcp->ka_timer.id = PJ_FALSE;
    }

    /* Cancel all delayed transmits */
    while (!pj_list_empty(&fake_tcp->delayed_list)) {
	struct delayed_tdata *pending_tx;
	pj_ioqueue_op_key_t *op_key;

	pending_tx = fake_tcp->delayed_list.next;
	pj_list_erase(pending_tx);

	op_key = (pj_ioqueue_op_key_t*)pending_tx->tdata_op_key;

	on_data_sent(fake_tcp->asock, op_key, -reason);  // The recipients of these callbacks had better still exist!
    }

    if (fake_tcp->rdata.tp_info.pool) {
	pj_pool_release(fake_tcp->rdata.tp_info.pool);
	fake_tcp->rdata.tp_info.pool = NULL;
    }

    if (fake_tcp->asock) {
	pj_activesock_close(fake_tcp->asock);
	fake_tcp->asock = NULL;
	fake_tcp->sock = PJ_INVALID_SOCKET;
    } else if (fake_tcp->sock != PJ_INVALID_SOCKET) {
	pj_sock_close(fake_tcp->sock);
	fake_tcp->sock = PJ_INVALID_SOCKET;
    }

    if (fake_tcp->base.lock) {
	pj_lock_destroy(fake_tcp->base.lock);
	fake_tcp->base.lock = NULL;
    }

    if (fake_tcp->base.ref_cnt) {
	pj_atomic_destroy(fake_tcp->base.ref_cnt);
	fake_tcp->base.ref_cnt = NULL;
    }

    if (fake_tcp->base.pool) {
	pj_pool_t *pool;

	if (reason != PJ_SUCCESS) {
	    char errmsg[PJ_ERR_MSG_SIZE];

	    pj_strerror(reason, errmsg, sizeof(errmsg));
	    PJ_LOG(4,(fake_tcp->base.obj_name,
		      "FAKE_TCP transport destroyed with reason %d: %s",
		      reason, errmsg));

	} else {

	    PJ_LOG(4,(fake_tcp->base.obj_name,
		      "FAKE_TCP transport destroyed normally"));

	}

	pool = fake_tcp->base.pool;
	fake_tcp->base.pool = NULL;
	pj_pool_release(pool);
    }

    return PJ_SUCCESS;
}


/*
 * This utility function creates receive data buffers and start
 * asynchronous recv() operations from the socket. It is called after
 * accept() or connect() operation complete.
 */
static pj_status_t fake_tcp_start_read(struct fake_tcp_transport *fake_tcp)
{
    pj_pool_t *pool;
    pj_ssize_t size;
    pj_sockaddr *rem_addr;
    void *readbuf[1];
    pj_status_t status;

    /* Init rdata */
    pool = pjsip_endpt_create_pool(fake_tcp->base.endpt,
				   "rtd%p",
				   PJSIP_POOL_RDATA_LEN,
				   PJSIP_POOL_RDATA_INC);
    if (!pool) {
	fake_tcp_perror(fake_tcp->base.obj_name, "Unable to create pool", PJ_ENOMEM);
	return PJ_ENOMEM;
    }

    fake_tcp->rdata.tp_info.pool = pool;

    fake_tcp->rdata.tp_info.transport = &fake_tcp->base;
    fake_tcp->rdata.tp_info.tp_data = fake_tcp;
    fake_tcp->rdata.tp_info.op_key.rdata = &fake_tcp->rdata;
    pj_ioqueue_op_key_init(&fake_tcp->rdata.tp_info.op_key.op_key,
			   sizeof(pj_ioqueue_op_key_t));

    fake_tcp->rdata.pkt_info.src_addr = fake_tcp->base.key.rem_addr;
    fake_tcp->rdata.pkt_info.src_addr_len = sizeof(fake_tcp->rdata.pkt_info.src_addr);
    rem_addr = &fake_tcp->base.key.rem_addr;
    pj_sockaddr_print(rem_addr, fake_tcp->rdata.pkt_info.src_name,
                      sizeof(fake_tcp->rdata.pkt_info.src_name), 0);
    fake_tcp->rdata.pkt_info.src_port = pj_sockaddr_get_port(rem_addr);

    size = sizeof(fake_tcp->rdata.pkt_info.packet);
    readbuf[0] = fake_tcp->rdata.pkt_info.packet;
    status = pj_activesock_start_read2(fake_tcp->asock, fake_tcp->base.pool, size,
				       readbuf, 0);
    if (status != PJ_SUCCESS && status != PJ_EPENDING) {
	PJ_LOG(4, (fake_tcp->base.obj_name,
		   "pj_activesock_start_read() error, status=%d",
		   status));
	return status;
    }

    return PJ_SUCCESS;
}


/* This callback is called by transport manager for the FAKE_TCP factory
 * to create outgoing transport to the specified destination.
 */
static pj_status_t lis_create_transport(pjsip_tpfactory *factory,
					pjsip_tpmgr *mgr,
					pjsip_endpoint *endpt,
					const pj_sockaddr *rem_addr,
					int addr_len,
					pjsip_transport **p_transport)
{
    struct fake_tcp_listener *listener;
    struct fake_tcp_transport *fake_tcp;
    pj_sock_t sock;
    pj_sockaddr local_addr;
    pj_status_t status;

    /* Sanity checks */
    PJ_ASSERT_RETURN(factory && mgr && endpt && rem_addr &&
		     addr_len && p_transport, PJ_EINVAL);

    /* Check that address is a sockaddr_in or sockaddr_in6*/
    PJ_ASSERT_RETURN((rem_addr->addr.sa_family == pj_AF_INET() &&
		      addr_len == sizeof(pj_sockaddr_in)) ||
		     (rem_addr->addr.sa_family == pj_AF_INET6() &&
		      addr_len == sizeof(pj_sockaddr_in6)), PJ_EINVAL);


    listener = (struct fake_tcp_listener*)factory;

    /* Create socket */
    status = pj_sock_socket(rem_addr->addr.sa_family, pj_SOCK_STREAM(),
                            0, &sock);
    if (status != PJ_SUCCESS)
	return status;

    /* Apply QoS, if specified */
    status = pj_sock_apply_qos2(sock, listener->qos_type,
				&listener->qos_params,
				2, listener->factory.obj_name,
				"outgoing SIP FAKE_TCP socket");

    /* Bind to listener's address and any port */
    pj_bzero(&local_addr, sizeof(local_addr));
    pj_sockaddr_cp(&local_addr, &listener->bound_addr);
    pj_sockaddr_set_port(&local_addr, 0);

    status = pj_sock_bind(sock, &local_addr,
                          pj_sockaddr_get_len(&local_addr));
    if (status != PJ_SUCCESS) {
	pj_sock_close(sock);
	return status;
    }

    /* Get the local port */
    addr_len = sizeof(local_addr);
    status = pj_sock_getsockname(sock, &local_addr, &addr_len);
    if (status != PJ_SUCCESS) {
	pj_sock_close(sock);
	return status;
    }

    /* Initially set the address from the listener's address */
    if (!pj_sockaddr_has_addr(&local_addr)) {
	pj_sockaddr_copy_addr(&local_addr, &listener->factory.local_addr);
    }

    /* Create the transport descriptor */
    status = fake_tcp_create(listener, NULL, sock, PJ_FALSE, &local_addr,
			rem_addr, &fake_tcp);
    if (status != PJ_SUCCESS)
	return status;


    /* Start asynchronous connect() operation */
    fake_tcp->has_pending_connect = PJ_TRUE;
    // status = pj_activesock_start_connect(fake_tcp->asock, fake_tcp->base.pool, rem_addr,
    //					 addr_len);
    // Start a zero duration timer to drive the on_connect_complete processing.
    // This has to be async because otherwise event state listeners will miss the
    // connected state transition.  It has to be zero duration so the timer
    // is scheduled on the next call to pjsip_endpt_handle_events.
    pj_time_val delay = {0, 0};
    status = pjsip_endpt_schedule_timer(listener->endpt,
                                        &fake_tcp->connect_timer,
                                        &delay);
    fake_tcp->connect_timer.id = PJ_TRUE;
    status = PJ_EPENDING;

    if (status == PJ_SUCCESS) {
	on_connect_complete(fake_tcp->asock, PJ_SUCCESS);
    } else if (status != PJ_EPENDING) {
	fake_tcp_destroy(&fake_tcp->base, status);
	return status;
    }

    if (fake_tcp->has_pending_connect) {
	/* Update (again) local address, just in case local address currently
	 * set is different now that asynchronous connect() is started.
	 */
	addr_len = sizeof(local_addr);
	if (pj_sock_getsockname(sock, &local_addr, &addr_len)==PJ_SUCCESS) {
	    pj_sockaddr *tp_addr = &fake_tcp->base.local_addr;

	    /* Some systems (like old Win32 perhaps) may not set local address
	     * properly before socket is fully connected.
	     */
	    if (pj_sockaddr_cmp(tp_addr, &local_addr) &&
		pj_sockaddr_get_port(&local_addr) != 0)
	    {
		pj_sockaddr_cp(tp_addr, &local_addr);
		sockaddr_to_host_port(fake_tcp->base.pool, &fake_tcp->base.local_name,
				      &local_addr);
	    }
	}

	PJ_LOG(4,(fake_tcp->base.obj_name,
		  "FAKE_TCP transport %.*s:%d is connecting to %.*s:%d...",
		  (int)fake_tcp->base.local_name.host.slen,
		  fake_tcp->base.local_name.host.ptr,
		  fake_tcp->base.local_name.port,
		  (int)fake_tcp->base.remote_name.host.slen,
		  fake_tcp->base.remote_name.host.ptr,
		  fake_tcp->base.remote_name.port));
    }

    /* Done */
    *p_transport = &fake_tcp->base;

    return PJ_SUCCESS;
}


/*
 * This callback is called by active socket when pending accept() operation
 * has completed.
 */
static pj_bool_t on_accept_complete(pj_activesock_t *asock,
				    pj_sock_t sock,
				    const pj_sockaddr_t *src_addr,
				    int src_addr_len)
{
    struct fake_tcp_listener *listener;
    struct fake_tcp_transport *fake_tcp;
    char addr[PJ_INET6_ADDRSTRLEN+10];
    pjsip_tp_state_callback state_cb;
    pj_sockaddr tmp_src_addr;
    pj_status_t status;

    PJ_UNUSED_ARG(src_addr_len);

    listener = (struct fake_tcp_listener*) pj_activesock_get_user_data(asock);

    PJ_ASSERT_RETURN(sock != PJ_INVALID_SOCKET, PJ_TRUE);

    PJ_LOG(4,(listener->factory.obj_name,
	      "FAKE_TCP listener %.*s:%d: got incoming FAKE_TCP connection "
	      "from %s, sock=%d",
	      (int)listener->factory.addr_name.host.slen,
	      listener->factory.addr_name.host.ptr,
	      listener->factory.addr_name.port,
	      pj_sockaddr_print(src_addr, addr, sizeof(addr), 3),
	      sock));

    /* Apply QoS, if specified */
    status = pj_sock_apply_qos2(sock, listener->qos_type,
				&listener->qos_params,
				2, listener->factory.obj_name,
				"incoming SIP FAKE_TCP socket");

    /* fake_tcp_create() expect pj_sockaddr, so copy src_addr to temporary var,
     * just in case.
     */
    pj_bzero(&tmp_src_addr, sizeof(tmp_src_addr));
    pj_sockaddr_cp(&tmp_src_addr, src_addr);

    /*
     * Incoming connection!
     * Create FAKE_TCP transport for the new socket.
     */
    status = fake_tcp_create( listener, NULL, sock, PJ_TRUE,
			 &listener->factory.local_addr,
			 &tmp_src_addr, &fake_tcp);
    if (status == PJ_SUCCESS) {

        /* Add a reference to prevent the transport from being destroyed while
         * we're operating on it.
         */
        pjsip_transport_add_ref(&fake_tcp->base);

	status = fake_tcp_start_read(fake_tcp);
	if (status != PJ_SUCCESS) {
	    PJ_LOG(3,(fake_tcp->base.obj_name, "New transport cancelled"));
            pjsip_transport_dec_ref(&fake_tcp->base);
	    fake_tcp_destroy(&fake_tcp->base, status);
	} else {
	    /* Start keep-alive timer */
	    if (PJSIP_FAKE_TCP_KEEP_ALIVE_INTERVAL) {
		pj_time_val delay = {PJSIP_FAKE_TCP_KEEP_ALIVE_INTERVAL, 0};
		pjsip_endpt_schedule_timer(listener->endpt,
					   &fake_tcp->ka_timer,
					   &delay);
		fake_tcp->ka_timer.id = PJ_TRUE;
		pj_gettimeofday(&fake_tcp->last_activity);
	    }

	    /* Notify application of transport state accepted */
	    state_cb = pjsip_tpmgr_get_state_cb(fake_tcp->base.tpmgr);
	    if (state_cb) {
		pjsip_transport_state_info state_info;

		pj_bzero(&state_info, sizeof(state_info));
		(*state_cb)(&fake_tcp->base, PJSIP_TP_STATE_CONNECTED, &state_info);
	    }
            pjsip_transport_dec_ref(&fake_tcp->base);
	}
    }

    return PJ_TRUE;
}


/*
 * Callback from ioqueue when packet is sent.
 */
static pj_bool_t on_data_sent(pj_activesock_t *asock,
			      pj_ioqueue_op_key_t *op_key,
			      pj_ssize_t bytes_sent)
{
    struct fake_tcp_transport *fake_tcp = (struct fake_tcp_transport*)
    				pj_activesock_get_user_data(asock);
    pjsip_tx_data_op_key *tdata_op_key = (pjsip_tx_data_op_key*)op_key;

    /* Note that op_key may be the op_key from keep-alive, thus
     * it will not have tdata etc.
     */

    tdata_op_key->tdata = NULL;

    if (tdata_op_key->callback) {
	/*
	 * Notify sip_transport.c that packet has been sent.
	 */
	if (bytes_sent == 0)
	    bytes_sent = -PJ_RETURN_OS_ERROR(OSERR_ENOTCONN);

	tdata_op_key->callback(&fake_tcp->base, tdata_op_key->token, bytes_sent);

	/* Mark last activity time */
	pj_gettimeofday(&fake_tcp->last_activity);

    }

    /* Check for error/closure */
    if (bytes_sent <= 0) {
	pj_status_t status;

	PJ_LOG(5,(fake_tcp->base.obj_name, "FAKE_TCP send() error, sent=%d",
		  bytes_sent));

	status = (bytes_sent == 0) ? PJ_RETURN_OS_ERROR(OSERR_ENOTCONN) :
				     -bytes_sent;

	fake_tcp_init_shutdown(fake_tcp, status);

	return PJ_FALSE;
    }

    return PJ_TRUE;
}


/*
 * This callback is called by transport manager to send SIP message
 */
static pj_status_t fake_tcp_send_msg(pjsip_transport *transport,
				pjsip_tx_data *tdata,
				const pj_sockaddr_t *rem_addr,
				int addr_len,
				void *token,
				pjsip_transport_callback callback)
{
    struct fake_tcp_transport *fake_tcp = (struct fake_tcp_transport*)transport;
    pj_ssize_t size;
    pj_bool_t delayed = PJ_FALSE;
    pj_status_t status = PJ_SUCCESS;

    /* Sanity check */
    PJ_ASSERT_RETURN(transport && tdata, PJ_EINVAL);

    /* Check that there's no pending operation associated with the tdata */
    PJ_ASSERT_RETURN(tdata->op_key.tdata == NULL, PJSIP_EPENDINGTX);

    /* Check the address is supported */
    PJ_ASSERT_RETURN(rem_addr && (addr_len==sizeof(pj_sockaddr_in) ||
	                          addr_len==sizeof(pj_sockaddr_in6)),
	             PJ_EINVAL);

    /* Init op key. */
    tdata->op_key.tdata = tdata;
    tdata->op_key.token = token;
    tdata->op_key.callback = callback;

    /* If asynchronous connect() has not completed yet, just put the
     * transmit data in the pending transmission list since we can not
     * use the socket yet.
     */
    if (fake_tcp->has_pending_connect) {

	/*
	 * Looks like connect() is still in progress. Check again (this time
	 * with holding the lock) to be sure.
	 */
	pj_lock_acquire(fake_tcp->base.lock);

	if (fake_tcp->has_pending_connect) {
	    struct delayed_tdata *delayed_tdata;

	    /*
	     * connect() is still in progress. Put the transmit data to
	     * the delayed list.
             * Starting from #1583 (https://trac.pjsip.org/repos/ticket/1583),
             * we also add timeout value for the transmit data. When the
             * connect() is completed, the timeout value will be checked to
             * determine whether the transmit data needs to be sent.
	     */
	    delayed_tdata = PJ_POOL_ZALLOC_T(tdata->pool,
					     struct delayed_tdata);
	    delayed_tdata->tdata_op_key = &tdata->op_key;
            if (tdata->msg && tdata->msg->type == PJSIP_REQUEST_MSG) {
                pj_gettickcount(&delayed_tdata->timeout);
                delayed_tdata->timeout.msec += pjsip_cfg()->tsx.td;
                pj_time_val_normalize(&delayed_tdata->timeout);
            }

	    pj_list_push_back(&fake_tcp->delayed_list, delayed_tdata);
	    status = PJ_EPENDING;

	    /* Prevent pj_ioqueue_send() to be called below */
	    delayed = PJ_TRUE;
	}

	pj_lock_release(fake_tcp->base.lock);
    }

    if (!delayed) {
	/*
	 * Transport is ready to go. Send the packet to ioqueue to be
	 * sent asynchronously.
	 */
	size = tdata->buf.cur - tdata->buf.start;
//	status = pj_activesock_send(fake_tcp->asock,
//				    (pj_ioqueue_op_key_t*)&tdata->op_key,
//				    tdata->buf.start, &size, 0);
        status = PJ_SUCCESS;  // drop on floor!

	if (status != PJ_EPENDING) {
	    /* Not pending (could be immediate success or error) */
	    tdata->op_key.tdata = NULL;

	    /* Shutdown transport on closure/errors */
	    if (size <= 0) {

		PJ_LOG(5,(fake_tcp->base.obj_name, "FAKE_TCP send() error, sent=%d",
			  size));

		if (status == PJ_SUCCESS)
		    status = PJ_RETURN_OS_ERROR(OSERR_ENOTCONN);

		fake_tcp_init_shutdown(fake_tcp, status);
	    }
	}
    }

    return status;
}


/*
 * This callback is called by transport manager to shutdown transport.
 */
static pj_status_t fake_tcp_shutdown(pjsip_transport *transport)
{
    struct fake_tcp_transport *fake_tcp = (struct fake_tcp_transport*)transport;

    /* Stop keep-alive timer. */
    if (fake_tcp->ka_timer.id) {
	pjsip_endpt_cancel_timer(fake_tcp->base.endpt, &fake_tcp->ka_timer);
	fake_tcp->ka_timer.id = PJ_FALSE;
    }

    return PJ_SUCCESS;
}


/*
 * Callback from ioqueue that an incoming data is received from the socket.
 */
static pj_bool_t on_data_read(pj_activesock_t *asock,
			      void *data,
			      pj_size_t size,
			      pj_status_t status,
			      pj_size_t *remainder)
{
    enum { MAX_IMMEDIATE_PACKET = 10 };
    struct fake_tcp_transport *fake_tcp;
    pjsip_rx_data *rdata;

    PJ_UNUSED_ARG(data);

    fake_tcp = (struct fake_tcp_transport*) pj_activesock_get_user_data(asock);
    rdata = &fake_tcp->rdata;

    /* Don't do anything if transport is closing. */
    if (fake_tcp->is_closing) {
	fake_tcp->is_closing++;
	return PJ_FALSE;
    }

    /* Houston, we have packet! Report the packet to transport manager
     * to be parsed.
     */
    if (status == PJ_SUCCESS) {
	pj_size_t size_eaten;

	/* Mark this as an activity */
	pj_gettimeofday(&fake_tcp->last_activity);

	pj_assert((void*)rdata->pkt_info.packet == data);

	/* Init pkt_info part. */
	rdata->pkt_info.len = size;
	rdata->pkt_info.zero = 0;
	pj_gettimeofday(&rdata->pkt_info.timestamp);

	/* Report to transport manager.
	 * The transport manager will tell us how many bytes of the packet
	 * have been processed (as valid SIP message).
	 */
	size_eaten =
	    pjsip_tpmgr_receive_packet(rdata->tp_info.transport->tpmgr,
				       rdata);

	pj_assert(size_eaten <= (pj_size_t)rdata->pkt_info.len);

	/* Move unprocessed data to the front of the buffer */
	*remainder = size - size_eaten;
	if (*remainder > 0 && *remainder != size) {
	    pj_memmove(rdata->pkt_info.packet,
		       rdata->pkt_info.packet + size_eaten,
		       *remainder);
	}

    } else {

	/* Transport is closed */
	PJ_LOG(4,(fake_tcp->base.obj_name, "FAKE_TCP connection closed"));

	fake_tcp_init_shutdown(fake_tcp, status);

	return PJ_FALSE;

    }

    /* Reset pool. */
    pj_pool_reset(rdata->tp_info.pool);

    return PJ_TRUE;
}


/*
 * Callback from ioqueue when asynchronous connect() operation completes.
 */
static pj_bool_t on_connect_complete(pj_activesock_t *asock,
				     pj_status_t status)
{
    struct fake_tcp_transport *fake_tcp;
    pj_sockaddr addr;
    int addrlen;
    pjsip_tp_state_callback state_cb;

    fake_tcp = (struct fake_tcp_transport*) pj_activesock_get_user_data(asock);

    /* Mark that pending connect() operation has completed. */
    fake_tcp->has_pending_connect = PJ_FALSE;

    /* Check connect() status */
    if (status != PJ_SUCCESS) {

	fake_tcp_perror(fake_tcp->base.obj_name, "FAKE_TCP connect() error", status);

	/* Cancel all delayed transmits */
	while (!pj_list_empty(&fake_tcp->delayed_list)) {
	    struct delayed_tdata *pending_tx;
	    pj_ioqueue_op_key_t *op_key;

	    pending_tx = fake_tcp->delayed_list.next;
	    pj_list_erase(pending_tx);

	    op_key = (pj_ioqueue_op_key_t*)pending_tx->tdata_op_key;

	    on_data_sent(fake_tcp->asock, op_key, -status);
	}

	fake_tcp_init_shutdown(fake_tcp, status);
	return PJ_FALSE;
    }

    PJ_LOG(4,(fake_tcp->base.obj_name,
	      "FAKE_TCP transport %.*s:%d is connected to %.*s:%d",
	      (int)fake_tcp->base.local_name.host.slen,
	      fake_tcp->base.local_name.host.ptr,
	      fake_tcp->base.local_name.port,
	      (int)fake_tcp->base.remote_name.host.slen,
	      fake_tcp->base.remote_name.host.ptr,
	      fake_tcp->base.remote_name.port));


    /* Update (again) local address, just in case local address currently
     * set is different now that the socket is connected (could happen
     * on some systems, like old Win32 probably?).
     */
    addrlen = sizeof(addr);
    if (pj_sock_getsockname(fake_tcp->sock, &addr, &addrlen)==PJ_SUCCESS) {
	pj_sockaddr *tp_addr = &fake_tcp->base.local_addr;

	if (pj_sockaddr_has_addr(&addr) &&
	    pj_sockaddr_cmp(&addr, tp_addr) != 0)
	{
	    pj_sockaddr_cp(tp_addr, &addr);
	    sockaddr_to_host_port(fake_tcp->base.pool, &fake_tcp->base.local_name,
				  tp_addr);
	}
    }

    /* Start pending read */
    status = fake_tcp_start_read(fake_tcp);
    if (status != PJ_SUCCESS) {
	fake_tcp_init_shutdown(fake_tcp, status);
	return PJ_FALSE;
    }

    /* Notify application of transport state connected */
    state_cb = pjsip_tpmgr_get_state_cb(fake_tcp->base.tpmgr);
    if (state_cb) {
	pjsip_transport_state_info state_info;

	pj_bzero(&state_info, sizeof(state_info));
	(*state_cb)(&fake_tcp->base, PJSIP_TP_STATE_CONNECTED, &state_info);
    }

    /* Flush all pending send operations */
    fake_tcp_flush_pending_tx(fake_tcp);

    /* Start keep-alive timer */
    if (PJSIP_FAKE_TCP_KEEP_ALIVE_INTERVAL) {
	pj_time_val delay = { PJSIP_FAKE_TCP_KEEP_ALIVE_INTERVAL, 0 };
	pjsip_endpt_schedule_timer(fake_tcp->base.endpt, &fake_tcp->ka_timer,
				   &delay);
	fake_tcp->ka_timer.id = PJ_TRUE;
	pj_gettimeofday(&fake_tcp->last_activity);
    }

    return PJ_TRUE;
}

/* Transport keep-alive timer callback */
static void fake_tcp_keep_alive_timer(pj_timer_heap_t *th, pj_timer_entry *e)
{
    struct fake_tcp_transport *fake_tcp = (struct fake_tcp_transport*) e->user_data;
    pj_time_val delay;
    pj_time_val now;
    pj_ssize_t size;
    pj_status_t status;

    PJ_UNUSED_ARG(th);

    fake_tcp->ka_timer.id = PJ_TRUE;

    pj_gettimeofday(&now);
    PJ_TIME_VAL_SUB(now, fake_tcp->last_activity);

    if (now.sec > 0 && now.sec < PJSIP_FAKE_TCP_KEEP_ALIVE_INTERVAL) {
	/* There has been activity, so don't send keep-alive */
	delay.sec = PJSIP_FAKE_TCP_KEEP_ALIVE_INTERVAL - now.sec;
	delay.msec = 0;

	pjsip_endpt_schedule_timer(fake_tcp->base.endpt, &fake_tcp->ka_timer,
				   &delay);
	fake_tcp->ka_timer.id = PJ_TRUE;
	return;
    }

    PJ_LOG(5,(fake_tcp->base.obj_name, "Sending %d byte(s) keep-alive to %.*s:%d",
	      (int)fake_tcp->ka_pkt.slen, (int)fake_tcp->base.remote_name.host.slen,
	      fake_tcp->base.remote_name.host.ptr,
	      fake_tcp->base.remote_name.port));

    /* Send the data */
    size = fake_tcp->ka_pkt.slen;
//    status = pj_activesock_send(fake_tcp->asock, &fake_tcp->ka_op_key.key,
//				fake_tcp->ka_pkt.ptr, &size, 0);
    status = PJ_SUCCESS;  // drop on floor!

    if (status != PJ_SUCCESS && status != PJ_EPENDING) {
	fake_tcp_perror(fake_tcp->base.obj_name,
		   "Error sending keep-alive packet", status);
	fake_tcp_init_shutdown(fake_tcp, status);
	return;
    }

    /* Register next keep-alive */
    delay.sec = PJSIP_FAKE_TCP_KEEP_ALIVE_INTERVAL;
    delay.msec = 0;

    pjsip_endpt_schedule_timer(fake_tcp->base.endpt, &fake_tcp->ka_timer,
			       &delay);
    fake_tcp->ka_timer.id = PJ_TRUE;
}

/* Transport connect timer callback */
static void fake_tcp_connect_timer(pj_timer_heap_t *th, pj_timer_entry *e)
{
    struct fake_tcp_transport *fake_tcp = (struct fake_tcp_transport*) e->user_data;

    PJ_UNUSED_ARG(th);

    PJ_LOG(5,(fake_tcp->base.obj_name, "FAKE_TCP connected"));

    on_connect_complete(fake_tcp->asock, PJ_SUCCESS);

    fake_tcp->connect_timer.id = PJ_FALSE;
}