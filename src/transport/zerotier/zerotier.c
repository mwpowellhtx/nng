//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifdef NNG_HAVE_ZEROTIER
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"

const char *nng_opt_zt_home = "zt:home";
const char *nng_opt_zt_nwid = "zt:nwid";
const char *nng_opt_zt_node = "zt:node";

int nng_optid_zt_home = -1;
int nng_optid_zt_nwid = -1;
int nng_optid_zt_node = -1;

#ifndef _WIN32
#include <unistd.h>
#endif

#include <ZeroTierOne.h>

// ZeroTier Transport.  This sits on the ZeroTier L2 network, which itself
// is implemented on top of UDP.  This requires the 3rd party
// libzerotiercore library (which is GPLv3!) and platform specific UDP
// functionality to be built in.  Note that care must be taken to link
// dynamically if one wishes to avoid making your entire application GPL3.
// (Alternatively ZeroTier offers commercial licenses which may prevent
// this particular problem.)  This implementation does not make use of
// certain advanced capabilities in ZeroTier such as more sophisticated
// route management and TCP fallback.  You need to have connectivity
// to the Internet to use this.  (Or at least to your Planetary root.)
//
// Because ZeroTier takes a while to establish connectivity, it is even
// more important that applicaitons using the ZeroTier transport not
// assume that a connection will be immediately available.  It can take
// quite a few seconds for peer-to-peer connectivity to be established.
//
// The ZeroTier transport was funded by Capitar IT Group, BV.
//
// This transport is highly experimental.

// ZeroTier and UDP are connectionless, but nng is designed around
// connection oriented paradigms.  Therefore we will emulate a connection
// as follows:
//
// xxx...
//
// Every participant has an "address", which is a 64-bit value constructed
// using the ZT node number in the upper 40-bits, and a 24-bit port number
// in the lower bits.  We elect to operate primarily on these addresses,
// but the wire protocol relies on just conveying the 24-bit port along
// with the MAC address (from which the ZT node number can be derived,
// given the network ID.)

typedef struct zt_pipe     zt_pipe;
typedef struct zt_ep       zt_ep;
typedef struct zt_node     zt_node;
typedef struct zt_frag     zt_frag;
typedef struct zt_fraglist zt_fraglist;

// Port numbers are stored as 24-bit values in network byte order.
#define ZT_GET24(ptr, v)                              \
	v = (((uint32_t)((uint8_t)(ptr)[0])) << 16) + \
	    (((uint32_t)((uint8_t)(ptr)[1])) << 8) +  \
	    (((uint32_t)(uint8_t)(ptr)[2]))

#define ZT_PUT24(ptr, u)                                     \
	do {                                                 \
		(ptr)[0] = (uint8_t)(((uint32_t)(u)) >> 16); \
		(ptr)[1] = (uint8_t)(((uint32_t)(u)) >> 8);  \
		(ptr)[2] = (uint8_t)((uint32_t)(u));         \
	} while (0)

static const uint16_t zt_ethertype = 0x901;
static const uint8_t  zt_version   = 0x01;
static const uint32_t zt_ephemeral = 0x800000u; // start of ephemeral ports
static const uint32_t zt_max_port  = 0xffffffu; // largest port
static const uint32_t zt_port_mask = 0xffffffu; // mask of valid ports

// These are compile time tunables for now.
enum zt_tunables {
	zt_listenq       = 128,              // backlog queue length
	zt_listen_expire = 60000000,         // maximum time in backlog
	zt_rcv_bufsize   = ZT_MAX_MTU + 128, // max UDP recv
	zt_conn_attempts = 12,               // connection attempts (default)
	zt_conn_interval = 5000000,          // between attempts (usec)
	zt_udp_sendq     = 16,               // outgoing UDP queue length
	zt_recvq         = 2,                // max pending recv (per pipe)
	zt_recv_stale    = 1000000,          // frags older than are stale
};

enum zt_op_codes {
	zt_op_data     = 0x00, // data, final fragment
	zt_op_data_mf  = 0x01, // data, more fragments
	zt_op_conn_req = 0x10, // connect request
	zt_op_conn_ack = 0x12, // connect accepted
	zt_op_disc_req = 0x20, // disconnect request (no ack)
	zt_op_ping_req = 0x30, // ping request
	zt_op_ping_ack = 0x32, // ping response
	zt_op_error    = 0x40, // error response
};

enum zt_offsets {
	zt_offset_op          = 0x00,
	zt_offset_flags       = 0x01,
	zt_offset_version     = 0x02, // protocol version number (2 bytes)
	zt_offset_zero1       = 0x04, // reserved, must be zero (1 byte)
	zt_offset_dst_port    = 0x05, // destination port (3 bytes)
	zt_offset_zero2       = 0x08, // reserved, must be zero (1 byte)
	zt_offset_src_port    = 0x09, // source port number (3 bytes)
	zt_offset_creq_proto  = 0x0C, // SP protocol number (2 bytes)
	zt_offset_cack_proto  = 0x0C, // SP protocol number (2 bytes)
	zt_offset_err_code    = 0x0C, // error code (1 byte)
	zt_offset_err_msg     = 0x0D, // error message (string)
	zt_offset_data_id     = 0x0C, // message ID (2 bytes)
	zt_offset_data_fragsz = 0x0E, // fragment size
	zt_offset_data_frag   = 0x10, // fragment number, first is 1 (2 bytes)
	zt_offset_data_nfrag  = 0x12, // total fragments (2 bytes)
	zt_offset_data_data   = 0x14, // user payload
	zt_size_headers       = 0x0C, // size of headers
	zt_size_conn_req      = 0x0E, // size of conn_req (connect request)
	zt_size_conn_ack      = 0x0E, // size of conn_ack (connect reply)
	zt_size_disc_req      = 0x0C, // size of disc_req (disconnect)
	zt_size_ping_req      = 0x0C, // size of ping request
	zt_size_ping_ack      = 0x0C, // size of ping reply
	zt_size_data          = 0x14, // size of data message (w/o payload)
};

enum zt_errors {
	zt_err_refused = 0x01, // Connection refused
	zt_err_notconn = 0x02, // Connection does not exit
	zt_err_wrongsp = 0x03, // SP protocol mismatch
	zt_err_proto   = 0x04, // Other protocol errror
	zt_err_msgsize = 0x05, // Message to large
	zt_err_unknown = 0x06, // Other errors
};

// This node structure is wrapped around the ZT_node; this allows us to
// have multiple endpoints referencing the same ZT_node, but also to
// support different nodes (identities) based on different homedirs.
// This means we need to stick these on a global linked list, manage
// them with a reference count, and uniquely identify them using the
// homedir.
struct zt_node {
	char          zn_path[NNG_MAXADDRLEN]; // ought to be sufficient
	ZT_Node *     zn_znode;
	uint64_t      zn_self;
	nni_list_node zn_link;
	int           zn_closed;
	nni_plat_udp *zn_udp4;
	nni_plat_udp *zn_udp6;
	nni_list      zn_eplist;
	nni_list      zn_plist;
	nni_idhash *  zn_ports;
	nni_idhash *  zn_eps;
	nni_idhash *  zn_pipes;
	nni_idhash *  zn_peers; // indexed by remote address
	nni_aio *     zn_rcv4_aio;
	char *        zn_rcv4_buf;
	nng_sockaddr  zn_rcv4_addr;
	nni_aio *     zn_rcv6_aio;
	char *        zn_rcv6_buf;
	nng_sockaddr  zn_rcv6_addr;
	nni_thr       zn_bgthr;
	nni_time      zn_bgtime;
	nni_cv        zn_bgcv;
	nni_cv        zn_snd6_cv;
};

struct zt_fraglist {
	nni_time     fl_time;  // time first frag was received
	uint32_t     fl_msgid; // message id
	int          fl_ready; // we have all messages
	unsigned int fl_fragsz;
	unsigned int fl_nfrags;
	uint8_t *    fl_missing;
	size_t       fl_missingsz;
	nni_msg *    fl_msg;
};

struct zt_pipe {
	nni_list_node zp_link;
	const char *  zp_addr;
	zt_node *     zp_ztn;
	uint64_t      zp_nwid;
	uint64_t      zp_laddr;
	uint64_t      zp_raddr;
	uint16_t      zp_peer;
	uint16_t      zp_proto;
	uint16_t      zp_next_msgid;
	size_t        zp_rcvmax;
	size_t        zp_mtu;
	int           zp_closed;
	nni_aio *     zp_user_rxaio;
	zt_fraglist   zp_recvq[zt_recvq];

	nni_aio *zp_rxaio;
	nni_aio *zp_pngaio;
	nni_msg *zp_rxmsg;
};

typedef struct zt_creq zt_creq;
struct zt_creq {
	uint64_t cr_expire;
	uint64_t cr_raddr;
	uint16_t cr_proto;
};

struct zt_ep {
	nni_list_node ze_link;
	char          ze_url[NNG_MAXADDRLEN];
	char          ze_home[NNG_MAXADDRLEN]; // should be enough
	zt_node *     ze_ztn;
	uint64_t      ze_nwid;
	int           ze_mode;
	nni_sockaddr  ze_addr;
	uint64_t      ze_raddr; // remote node address
	uint64_t      ze_laddr; // local node address
	uint16_t      ze_proto;
	size_t        ze_rcvmax;
	nni_aio *     ze_aio;
	nni_aio *     ze_creq_aio;
	int           ze_creq_try;
	nni_list      ze_aios;
	int           ze_maxmtu;
	int           ze_phymtu;

	// Incoming connection requests (server only).  We only
	// only have "accepted" requests -- that is we won't have an
	// established connection/pipe unless the application calls
	// accept.  Since the "application" is our library, that should
	// be pretty much as fast we can run.
	zt_creq ze_creqs[zt_listenq];
	int     ze_creq_head;
	int     ze_creq_tail;
};

// Locking strategy.  At present the ZeroTier core is not reentrant or fully
// threadsafe.  (We expect this will be fixed.)  Furthermore, there are
// some significant challenges in dealing with locks associated with the
// callbacks, etc.  So we take a big-hammer approach, and just use a single
// global lock for everything.  We hold this lock when calling into the
// ZeroTier framework.  Since ZeroTier has no independent threads, that
// means that it will always hold this lock in its core, and the lock will
// also be held automatically in any of our callbacks.  We never hold any
// other locks across ZeroTier core calls. We may not acquire the global
// lock in callbacks (they will already have it held). Any other locks
// can be acquired as long as they are not held during calls into ZeroTier.
//
// This will have a detrimental impact on performance, but to be completely
// honest we don't think anyone will be using the ZeroTier transport in
// excessively performance critical applications; scalability may become
// a factor for large servers sitting in a ZeroTier hub situation.  (Then
// again, since only the zerotier procesing is single threaded, it may not
// be that much of a bottleneck -- really depends on how expensive these
// operations are.  We can use lockstat or other lock-hotness tools to
// check for this later.)

static nni_mtx  zt_lk;
static nni_list zt_nodes;

static void zt_ep_send_conn_req(zt_ep *);
static void zt_ep_conn_req_cb(void *);
static void zt_ep_doaccept(zt_ep *);
static void zt_pipe_dorecv(zt_pipe *);
static int  zt_pipe_init(zt_pipe **, zt_ep *, uint64_t, uint64_t);
static void zt_fraglist_clear(zt_fraglist *);
static void zt_fraglist_free(zt_fraglist *);

static uint64_t
zt_now(void)
{
	// We return msec
	return (nni_clock() / 1000);
}

static void
zt_bgthr(void *arg)
{
	zt_node *ztn = arg;
	nni_time now;

	nni_mtx_lock(&zt_lk);
	for (;;) {
		now = nni_clock(); // msecmsec

		if (ztn->zn_closed) {
			break;
		}

		if (now < ztn->zn_bgtime) {
			nni_cv_until(&ztn->zn_bgcv, ztn->zn_bgtime);
			continue;
		}

		now /= 1000; // usec -> msec
		ZT_Node_processBackgroundTasks(ztn->zn_znode, NULL, now, &now);

		ztn->zn_bgtime = now * 1000; // usec
	}
	nni_mtx_unlock(&zt_lk);
}

static void
zt_node_resched(zt_node *ztn, uint64_t msec)
{
	ztn->zn_bgtime = msec * 1000; // convert to usec
	nni_cv_wake1(&ztn->zn_bgcv);
}

static void
zt_node_rcv4_cb(void *arg)
{
	zt_node *               ztn = arg;
	nni_aio *               aio = ztn->zn_rcv4_aio;
	struct sockaddr_storage sa;
	struct sockaddr_in *    sin;
	nng_sockaddr_in *       nsin;
	uint64_t                now;
	char                    abuf[64];

	if (nni_aio_result(aio) != 0) {
		// Outside of memory exhaustion, we can't really think
		// of any reason for this to legitimately fail.
		// Arguably we should inject a fallback delay, but for
		// now we just carry on.
		// XXX: REVIEW THIS.  Its clearly wrong!  If the socket
		// is closed or fails in a permanent way, then we need
		// to stop the UDP work, and forward an error to all
		// the other endpoints and pipes!
		return;
	}

	memset(&sa, 0, sizeof(sa));
	sin                  = (void *) &sa;
	nsin                 = &ztn->zn_rcv4_addr.s_un.s_in;
	sin->sin_family      = AF_INET;
	sin->sin_port        = nsin->sa_port;
	sin->sin_addr.s_addr = nsin->sa_addr;
	// inet_ntop(AF_INET, &sin->sin_addr, abuf, sizeof(abuf));
	//	printf("NODE RECVv4 CB called RCV FROM %s %u!\n", abuf,
	//	    htons(sin->sin_port));

	nni_mtx_lock(&zt_lk);
	now = zt_now();

	// We are not going to perform any validation of the data; we
	// just pass this straight into the ZeroTier core.
	// XXX: CHECK THIS, if it fails then we have a fatal error with
	// the znode, and have to shut everything down.
	ZT_Node_processWirePacket(ztn->zn_znode, NULL, now, 0, (void *) &sa,
	    ztn->zn_rcv4_buf, aio->a_count, &now);

	// Schedule background work
	zt_node_resched(ztn, now);

	// Schedule another receive.
	if (ztn->zn_udp4 != NULL) {
		aio->a_count = 0;
		nni_plat_udp_recv(ztn->zn_udp4, aio);
	}
	nni_mtx_unlock(&zt_lk);
}

static void
zt_node_rcv6_cb(void *arg)
{
	zt_node *                ztn = arg;
	nni_aio *                aio = ztn->zn_rcv6_aio;
	struct sockaddr_storage  sa;
	struct sockaddr_in6 *    sin6;
	struct nng_sockaddr_in6 *nsin6;
	uint64_t                 now;
	char                     abuf[64];

	if (nni_aio_result(aio) != 0) {
		// Outside of memory exhaustion, we can't really think
		// of any reason for this to legitimately fail.
		// Arguably we should inject a fallback delay, but for
		// now we just carry on.
		// XXX: REVIEW THIS.  Its clearly wrong!  If the socket
		// is closed or fails in a permanent way, then we need
		// to stop the UDP work, and forward an error to all
		// the other endpoints and pipes!
		return;
	}

	memset(&sa, 0, sizeof(sa));
	sin6              = (void *) &sa;
	nsin6             = &ztn->zn_rcv6_addr.s_un.s_in6;
	sin6->sin6_family = AF_INET6;
	sin6->sin6_port   = nsin6->sa_port;
	memcpy(&sin6->sin6_addr, nsin6->sa_addr, 16);
	// inet_ntop(AF_INET6, &sin6->sin6_addr, abuf, sizeof(abuf));
	//	printf("NODE RECVv6 CB called RCV FROM %s %u!\n", abuf,
	//	    htons(sin6->sin6_port));

	nni_mtx_lock(&zt_lk);
	now = zt_now(); // msec

	// We are not going to perform any validation of the data; we
	// just pass this straight into the ZeroTier core.
	// XXX: CHECK THIS, if it fails then we have a fatal error with
	// the znode, and have to shut everything down.
	ZT_Node_processWirePacket(ztn->zn_znode, NULL, now, 0, (void *) &sa,
	    ztn->zn_rcv6_buf, aio->a_count, &now);

	// Schedule background work
	zt_node_resched(ztn, now);

	// Schedule another receive.
	if (ztn->zn_udp6 != NULL) {
		aio->a_count = 0;
		nni_plat_udp_recv(ztn->zn_udp6, aio);
	}
	nni_mtx_unlock(&zt_lk);
}

static uint64_t
zt_mac_to_node(uint64_t mac, uint64_t nwid)
{
	uint64_t node;
	// This extracts a node address from a mac addres.  The
	// network ID is mixed in, and has to be extricated.  We
	// the node ID is located in the lower 40 bits, and scrambled
	// against the nwid.
	node = mac & 0xffffffffffull;
	node ^= ((nwid >> 8) & 0xff) << 32;
	node ^= ((nwid >> 16) & 0xff) << 24;
	node ^= ((nwid >> 24) & 0xff) << 16;
	node ^= ((nwid >> 32) & 0xff) << 8;
	node ^= (nwid >> 40) & 0xff;
	return (node);
}

static uint64_t
zt_node_to_mac(uint64_t node, uint64_t nwid)
{
	uint64_t mac;
	// We use LSB of network ID, and make sure that we clear
	// multicast and set local administration -- this is the first
	// octet of the 48 bit mac address.  We also avoid 0x52, which
	// is known to be used in KVM, libvirt, etc.
	mac = ((uint8_t)(nwid & 0xfe) | 0x02);
	if (mac == 0x52) {
		mac = 0x32;
	}
	mac <<= 40;
	mac |= node;
	// The rest of the network ID is XOR'd in, in reverse byte
	// order.
	mac ^= ((nwid >> 8) & 0xff) << 32;
	mac ^= ((nwid >> 16) & 0xff) << 24;
	mac ^= ((nwid >> 24) & 0xff) << 16;
	mac ^= ((nwid >> 32) & 0xff) << 8;
	mac ^= (nwid >> 40) & 0xff;
	return (mac);
}

static int
zt_result(enum ZT_ResultCode rv)
{
	switch (rv) {
	case ZT_RESULT_OK:
		return (0);
	case ZT_RESULT_OK_IGNORED:
		return (0);
	case ZT_RESULT_FATAL_ERROR_OUT_OF_MEMORY:
		return (NNG_ENOMEM);
	case ZT_RESULT_FATAL_ERROR_DATA_STORE_FAILED:
		return (NNG_EPERM);
	case ZT_RESULT_FATAL_ERROR_INTERNAL:
		return (NNG_EINTERNAL);
	case ZT_RESULT_ERROR_NETWORK_NOT_FOUND:
		return (NNG_EADDRINVAL);
	case ZT_RESULT_ERROR_UNSUPPORTED_OPERATION:
		return (NNG_ENOTSUP);
	case ZT_RESULT_ERROR_BAD_PARAMETER:
		return (NNG_EINVAL);
	default:
		return (NNG_ETRANERR + (int) rv);
	}
}

// ZeroTier Node API callbacks
static int
zt_virtual_config(ZT_Node *node, void *userptr, void *thr, uint64_t nwid,
    void **netptr, enum ZT_VirtualNetworkConfigOperation op,
    const ZT_VirtualNetworkConfig *config)
{
	zt_node *ztn = userptr;
	zt_ep *  ep;

	NNI_ARG_UNUSED(thr);
	NNI_ARG_UNUSED(netptr);

	NNI_ASSERT(node == ztn->zn_znode);

	// Maybe we don't have to create taps or anything like that.
	// We do get our mac and MTUs from this, so there's that.
	switch (op) {
	case ZT_VIRTUAL_NETWORK_CONFIG_OPERATION_UP:
	case ZT_VIRTUAL_NETWORK_CONFIG_OPERATION_CONFIG_UPDATE:

		// We only really care about changes to the MTU.  From
		// an API perspective the MAC could change, but that
		// cannot really happen because the node identity and
		// the nwid are fixed.
		NNI_LIST_FOREACH (&ztn->zn_eplist, ep) {
			NNI_ASSERT(nwid == config->nwid);
			if (ep->ze_nwid != config->nwid) {
				continue;
			}
			ep->ze_maxmtu = config->mtu;
			ep->ze_phymtu = config->physicalMtu;

			if ((ep->ze_mode == NNI_EP_MODE_DIAL) &&
			    (nni_list_first(&ep->ze_aios) != NULL)) {
				zt_ep_send_conn_req(ep);
			}
			// if (ep->ze_mode == NNI_EP
			//	zt_send_
			//	nni_aio_finish(ep->ze_join_aio, 0);
			// }
			// XXX: schedule creqs if needed!
		}
		break;
	case ZT_VIRTUAL_NETWORK_CONFIG_OPERATION_DESTROY:
	case ZT_VIRTUAL_NETWORK_CONFIG_OPERATION_DOWN:
	// XXX: tear down endpoints?
	default:
		break;
	}
	return (0);
}

// zt_send modifies the start of the supplied buffer to update the
// message headers with protocol specific details (version, port numbers,
// etc.) and then sends it over the virtual network.
static void
zt_send(zt_node *ztn, uint64_t nwid, uint8_t op, uint64_t raddr,
    uint64_t laddr, uint8_t *data, size_t len)
{
	uint64_t srcmac = zt_node_to_mac(laddr >> 24, nwid);
	uint64_t dstmac = zt_node_to_mac(raddr >> 24, nwid);
	uint64_t now    = zt_now();

	NNI_ASSERT(len >= zt_size_headers);
	data[zt_offset_op]    = op;
	data[zt_offset_flags] = 0;
	data[zt_offset_zero1] = 0;
	data[zt_offset_zero2] = 0;
	NNI_PUT16(data + zt_offset_version, zt_version);
	ZT_PUT24(data + zt_offset_dst_port, raddr & zt_port_mask);
	ZT_PUT24(data + zt_offset_src_port, laddr & zt_port_mask);

	(void) ZT_Node_processVirtualNetworkFrame(ztn->zn_znode, NULL, now,
	    nwid, srcmac, dstmac, zt_ethertype, 0, data, len, &now);

	zt_node_resched(ztn, now);
}

static void
zt_send_err(zt_node *ztn, uint64_t nwid, uint64_t raddr, uint64_t laddr,
    uint8_t err, const char *msg)
{
	uint8_t data[128];

	NNI_ASSERT((strlen(msg) + zt_offset_err_msg) < sizeof(data));

	data[zt_offset_err_code] = err;
	nni_strlcpy((char *) data + zt_offset_err_msg, msg,
	    sizeof(data) - zt_offset_err_msg);

	zt_send(ztn, nwid, zt_op_error, raddr, laddr, data,
	    strlen(msg) + zt_offset_err_msg);
}

static void
zt_pipe_send_disc_req(zt_pipe *p)
{
	uint8_t data[zt_size_disc_req];

	zt_send(p->zp_ztn, p->zp_nwid, zt_op_disc_req, p->zp_raddr,
	    p->zp_laddr, data, sizeof(data));
}

static void
zt_pipe_send_conn_ack(zt_pipe *p)
{
	uint8_t data[zt_size_conn_ack];

	NNI_PUT16(data + zt_offset_cack_proto, p->zp_proto);
	zt_send(p->zp_ztn, p->zp_nwid, zt_op_conn_ack, p->zp_raddr,
	    p->zp_laddr, data, sizeof(data));
}

static void
zt_ep_send_conn_req(zt_ep *ep)
{
	uint8_t data[zt_size_conn_req];

	NNI_PUT16(data + zt_offset_creq_proto, ep->ze_proto);
	zt_send(ep->ze_ztn, ep->ze_nwid, zt_op_conn_req, ep->ze_raddr,
	    ep->ze_laddr, data, sizeof(data));
}

static void
zt_ep_recv_conn_ack(zt_ep *ep, uint64_t raddr, const uint8_t *data, size_t len)
{
	zt_node *ztn = ep->ze_ztn;
	nni_aio *aio = ep->ze_creq_aio;
	zt_pipe *p;
	int      rv;

	if (ep->ze_mode != NNI_EP_MODE_DIAL) {
		zt_send_err(ztn, ep->ze_nwid, raddr, ep->ze_laddr,
		    zt_err_proto, "Inappropriate operation");
		return;
	}

	if (len != zt_size_conn_ack) {
		zt_send_err(ztn, ep->ze_nwid, raddr, ep->ze_laddr,
		    zt_err_proto, "Bad message length");
		return;
	}

	if (ep->ze_creq_try == 0) {
		return;
	}

	// Do we already have a matching pipe?  If so, we can discard
	// the operation.  This should not happen, since we normally,
	// deregister the endpoint when we create the pipe.
	if ((nni_idhash_find(ztn->zn_peers, raddr, (void **) &p)) == 0) {
		return;
	}

	if ((rv = zt_pipe_init(&p, ep, raddr, ep->ze_laddr)) != 0) {
		// We couldn't create the pipe, just drop it.
		nni_aio_finish_error(aio, rv);
		return;
	}
	NNI_GET16(data + zt_offset_cack_proto, p->zp_peer);

	// Reset the address of the endpoint, so that the next call to
	// ep_connect will bind a new one -- we are using this one for the
	// pipe.
	nni_idhash_remove(ztn->zn_eps, ep->ze_laddr);
	ep->ze_laddr = 0;

	printf("GIVING DIALER GOOD PIPE!\n");
	nni_aio_finish_pipe(ep->ze_creq_aio, p);
}

static void
zt_ep_recv_conn_req(zt_ep *ep, uint64_t raddr, const uint8_t *data, size_t len)
{
	zt_node *ztn = ep->ze_ztn;
	zt_pipe *p;
	int      i;

	if (ep->ze_mode != NNI_EP_MODE_LISTEN) {
		zt_send_err(ztn, ep->ze_nwid, raddr, ep->ze_laddr,
		    zt_err_proto, "Inappropriate operation");
		return;
	}
	if (len != zt_size_conn_req) {
		zt_send_err(ztn, ep->ze_nwid, raddr, ep->ze_laddr,
		    zt_err_proto, "Bad message length");
		return;
	}

	// If we already have created a pipe for this connection
	// then just reply the conn ack.
	if ((nni_idhash_find(ztn->zn_peers, raddr, (void **) &p)) == 0) {
		printf("WE ALREADY HAVE A PIPE!\n");
		zt_pipe_send_conn_ack(p);
		return;
	}

	// We may already have a connection request queued (if this was
	// a resend for example); if that's the case we just ignore
	// this one.
	for (i = ep->ze_creq_tail; i != ep->ze_creq_head; i++) {
		if (ep->ze_creqs[i % zt_listenq].cr_raddr == raddr) {
			printf("THAT ONE IS PENDING!\n");
			return;
		}
	}
	// We may already have filled our listenq, in which case we just drop.
	if ((ep->ze_creq_tail + zt_listenq) == ep->ze_creq_head) {
		// We have taken as many as we can, so just drop it.
		printf("LISTENQ FULL\n");
		return;
	}

	// Record the connection request, and then process any
	// pending acceptors.
	i = ep->ze_creq_head % zt_listenq;

	NNI_GET16(data + zt_offset_creq_proto, ep->ze_creqs[i].cr_proto);
	ep->ze_creqs[i].cr_raddr  = raddr;
	ep->ze_creqs[i].cr_expire = nni_clock() + zt_listen_expire;
	ep->ze_creq_head++;

	zt_ep_doaccept(ep);
}

static void
zt_ep_recv_error(zt_ep *ep, uint64_t raddr, const uint8_t *data, size_t len)
{
	nni_aio *aio;
	int      code;

	// Most of the time we don't care about errors.  The exception here
	// is that when we have an outstanding CON_REQ, we would like to
	// process that appropriately.

	if (ep->ze_mode != NNI_EP_MODE_DIAL) {
		// Drop it.
		return;
	}

	if (len < zt_offset_err_msg) {
		// Malformed error frame.
		return;
	}

	code = data[zt_offset_err_code];
	switch (code) {
	case zt_err_refused:
		code = NNG_ECONNREFUSED;
		break;
	case zt_err_notconn:
		code = NNG_ECLOSED;
		break;
	case zt_err_wrongsp:
		code = NNG_EPROTO;
		break;
	default:
		code = NNG_ETRANERR;
		break;
	}

	if (ep->ze_creq_try > 0) {
		ep->ze_creq_try = 0;
		nni_aio_finish_error(ep->ze_creq_aio, code);
	}
}

static void
zt_ep_virtual_recv(
    zt_ep *ep, uint8_t op, uint64_t raddr, const uint8_t *data, size_t len)
{
	zt_node *ztn   = ep->ze_ztn;
	uint64_t laddr = ep->ze_laddr;
	uint64_t nwid  = ep->ze_nwid;
	zt_creq *creq;
	zt_pipe *p;

	// Only listeners should be receiving.  Dialers receive on the pipe,
	// rather than the endpoint.  The only message that endpoints can
	// receive are connection requests.
	switch (op) {
	case zt_op_conn_req:
		zt_ep_recv_conn_req(ep, raddr, data, len);
		return;
	case zt_op_conn_ack:
		zt_ep_recv_conn_ack(ep, raddr, data, len);
		return;
	case zt_op_error:
		zt_ep_recv_error(ep, raddr, data, len);
		return;
	default:
		zt_send_err(ep->ze_ztn, ep->ze_nwid, raddr, ep->ze_laddr,
		    zt_err_proto, "Bad operation");
		return;
	}
}

static void
zt_pipe_recv_data(zt_pipe *p, const uint8_t *data, size_t len, int last)
{
	nni_aio *    aio;
	uint16_t     msgid;
	uint16_t     fragno;
	uint16_t     nfrags;
	uint16_t     fragsz;
	zt_fraglist *fl;
	int          i;
	int          slot;
	uint8_t      bit;
	uint8_t *    body;
	int          rv;

	if (len < zt_size_data) {
		// Runt frame.  Drop it and close pipe with a protocol error.
		if ((aio = p->zp_user_rxaio) != NULL) {
			p->zp_user_rxaio = NULL;
			p->zp_closed     = 1;
			// XXX: send an error to other side..
			// XXX: zt_pipe_send_error(p, protoocol....).
			nni_aio_finish_error(aio, NNG_EPROTO);
			return;
		}
	}

	NNI_GET16(data + zt_offset_data_id, msgid);
	NNI_GET16(data + zt_offset_data_fragsz, fragsz);
	NNI_GET16(data + zt_offset_data_frag, fragno);
	NNI_GET16(data + zt_offset_data_nfrag, nfrags);
	len -= zt_offset_data_data;
	data += zt_offset_data_data;

	// Check for cases where message size is clearly too large.  Note
	// that we only can catch the case where a message is larger by
	// more than a fragment, since the final fragment may be shorter,
	// and we won't know that until we receive it.
	if ((nfrags * fragsz) >= (p->zp_rcvmax + fragsz)) {
		// XXX: zt_pipe_send_error(p, emsgtoobig);
		return;
	}

	// We run the recv logic once, to clear stale fragment entries.
	zt_pipe_dorecv(p);

	// Find a suitable fragment slot.
	slot = -1;
	for (i = 0; i < zt_recvq; i++) {
		fl = &p->zp_recvq[i];
		// This was our message ID, we always use it.
		if (msgid == fl->fl_msgid) {
			slot = i;
			break;
		}

		if (slot < 0) {
			slot = i;
		} else if (fl->fl_time < p->zp_recvq[slot].fl_time) {
			// This has an earlier expiration, so lets choose it.
			slot = i;
		}
	}

	NNI_ASSERT(slot >= 0);

	fl = &p->zp_recvq[slot];
	if (fl->fl_msgid != msgid) {
		// First fragment we've received for this message (but might
		// not be first fragment for message!)
		zt_fraglist_clear(fl);

		rv = nni_msg_alloc(&fl->fl_msg, nfrags * fragsz);
		if (rv != 0) {
			// XXX: out of memory, close the pipe?
			return;
		}

		fl->fl_nfrags = nfrags;
		fl->fl_fragsz = fragsz;
		fl->fl_msgid  = msgid;

		// Set the missing mask.
		memset(fl->fl_missing, 0xff, nfrags / 8);
		fl->fl_missing[i] |= ((1 << (nfrags % 8)) - 1);
	}
	if ((nfrags != fl->fl_nfrags) || (fragsz != fl->fl_fragsz) ||
	    (fragno >= nfrags) || (fragsz == 0) || (nfrags == 0) ||
	    ((fragno != (nfrags - 1)) && (len != fragsz))) {
		// Protocol error, message parameters changed.
		// zt_pipe_send_error(p, bad message);
		zt_fraglist_clear(fl);
		return;
	}

	bit = (uint8_t)(1 << (fragno % 8));
	if (fl->fl_missing[fragno / 8] & bit) {
		// We've already got this fragment, ignore it.  We don't
		// bother to check for changed data.
		return;
	}

	fl->fl_missing[fragno / 8] &= ~(bit);
	body = nni_msg_body(fl->fl_msg);
	body += fragno * fragsz;
	memcpy(body, data, len);
	if (fragno == (nfrags - 1)) {
		// Last frag, maybe shorten the message.
		nni_msg_chop(fl->fl_msg, (fragsz - len));
		if (nni_msg_len(fl->fl_msg) > p->zp_rcvmax) {
			// Strict enforcement of max recv.
			zt_fraglist_clear(fl);
			// XXX: zt_pipe_send_error(p, emsgsize);
			return;
		}
	}

	for (i = 0; i < fl->fl_missingsz; i++) {
		if (fl->fl_missing[i]) {
			return;
		}
	}

	// We got all fragments... try to send it up.
	fl->fl_ready = 1;
	zt_pipe_dorecv(p);
}

static void
zt_pipe_recv_disc_req(zt_pipe *p, const uint8_t *data, size_t len)
{
	nni_aio *aio;
	printf("REMOTE DISCONNECT!\n");
	// NB: lock held already.
	// We don't bother to check the length... we're going to
	// disconnect anyway.
	if ((aio = p->zp_user_rxaio) != NULL) {
		p->zp_user_rxaio = NULL;
		p->zp_closed     = 1;
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
}

// This function is called when we have determined that a frame has
// arrived for a pipe.  The remote and local addresses were both
// matched by the caller.
static void
zt_pipe_virtual_recv(zt_pipe *p, uint8_t op, const uint8_t *data, size_t len)
{
	printf("PIPE VIRTUAL RECV!\n");
	switch (op) {
	case zt_op_disc_req:
		zt_pipe_recv_disc_req(p, data, len);
		return;
	}
}

// This function is called when a frame arrives on the
// *virtual* network.
static void
zt_virtual_recv(ZT_Node *node, void *userptr, void *thr, uint64_t nwid,
    void **netptr, uint64_t srcmac, uint64_t dstmac, unsigned int ethertype,
    unsigned int vlanid, const void *payload, unsigned int len)
{
	zt_node *      ztn = userptr;
	uint8_t        op;
	const uint8_t *data = payload;
	uint16_t       proto;
	uint16_t       version;
	uint32_t       rport;
	uint32_t       lport;
	zt_ep *        ep;
	zt_pipe *      p;
	uint64_t       raddr;
	uint64_t       laddr;

	printf("VIRTUAL NET FRAME RECVD\n");
	if ((ethertype != zt_ethertype) || (len < zt_size_headers) ||
	    (data[zt_offset_flags] != 0) || (data[zt_offset_zero1] != 0) ||
	    (data[zt_offset_zero2] != 0)) {
		return;
	}
	NNI_GET16(data + zt_offset_version, version);
	if (version != zt_version) {
		return;
	}

	op = data[zt_offset_op];

	ZT_GET24(data + zt_offset_dst_port, lport);
	ZT_GET24(data + zt_offset_src_port, rport);

	raddr = zt_mac_to_node(srcmac, nwid);
	raddr <<= 24;
	raddr |= rport;

	laddr = zt_mac_to_node(dstmac, nwid);
	laddr <<= 24;
	laddr |= lport;

	// NB: We are holding the zt_lock.

	// Look up a pipe, but also we use this chance to check
	// that the source address matches what the pipe was
	// established with.  If the pipe does not match then
	// we nak it.
	if ((nni_idhash_find(ztn->zn_pipes, laddr, (void **) &p) == 0) &&
	    (p->zp_nwid == nwid)) {
		if (p->zp_raddr == 0) {
			// Remote address is zero while waiting for the
			// reply from the conn_req.
			p->zp_raddr = raddr;
		} else if (p->zp_raddr != raddr) {
			zt_send_err(ztn, nwid, raddr, laddr, zt_err_notconn,
			    "Not connected");
			return;
		}
		zt_pipe_virtual_recv(p, op, data, len);
		return;
	}

	// No pipe, so look for an endpoint.
	if ((nni_idhash_find(ztn->zn_eps, laddr, (void **) &ep) == 0) &&
	    (ep->ze_nwid == nwid)) {
		// direct this to an endpoint.
		zt_ep_virtual_recv(ep, op, raddr, data, len);
		return;
	}

	// We have a request for which we have no listener, and no
	// pipe. For some of these we send back a NAK, but for others
	// we just drop the frame.
	switch (op) {
	case zt_op_conn_req:
		// No listener.  Connection refused.
		zt_send_err(ztn, nwid, raddr, laddr, zt_err_refused,
		    "Connection refused");
		return;
	case zt_op_data:
	case zt_op_ping_req:
	case zt_op_conn_ack:
		zt_send_err(
		    ztn, nwid, raddr, laddr, zt_err_notconn, "Not connected");
		break;
	case zt_op_error:
	case zt_op_ping_ack:
	case zt_op_disc_req:
	default:
		// Just drop these.
		break;
	}
}

static void
zt_event_cb(ZT_Node *node, void *userptr, void *thr, enum ZT_Event event,
    const void *payload)
{
	switch (event) {
	case ZT_EVENT_ONLINE:
		printf("EVENT ONLINE!\n");
		break;
	case ZT_EVENT_UP:
		printf("EVENT UP!\n");
		break;
	case ZT_EVENT_DOWN:
		printf("EVENT DOWN!\n");
		break;
	case ZT_EVENT_OFFLINE:
		printf("EVENT OFFLINE!\n");
		break;
	case ZT_EVENT_TRACE:
		// printf("TRACE: %s\n", (const char *) payload);
		break;
	case ZT_EVENT_REMOTE_TRACE:
		printf("REMOTE TRACE\n");
		break;
	default:
		printf("OTHER EVENT %d\n", event);
		break;
	}
}

static const char *zt_files[] = {
	// clang-format off
	NULL, // none, i.e. not used at all
	"identity.public",
	"identity.secret",
	"planet",
	NULL, // moon, e.g. moons.d/<ID>.moon -- we don't persist it
	NULL, // peer, e.g. peers.d/<ID> -- we don't persist this
	NULL, // network, e.g. networks.d/<ID>.conf -- we don't persist
	// clang-format on
};

#ifdef _WIN32
#define unlink DeleteFile
#define pathsep "\\"
#else
#define pathsep "/"
#endif

static struct {
	size_t len;
	void * data;
} zt_ephemeral_state[ZT_STATE_OBJECT_NETWORK_CONFIG];

static void
zt_state_put(ZT_Node *node, void *userptr, void *thr,
    enum ZT_StateObjectType objtype, const uint64_t objid[2], const void *data,
    int len)
{
	FILE *      file;
	zt_node *   ztn = userptr;
	char        path[NNG_MAXADDRLEN + 1];
	const char *fname;
	size_t      sz;

	NNI_ARG_UNUSED(objid); // only use global files

	if ((objtype > ZT_STATE_OBJECT_NETWORK_CONFIG) ||
	    ((fname = zt_files[(int) objtype]) == NULL)) {
		return;
	}

	// If we have no valid path, then we just use ephemeral data.
	if (strlen(ztn->zn_path) == 0) {
		void * ndata = NULL;
		void * odata = zt_ephemeral_state[objtype].data;
		size_t olen  = zt_ephemeral_state[objtype].len;
		if ((len >= 0) && ((ndata = nni_alloc(len)) != NULL)) {
			memcpy(ndata, data, len);
		}
		zt_ephemeral_state[objtype].data = ndata;
		zt_ephemeral_state[objtype].len  = len;

		if (olen > 0) {
			nni_free(odata, olen);
		}
		return;
	}

	sz = sizeof(path);
	if (snprintf(path, sz, "%s%s%s", ztn->zn_path, pathsep, fname) >= sz) {
		// If the path is too long, we can't cope.  We
		// just decline to store anything.
		return;
	}

	// We assume that everyone can do standard C I/O.
	// This may be a bad assumption.  If that's the case,
	// the platform should supply an alternative
	// implementation. We are also assuming that we don't
	// need to worry about atomic updates.  As these items
	// (keys, etc.)  pretty much don't change, this should
	// be fine.

	if (len < 0) {
		(void) unlink(path);
		return;
	}

	if ((file = fopen(path, "wb")) == NULL) {
		return;
	}

	if (fwrite(data, 1, len, file) != len) {
		fclose(file);
		(void) unlink(path);
	}
	(void) fclose(file);
}

static int
zt_state_get(ZT_Node *node, void *userptr, void *thr,
    enum ZT_StateObjectType objtype, const uint64_t objid[2], void *data,
    unsigned int len)
{
	FILE *      file;
	zt_node *   ztn = userptr;
	char        path[NNG_MAXADDRLEN + 1];
	const char *fname;
	int         nread;
	size_t      sz;

	NNI_ARG_UNUSED(objid); // we only use global files

	if ((objtype > ZT_STATE_OBJECT_NETWORK_CONFIG) ||
	    ((fname = zt_files[(int) objtype]) == NULL)) {
		return (-1);
	}

	// If no base directory, we are using ephemeral data.
	if (strlen(ztn->zn_path) == 0) {
		if (zt_ephemeral_state[objtype].data == NULL) {
			return (-1);
		}
		if (zt_ephemeral_state[objtype].len > len) {
			return (-1);
		}
		len = zt_ephemeral_state[objtype].len;
		memcpy(data, zt_ephemeral_state[objtype].data, len);
		return (len);
	}

	sz = sizeof(path);
	if (snprintf(path, sz, "%s%s%s", ztn->zn_path, pathsep, fname) >= sz) {
		// If the path is too long, we can't cope.
		return (-1);
	}

	// We assume that everyone can do standard C I/O.
	// This may be a bad assumption.  If that's the case,
	// the platform should supply an alternative
	// implementation. We are also assuming that we don't
	// need to worry about atomic updates.  As these items
	// (keys, etc.)  pretty much don't change, this should
	// be fine.

	if ((file = fopen(path, "rb")) == NULL) {
		return (-1);
	}

	// seek to end of file
	(void) fseek(file, 0, SEEK_END);
	if (ftell(file) > len) {
		fclose(file);
		return (-1);
	}
	(void) fseek(file, 0, SEEK_SET);

	nread = (int) fread(data, 1, len, file);
	(void) fclose(file);

	return (nread);
}

typedef struct zt_send_hdr {
	nni_sockaddr sa;
	size_t       len;
} zt_send_hdr;

static void
zt_wire_packet_send_cb(void *arg)
{
	// We don't actually care much about the results, we
	// just need to release the resources.
	nni_aio *    aio = arg;
	zt_send_hdr *hdr;

	hdr = nni_aio_get_data(aio);
	nni_free(hdr, hdr->len + sizeof(*hdr));
	nni_aio_fini_cb(aio);
}

// This function is called when ZeroTier desires to send a
// physical frame. The data is a UDP payload, the rest of the
// payload should be set over vanilla UDP.
static int
zt_wire_packet_send(ZT_Node *node, void *userptr, void *thr, int64_t socket,
    const struct sockaddr_storage *remaddr, const void *data, unsigned int len,
    unsigned int ttl)
{
	nni_aio *            aio;
	nni_sockaddr         addr;
	struct sockaddr_in * sin  = (void *) remaddr;
	struct sockaddr_in6 *sin6 = (void *) remaddr;
	zt_node *            ztn  = userptr;
	nni_plat_udp *       udp;
	uint16_t             port;
	char *               buf;
	zt_send_hdr *        hdr;

	// char                 abuf[64];

	NNI_ARG_UNUSED(thr);
	NNI_ARG_UNUSED(socket);
	NNI_ARG_UNUSED(ttl);

	// Kind of unfortunate, but we have to convert the
	// sockaddr to a neutral form, and then back again in
	// the platform layer.
	switch (sin->sin_family) {
	case AF_INET:
		addr.s_un.s_in.sa_family = NNG_AF_INET;
		addr.s_un.s_in.sa_port   = sin->sin_port;
		addr.s_un.s_in.sa_addr   = sin->sin_addr.s_addr;
		udp                      = ztn->zn_udp4;
		port                     = htons(sin->sin_port);
		// XXX:
		// inet_ntop(AF_INET, &sin->sin_addr, abuf,
		// sizeof(abuf));
		break;
	case AF_INET6:
		addr.s_un.s_in6.sa_family = NNG_AF_INET6;
		addr.s_un.s_in6.sa_port   = sin6->sin6_port;
		udp                       = ztn->zn_udp6;
		port                      = htons(sin6->sin6_port);
		memcpy(addr.s_un.s_in6.sa_addr, sin6->sin6_addr.s6_addr, 16);
		// XXX:
		// inet_ntop(AF_INET6, &sin6->sin6_addr, abuf,
		// sizeof(abuf));
		break;
	default:
		// No way to understand the address.
		return (-1);
	}

	if (nni_aio_init(&aio, zt_wire_packet_send_cb, NULL) != 0) {
		// Out of memory
		return (-1);
	}
	if ((buf = nni_alloc(sizeof(*hdr) + len)) == NULL) {
		nni_aio_fini(aio);
		return (-1);
	}

	hdr = (void *) buf;
	buf += sizeof(*hdr);

	memcpy(buf, data, len);
	nni_aio_set_data(aio, hdr);
	hdr->sa  = addr;
	hdr->len = len;

	aio->a_addr           = &hdr->sa;
	aio->a_niov           = 1;
	aio->a_iov[0].iov_buf = buf;
	aio->a_iov[0].iov_len = len;

	// printf("SENDING UDP FRAME TO %s %d\n", abuf, port);
	// This should be non-blocking/best-effort, so while
	// not great that we're holding the lock, also not
	// tragic.
	nni_aio_set_synch(aio);
	nni_plat_udp_send(udp, aio);

	return (0);
}

static struct ZT_Node_Callbacks zt_callbacks = {
	.version                      = 0,
	.statePutFunction             = zt_state_put,
	.stateGetFunction             = zt_state_get,
	.wirePacketSendFunction       = zt_wire_packet_send,
	.virtualNetworkFrameFunction  = zt_virtual_recv,
	.virtualNetworkConfigFunction = zt_virtual_config,
	.eventCallback                = zt_event_cb,
	.pathCheckFunction            = NULL,
	.pathLookupFunction           = NULL,
};

static void
zt_node_destroy(zt_node *ztn)
{
	nni_aio_stop(ztn->zn_rcv4_aio);
	nni_aio_stop(ztn->zn_rcv6_aio);

	// Wait for background thread to exit!
	nni_thr_fini(&ztn->zn_bgthr);

	if (ztn->zn_znode != NULL) {
		ZT_Node_delete(ztn->zn_znode);
	}

	if (ztn->zn_udp4 != NULL) {
		nni_plat_udp_close(ztn->zn_udp4);
	}
	if (ztn->zn_udp6 != NULL) {
		nni_plat_udp_close(ztn->zn_udp6);
	}

	if (ztn->zn_rcv4_buf != NULL) {
		nni_free(ztn->zn_rcv4_buf, zt_rcv_bufsize);
	}
	if (ztn->zn_rcv6_buf != NULL) {
		nni_free(ztn->zn_rcv6_buf, zt_rcv_bufsize);
	}
	nni_aio_fini(ztn->zn_rcv4_aio);
	nni_aio_fini(ztn->zn_rcv6_aio);
	nni_idhash_fini(ztn->zn_eps);
	nni_idhash_fini(ztn->zn_pipes);
	nni_idhash_fini(ztn->zn_peers);
	nni_cv_fini(&ztn->zn_bgcv);
	NNI_FREE_STRUCT(ztn);
}

static int
zt_node_create(zt_node **ztnp, const char *path)
{
	zt_node *          ztn;
	nng_sockaddr       sa4;
	nng_sockaddr       sa6;
	int                rv;
	enum ZT_ResultCode zrv;

	// We want to bind to any address we can (for now).
	// Note that at the moment we only support IPv4.  Its
	// unclear how we are meant to handle underlying IPv6
	// in ZeroTier.  Probably we can use IPv6 dual stock
	// sockets if they exist, but not all platforms support
	// dual-stack.  Furhtermore, IPv6 is not available
	// everywhere, and the root servers may be IPv4 only.
	memset(&sa4, 0, sizeof(sa4));
	sa4.s_un.s_in.sa_family = NNG_AF_INET;
	memset(&sa6, 0, sizeof(sa6));
	sa6.s_un.s_in6.sa_family = NNG_AF_INET6;

	if ((ztn = NNI_ALLOC_STRUCT(ztn)) == NULL) {
		return (NNG_ENOMEM);
	}
	NNI_LIST_INIT(&ztn->zn_eplist, zt_ep, ze_link);
	NNI_LIST_INIT(&ztn->zn_plist, zt_pipe, zp_link);
	nni_cv_init(&ztn->zn_bgcv, &zt_lk);
	nni_aio_init(&ztn->zn_rcv4_aio, zt_node_rcv4_cb, ztn);
	nni_aio_init(&ztn->zn_rcv6_aio, zt_node_rcv6_cb, ztn);

	if (((ztn->zn_rcv4_buf = nni_alloc(zt_rcv_bufsize)) == NULL) ||
	    ((ztn->zn_rcv6_buf = nni_alloc(zt_rcv_bufsize)) == NULL)) {
		zt_node_destroy(ztn);
		return (NNG_ENOMEM);
	}
	if (((rv = nni_idhash_init(&ztn->zn_ports)) != 0) ||
	    ((rv = nni_idhash_init(&ztn->zn_eps)) != 0) ||
	    ((rv = nni_idhash_init(&ztn->zn_pipes)) != 0) ||
	    ((rv = nni_idhash_init(&ztn->zn_peers)) != 0) ||
	    ((rv = nni_thr_init(&ztn->zn_bgthr, zt_bgthr, ztn)) != 0) ||
	    ((rv = nni_plat_udp_open(&ztn->zn_udp4, &sa4)) != 0) ||
	    ((rv = nni_plat_udp_open(&ztn->zn_udp6, &sa6)) != 0)) {
		zt_node_destroy(ztn);
		return (rv);
	}

	// Setup for dynamic ephemeral port allocations.  We
	// set the range to allow for ephemeral ports, but not
	// higher than the max port, and starting with an
	// initial random value.  Note that this should give us
	// about 8 million possible ephemeral ports.
	nni_idhash_set_limits(ztn->zn_ports, zt_ephemeral, zt_max_port,
	    (nni_random() % (zt_max_port - zt_ephemeral)) + zt_ephemeral);

	nni_strlcpy(ztn->zn_path, path, sizeof(ztn->zn_path));
	zrv = ZT_Node_new(&ztn->zn_znode, ztn, NULL, &zt_callbacks, zt_now());
	if (zrv != ZT_RESULT_OK) {
		zt_node_destroy(ztn);
		return (zt_result(zrv));
	}

	nni_list_append(&zt_nodes, ztn);

	ztn->zn_self = ZT_Node_address(ztn->zn_znode);

	nni_thr_run(&ztn->zn_bgthr);

	// Schedule an initial background run.
	zt_node_resched(ztn, 1);

	// Schedule receive
	ztn->zn_rcv4_aio->a_niov           = 1;
	ztn->zn_rcv4_aio->a_iov[0].iov_buf = ztn->zn_rcv4_buf;
	ztn->zn_rcv4_aio->a_iov[0].iov_len = zt_rcv_bufsize;
	ztn->zn_rcv4_aio->a_addr           = &ztn->zn_rcv4_addr;
	ztn->zn_rcv4_aio->a_count          = 0;
	ztn->zn_rcv6_aio->a_niov           = 1;
	ztn->zn_rcv6_aio->a_iov[0].iov_buf = ztn->zn_rcv6_buf;
	ztn->zn_rcv6_aio->a_iov[0].iov_len = zt_rcv_bufsize;
	ztn->zn_rcv6_aio->a_addr           = &ztn->zn_rcv6_addr;
	ztn->zn_rcv6_aio->a_count          = 0;

	nni_plat_udp_recv(ztn->zn_udp4, ztn->zn_rcv4_aio);
	nni_plat_udp_recv(ztn->zn_udp6, ztn->zn_rcv6_aio);

	*ztnp = ztn;
	return (0);
}

static int
zt_node_find(zt_ep *ep)
{
	zt_node *                ztn;
	int                      rv;
	nng_sockaddr             sa;
	ZT_VirtualNetworkConfig *cf;

	NNI_LIST_FOREACH (&zt_nodes, ztn) {
		if (strcmp(ep->ze_home, ztn->zn_path) == 0) {
			goto done;
		}
	}

	// We didn't find a node, so make one.  And try to
	// initialize it.
	if ((rv = zt_node_create(&ztn, ep->ze_home)) != 0) {
		return (rv);
	}

done:

	ep->ze_ztn = ztn;
	if (nni_list_node_active(&ep->ze_link)) {
		nni_list_node_remove(&ep->ze_link);
	}
	nni_list_append(&ztn->zn_eplist, ep);

	(void) ZT_Node_join(ztn->zn_znode, ep->ze_nwid, ztn, NULL);

	if ((cf = ZT_Node_networkConfig(ztn->zn_znode, ep->ze_nwid)) != NULL) {
		NNI_ASSERT(cf->nwid == ep->ze_nwid);
		ep->ze_maxmtu = cf->mtu;
		ep->ze_phymtu = cf->physicalMtu;
		ZT_Node_freeQueryResult(ztn->zn_znode, cf);
	}

	return (0);
}

static int
zt_chkopt(int opt, const void *dat, size_t sz)
{
	if (opt == nng_optid_recvmaxsz) {
		// We cannot deal with message sizes larger
		// than 64k.
		return (nni_chkopt_size(dat, sz, 0, 0xffffffffU));
	}
	if (opt == nng_optid_zt_home) {
		size_t l = nni_strnlen(dat, sz);
		if ((l >= sz) || (l >= NNG_MAXADDRLEN)) {
			return (NNG_EINVAL);
		}
		// XXX: should we apply additional security
		// checks? home path is not null terminated
		return (0);
	}
	return (NNG_ENOTSUP);
}

static int
zt_tran_init(void)
{
	int rv;
	if (((rv = nni_option_register(nng_opt_zt_home, &nng_optid_zt_home)) !=
	        0) ||
	    ((rv = nni_option_register(nng_opt_zt_node, &nng_optid_zt_node)) !=
	        0) ||
	    ((rv = nni_option_register(nng_opt_zt_nwid, &nng_optid_zt_nwid)) !=
	        0)) {
		return (rv);
	}
	nni_mtx_init(&zt_lk);
	NNI_LIST_INIT(&zt_nodes, zt_node, zn_link);
	return (0);
}

static void
zt_tran_fini(void)
{
	nng_optid_zt_home = -1;
	nng_optid_zt_nwid = -1;
	nng_optid_zt_node = -1;
	zt_node *ztn;

	nni_mtx_lock(&zt_lk);
	while ((ztn = nni_list_first(&zt_nodes)) != 0) {
		nni_list_remove(&zt_nodes, ztn);
		ztn->zn_closed = 1;
		nni_cv_wake(&ztn->zn_bgcv);
		nni_mtx_unlock(&zt_lk);

		zt_node_destroy(ztn);

		nni_mtx_lock(&zt_lk);
	}
	nni_mtx_unlock(&zt_lk);

	for (int i = 0; i < ZT_STATE_OBJECT_NETWORK_CONFIG; i++) {
		if (zt_ephemeral_state[i].len > 0) {
			nni_free(zt_ephemeral_state[i].data,
			    zt_ephemeral_state[i].len);
		}
	}
	NNI_ASSERT(nni_list_empty(&zt_nodes));
	nni_mtx_fini(&zt_lk);
}

static void
zt_pipe_close(void *arg)
{
	zt_pipe *p = arg;
	nni_aio *aio;

	printf("PIPE CLOSE CALLED!\n");
	nni_mtx_lock(&zt_lk);
	p->zp_closed = 1;
	if ((aio = p->zp_user_rxaio) != NULL) {
		p->zp_user_rxaio = NULL;
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}
	zt_pipe_send_disc_req(p);
	nni_mtx_unlock(&zt_lk);
	printf("PIPE CLOSE DONE\n");
}

static void
zt_pipe_fini(void *arg)
{
	zt_pipe *p   = arg;
	zt_node *ztn = p->zp_ztn;

	nni_aio_stop(p->zp_rxaio);

	// This tosses the connection details and all state.
	nni_mtx_lock(&zt_lk);
	nni_idhash_remove(ztn->zn_ports, p->zp_laddr & zt_port_mask);
	nni_idhash_remove(ztn->zn_pipes, p->zp_laddr);
	nni_idhash_remove(ztn->zn_peers, p->zp_raddr);
	nni_mtx_unlock(&zt_lk);

	for (int i = 0; i < zt_recvq; i++) {
		zt_fraglist_free(&p->zp_recvq[i]);
	}

	NNI_FREE_STRUCT(p);
}

static void
zt_pipe_recv_cb(void *arg)
{
}

static int
zt_pipe_init(zt_pipe **pipep, zt_ep *ep, uint64_t raddr, uint64_t laddr)
{
	zt_pipe *p;
	uint64_t port = 0;
	int      rv;
	zt_node *ztn = ep->ze_ztn;
	int      i;
	size_t   maxfrag;
	size_t   maxfrags;

	if ((p = NNI_ALLOC_STRUCT(p)) == NULL) {
		return (NNG_ENOMEM);
	}
	p->zp_ztn        = ztn;
	p->zp_raddr      = raddr;
	p->zp_proto      = ep->ze_proto;
	p->zp_nwid       = ep->ze_nwid;
	p->zp_raddr      = raddr;
	p->zp_mtu        = ep->ze_phymtu;
	p->zp_rcvmax     = ep->ze_rcvmax;
	p->zp_next_msgid = (uint16_t) nni_random();

	if (laddr == 0) {
		// Locate a suitable port number.
		if ((rv = nni_idhash_alloc(ztn->zn_ports, &port, p)) != 0) {
			NNI_FREE_STRUCT(p);
			return (rv);
		}

		// Now we stash the port here.  We use the same address
		// as the endpoint, but we strip out the port number
		// and overwrite with our own.
		p->zp_laddr = ep->ze_laddr;
		p->zp_laddr >>= 24;
		p->zp_laddr <<= 24;
		p->zp_laddr |= port;
	} else {
		p->zp_laddr = laddr;
	}

	if (((rv = nni_aio_init(&p->zp_rxaio, zt_pipe_recv_cb, p)) != 0) ||
	    ((rv = nni_idhash_insert(ztn->zn_pipes, p->zp_laddr, p)) != 0) ||
	    ((rv = nni_idhash_insert(ztn->zn_peers, p->zp_raddr, p)) != 0)) {
		zt_pipe_fini(p);
	}

	// the largest fragment we can accept on this pipe
	maxfrag = p->zp_mtu - zt_offset_data_data;
	// and the larger fragment count we can accept on this pipe
	// (round up)
	maxfrags = (p->zp_rcvmax + (maxfrag - 1)) / maxfrag;

	for (i = 0; i < zt_recvq; i++) {
		zt_fraglist *fl  = &p->zp_recvq[i];
		fl->fl_time      = NNI_TIME_ZERO;
		fl->fl_msgid     = 0;
		fl->fl_ready     = 0;
		fl->fl_missingsz = (maxfrags + 7) / 8;
		fl->fl_missing   = nni_alloc(fl->fl_missingsz);
		if (fl->fl_missing == NULL) {
			zt_pipe_fini(p);
			return (NNG_ENOMEM);
		}
	}

	*pipep = p;
	return (0);
}

static void
zt_pipe_send(void *arg, nni_aio *aio)
{
	// As we are sending UDP, and there is no callback to worry
	// about, we just go ahead and send out a stream of messages
	// synchronously.
	zt_pipe *p = arg;
	size_t   offset;
	size_t   len;
	int      i;
	uint8_t  data[ZT_MAX_MTU];
	uint16_t id;
	uint16_t nfrags;
	uint16_t fragno;
	uint16_t fragsz;
	size_t   bytes;

	nni_mtx_lock(&zt_lk);
	if (nni_aio_start(aio, NULL, p) != 0) {
		nni_mtx_unlock(&zt_lk);
		return;
	}

	if (p->zp_closed) {
		nni_aio_finish_error(aio, NNG_ECLOSED);
		nni_mtx_unlock(&zt_lk);
		return;
	}

	fragsz = (uint16_t)(p->zp_mtu - zt_offset_data_data);

	bytes = 0;
	for (i = 0; i < aio->a_niov; i++) {
		bytes += aio->a_iov[i].iov_len;
	}
	if (bytes >= (0xfffe * fragsz)) {
		nni_aio_finish_error(aio, NNG_EMSGSIZE);
		nni_mtx_unlock(&zt_lk);
		return;
	}
	// above check means nfrags will fit in 16-bits.
	nfrags = (uint16_t)((bytes + (fragsz - 1)) / fragsz);

	// get the next message ID, but skip 0
	if ((id = p->zp_next_msgid++) == 0) {
		id = p->zp_next_msgid++;
	}

	offset = 0;
	while (aio->a_niov != 0) {
		// We send in chunks.  Each chunk is at most the
		// optimimum physical MTU minus the room we need for
		// the headers.
		len = fragsz;
		if (aio->a_iov[0].iov_len > len) {
			memcpy(data + zt_offset_data_data,
			    aio->a_iov[0].iov_buf, len);
			aio->a_iov[0].iov_buf += len;
		} else {
			len = aio->a_iov[0].iov_len;
			memcpy(data + zt_offset_data_data,
			    aio->a_iov[0].iov_buf, len);
			aio->a_niov--;
			for (int i = 0; i < aio->a_niov; i++) {
				aio->a_iov[i] = aio->a_iov[i + 1];
			}
			NNI_ASSERT(fragno + 1 == nfrags);
		}
		NNI_PUT16(data + zt_offset_data_id, id);
		NNI_PUT16(data + zt_offset_data_fragsz, fragsz);
		NNI_PUT16(data + zt_offset_data_frag, fragno++);
		NNI_PUT16(data + zt_offset_data_nfrag, nfrags);
		offset += len;
		zt_send(p->zp_ztn, p->zp_nwid, zt_op_data, p->zp_raddr,
		    p->zp_laddr, data, len);
	}
	nni_aio_finish(aio, 0, offset);
	nni_mtx_unlock(&zt_lk);
}

static void
zt_pipe_cancel_recv(nni_aio *aio, int rv)
{
	zt_pipe *p = aio->a_prov_data;
	printf("CANCEL RECV START\n");
	nni_mtx_lock(&zt_lk);
	if (p->zp_user_rxaio != aio) {
		nni_mtx_unlock(&zt_lk);
		printf("NOT OUR RECV TO CANCEL\n");
	}
	p->zp_user_rxaio = NULL;
	nni_mtx_unlock(&zt_lk);
	nni_aio_finish_error(aio, rv);
	printf("CANCEL RECV DONE\n");
}

static void
zt_fraglist_clear(zt_fraglist *fl)
{
	nni_msg *msg;

	fl->fl_ready = 0;
	fl->fl_msgid = 0;
	fl->fl_time  = NNI_TIME_ZERO;
	if ((msg = fl->fl_msg) != NULL) {
		fl->fl_msg = NULL;
		nni_msg_free(msg);
	}
	memset(fl->fl_missing, 0, fl->fl_missingsz);
}

static void
zt_fraglist_free(zt_fraglist *fl)
{
	zt_fraglist_clear(fl);
	nni_free(fl->fl_missing, fl->fl_missingsz);
	fl->fl_missing = NULL;
}

static void
zt_pipe_dorecv(zt_pipe *p)
{
	nni_aio *aio = p->zp_user_rxaio;
	nni_time now = nni_clock();

	if (aio == NULL) {
		return;
	}

	for (int i = 0; i < zt_recvq; i++) {
		zt_fraglist *fl = &p->zp_recvq[i];
		nni_msg *    msg;

		if (now > (fl->fl_time + zt_recv_stale)) {
			// fragment list is stale, clean it.
			zt_fraglist_clear(fl);
			continue;
		}
		if (!fl->fl_ready) {
			continue;
		}

		// Got data.  Let's pass it up.
		msg        = fl->fl_msg;
		fl->fl_msg = NULL;
		NNI_ASSERT(msg != NULL);
		nni_aio_finish_msg(aio, msg);
		zt_fraglist_clear(fl);
		return;
	}
}

static void
zt_pipe_recv(void *arg, nni_aio *aio)
{
	zt_pipe *p = arg;

	nni_mtx_lock(&zt_lk);
	if (nni_aio_start(aio, zt_pipe_cancel_recv, p) != 0) {
		nni_mtx_unlock(&zt_lk);
		return;
	}
	if (p->zp_closed) {
		nni_aio_finish_error(aio, NNG_ECLOSED);
	} else {
		p->zp_user_rxaio = aio;
		zt_pipe_dorecv(p);
	}
	nni_mtx_unlock(&zt_lk);
}

static uint16_t
zt_pipe_peer(void *arg)
{
	zt_pipe *pipe = arg;

	return (pipe->zp_peer);
}

static int
zt_pipe_getopt(void *arg, int option, void *buf, size_t *szp)
{
	return (NNG_ENOTSUP);
}

static void
zt_pipe_start(void *arg, nni_aio *aio)
{
	zt_pipe *p = arg;

	nni_mtx_lock(&zt_lk);
	// XXX: send a gratuitous ping, and start the ping
	// interval timer.
	// zt_pipe_send_ping(p);
	nni_aio_finish(aio, 0, 0);
	nni_mtx_unlock(&zt_lk);
}

static void
zt_ep_fini(void *arg)
{
	zt_ep *ep = arg;
	nni_aio_stop(ep->ze_creq_aio);
	nni_aio_fini(ep->ze_creq_aio);
	NNI_FREE_STRUCT(ep);
}

static int
zt_parsehex(const char **sp, uint64_t *valp, int wildok)
{
	int         n;
	const char *s = *sp;
	char        c;
	uint64_t    v;

	if (wildok && *s == '*') {
		*valp = 0;
		s++;
		*sp = s;
		return (0);
	}

	for (v = 0, n = 0; (n < 16) && isxdigit(c = tolower(*s)); n++, s++) {
		v *= 16;
		if (isdigit(c)) {
			v += (c - '0');
		} else {
			v += ((c - 'a') + 10);
		}
	}

	*sp   = s;
	*valp = v;
	return (n ? 0 : NNG_EINVAL);
}

static int
zt_parsedec(const char **sp, uint64_t *valp)
{
	int         n;
	const char *s = *sp;
	char        c;
	uint64_t    v;

	for (v = 0, n = 0; (n < 20) && isdigit(c = *s); n++, s++) {
		v *= 10;
		v += (c - '0');
	}
	*sp   = s;
	*valp = v;
	return (n ? 0 : NNG_EINVAL);
}

static int
zt_ep_init(void **epp, const char *url, nni_sock *sock, int mode)
{
	zt_ep *     ep;
	size_t      sz;
	uint64_t    nwid;
	uint64_t    node;
	uint64_t    port;
	int         n;
	int         rv;
	char        c;
	const char *u;

	if ((ep = NNI_ALLOC_STRUCT(ep)) == NULL) {
		return (NNG_ENOMEM);
	}

	// URL parsing...
	// URL is form zt://<nwid>[/<remoteaddr>]:<port>
	// The <remoteaddr> part is required for remote
	// dialers, but is not used at all for listeners.  (We
	// have no notion of binding to different node
	// addresses.)
	ep->ze_mode   = mode;
	ep->ze_maxmtu = ZT_MAX_MTU;
	ep->ze_phymtu = ZT_MIN_MTU;
	ep->ze_aio    = NULL;
	ep->ze_proto  = nni_sock_proto(sock);
	sz            = sizeof(ep->ze_url);

	nni_aio_list_init(&ep->ze_aios);

	if ((strncmp(url, "zt://", strlen("zt://")) != 0) ||
	    (nni_strlcpy(ep->ze_url, url, sz) >= sz)) {
		zt_ep_fini(ep);
		return (NNG_EADDRINVAL);
	}
	rv = nni_aio_init(&ep->ze_creq_aio, zt_ep_conn_req_cb, ep);
	if (rv != 0) {
		zt_ep_fini(ep);
		return (rv);
	}

	u = url + strlen("zt://");
	// Parse the URL.

	switch (mode) {
	case NNI_EP_MODE_DIAL:
		// We require zt://<nwid>/<remotenode>:<port>
		// The remote node must be a 40 bit address
		// (max), and we require a non-zero port to
		// connect to.
		if ((zt_parsehex(&u, &nwid, 0) != 0) || (*u++ != '/') ||
		    (zt_parsehex(&u, &node, 1) != 0) ||
		    (node > 0xffffffffffull) || (*u++ != ':') ||
		    (zt_parsedec(&u, &port) != 0) || (*u != '\0') ||
		    (port > zt_max_port) || (port == 0)) {
			return (NNG_EADDRINVAL);
		}
		ep->ze_raddr = node;
		ep->ze_raddr <<= 24;
		ep->ze_raddr |= port;
		ep->ze_laddr = 0;
		break;
	case NNI_EP_MODE_LISTEN:
		// Listen mode is just zt://<nwid>:<port>.  The
		// port may be zero in this case, to indicate
		// that the server should allocate an ephemeral
		// port.  We do allow the same form of URL including
		// the node address, but that must be zero, a wild card,
		// or our own node address.
		if (zt_parsehex(&u, &nwid, 0) != 0) {
			return (NNG_EADDRINVAL);
		}
		node = 0;
		// Look for optional node address.
		if (*u == '/') {
			u++;
			if (zt_parsehex(&u, &node, 1) != 0) {
				return (NNG_EADDRINVAL);
			}
		}
		if ((*u++ != ':') || (zt_parsedec(&u, &port) != 0) ||
		    (*u != '\0') || (port > zt_max_port)) {
			return (NNG_EADDRINVAL);
		}
		ep->ze_laddr = node;
		ep->ze_laddr <<= 24;
		ep->ze_laddr |= port;
		ep->ze_raddr = 0;
		break;
	default:
		NNI_ASSERT(0);
		break;
	}

	ep->ze_nwid = nwid;

	nni_mtx_lock(&zt_lk);
	rv = zt_node_find(ep);
	nni_mtx_unlock(&zt_lk);

	if (rv != 0) {
		zt_ep_fini(ep);
		return (rv);
	}

	*epp = ep;
	return (0);
}

static void
zt_ep_close(void *arg)
{
	zt_ep *  ep = arg;
	zt_node *ztn;
	nni_aio *aio;

	nni_aio_cancel(ep->ze_creq_aio, NNG_ECLOSED);

	// Cancel any outstanding user operation(s) - they should have
	// been aborted by the above cancellation, but we need to be
	// sure, as the cancellation callback may not have run yet.

	nni_mtx_lock(&zt_lk);
	while ((aio = nni_list_first(&ep->ze_aios)) != NULL) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}

	// Endpoint framework guarantees to only call us once,
	// and to not call other things while we are closed.
	ztn = ep->ze_ztn;
	// If we're on the ztn node list, pull us off.
	if (ztn != NULL) {
		nni_list_node_remove(&ep->ze_link);
		nni_idhash_remove(ztn->zn_ports, ep->ze_laddr & zt_port_mask);
		nni_idhash_remove(ztn->zn_eps, ep->ze_laddr);
	}

	// XXX: clean up the pipe if a dialer

	nni_mtx_unlock(&zt_lk);
}

static int
zt_ep_bind_locked(zt_ep *ep)
{
	int      rv;
	uint64_t port;
	uint64_t node;
	zt_node *ztn;

	// If we haven't already got a ZT node, get one.
	if ((ztn = ep->ze_ztn) == NULL) {
		if ((rv = zt_node_find(ep)) != 0) {
			return (rv);
		}
		ztn = ep->ze_ztn;
	}

	node = ep->ze_laddr >> 24;
	if ((node != 0) && (node != ztn->zn_self)) {
		// User requested node id, but it doesn't match our own.
		return (NNG_EADDRINVAL);
	}

	if ((ep->ze_laddr & zt_port_mask) == 0) {
		// ask for an ephemeral port
		if ((rv = nni_idhash_alloc(ztn->zn_ports, &port, ep)) != 0) {
			return (rv);
		}
		NNI_ASSERT(port & zt_ephemeral);
	} else {
		void *conflict;
		// make sure port requested is free.
		port = ep->ze_laddr & zt_port_mask;

		if (nni_idhash_find(ztn->zn_ports, port, &conflict) == 0) {
			return (NNG_EADDRINUSE);
		}
		if ((rv = nni_idhash_insert(ztn->zn_ports, port, ep)) != 0) {
			return (rv);
		}
	}
	NNI_ASSERT(port <= zt_max_port);
	NNI_ASSERT(port > 0);

	ep->ze_laddr = ztn->zn_self;
	ep->ze_laddr <<= 24;
	ep->ze_laddr |= port;

	if ((rv = nni_idhash_insert(ztn->zn_eps, ep->ze_laddr, ep)) != 0) {
		nni_idhash_remove(ztn->zn_ports, port);
		return (rv);
	}

	return (0);
}

static int
zt_ep_bind(void *arg)
{
	int    rv;
	zt_ep *ep = arg;

	nni_mtx_lock(&zt_lk);
	rv = zt_ep_bind_locked(ep);
	nni_mtx_unlock(&zt_lk);

	return (rv);
}

static void
zt_ep_cancel(nni_aio *aio, int rv)
{
	zt_ep *ep = aio->a_prov_data;

	nni_mtx_lock(&zt_lk);
	if (nni_aio_list_active(aio)) {
		if (ep->ze_aio != NULL) {
			nni_aio_cancel(ep->ze_aio, rv);
		}
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&zt_lk);
}

static void
zt_ep_doaccept(zt_ep *ep)
{
	// Call with ep lock held.
	nni_time now;
	zt_pipe *p;
	int      rv;

	now = nni_clock();
	// Consume any timedout connect requests.
	while (ep->ze_creq_tail != ep->ze_creq_head) {
		zt_creq  creq;
		nni_aio *aio;

		creq = ep->ze_creqs[ep->ze_creq_tail % zt_listenq];
		// Discard old connection requests.
		if (creq.cr_expire < now) {
			ep->ze_creq_tail++;
			continue;
		}

		if ((aio = nni_list_first(&ep->ze_aios)) == NULL) {
			// No outstanding accept.  We're done.
			break;
		}

		// We have both a connection request, and a
		// place to accept it.

		// Advance the tail.
		ep->ze_creq_tail++;

		// We remove this AIO.  This keeps it from
		// being canceled.
		nni_aio_list_remove(aio);

		rv = zt_pipe_init(&p, ep, creq.cr_raddr, 0);
		if (rv != 0) {
			zt_send_err(ep->ze_ztn, ep->ze_nwid, creq.cr_raddr,
			    ep->ze_laddr, zt_err_unknown,
			    "Failed creating pipe");
			nni_aio_finish_error(aio, rv);
			continue;
		}
		p->zp_peer = creq.cr_proto;

		zt_pipe_send_conn_ack(p);
		printf("FINISHING WITH A GOOD PIPE!\n");
		nni_aio_finish_pipe(aio, p);
	}
}

static void
zt_ep_accept(void *arg, nni_aio *aio)
{
	zt_ep *ep = arg;

	nni_mtx_lock(&zt_lk);
	if (nni_aio_start(aio, zt_ep_cancel, ep) == 0) {
		nni_aio_list_append(&ep->ze_aios, aio);
		zt_ep_doaccept(ep);
	}
	nni_mtx_unlock(&zt_lk);
}

static void
zt_ep_conn_req_cancel(nni_aio *aio, int rv)
{
	// We don't have much to do here.  The AIO will have been
	// canceled as a result of the "parent" AIO canceling.
	zt_ep *ep       = aio->a_prov_data;
	ep->ze_creq_try = 0;
	nni_aio_finish_error(aio, rv);
}

static void
zt_ep_conn_req_cb(void *arg)
{
	zt_ep *  ep = arg;
	zt_pipe *p;
	nni_aio *aio = ep->ze_creq_aio;
	nni_aio *uaio;
	int      rv;

	NNI_ASSERT(ep->ze_mode == NNI_EP_MODE_DIAL);

	nni_mtx_lock(&zt_lk);
	rv = nni_aio_result(aio);
	switch (rv) {
	case 0:
		// Already canceled, or already handled?
		if (((uaio = nni_list_first(&ep->ze_aios)) == NULL) ||
		    ((p = nni_aio_get_pipe(aio)) == NULL)) {
			nni_mtx_unlock(&zt_lk);
			return;
		}
		ep->ze_creq_try = 0;
		nni_aio_list_remove(uaio);
		nni_aio_finish_pipe(uaio, p);
		nni_mtx_unlock(&zt_lk);
		return;

	case NNG_ETIMEDOUT:
		if (ep->ze_creq_try <= zt_conn_attempts) {
			// Timed out, but we can try again.
			ep->ze_creq_try++;
			nni_aio_set_timeout(
			    aio, nni_clock() + zt_conn_interval);
			nni_aio_start(aio, zt_ep_conn_req_cancel, ep);
			zt_ep_send_conn_req(ep);
			nni_mtx_unlock(&zt_lk);
			return;
		}
	}

	// These are failure modes.  Either we timed out too many
	// times, or an error occurred.

	ep->ze_creq_try = 0;
	while ((uaio = nni_list_first(&ep->ze_aios)) != NULL) {
		nni_aio_list_remove(uaio);
		nni_aio_finish_error(uaio, rv);
	}
	nni_mtx_unlock(&zt_lk);
}

static void
zt_ep_connect(void *arg, nni_aio *aio)
{
	zt_ep *  ep = arg;
	uint64_t port;
	int      rv;

	// We bind locally.  We'll use the address later when we give
	// it to the pipe, but this allows us to receive the initial
	// ack back from the server.  (This gives us an ephemeral
	// address to work with.)

	nni_mtx_lock(&zt_lk);

	if (nni_aio_start(aio, zt_ep_cancel, ep) == 0) {
		nni_time now = nni_clock();

		if ((rv = zt_ep_bind_locked(ep)) != 0) {
			nni_aio_finish_error(aio, rv);
			nni_mtx_unlock(&zt_lk);
			return;
		}

		nni_aio_list_append(&ep->ze_aios, aio);

		ep->ze_creq_try = 1;

		nni_aio_set_timeout(ep->ze_creq_aio, now + zt_conn_interval);
		// This can't fail -- the only way the ze_creq_aio gets
		// terminated would have required us to have also
		// canceled the user AIO and held the lock.
		(void) nni_aio_start(
		    ep->ze_creq_aio, zt_ep_conn_req_cancel, ep);

		// We send out the first connect message; it we are not
		// yet attached to the network the message will be
		// dropped.
		zt_ep_send_conn_req(ep);
	}
	nni_mtx_unlock(&zt_lk);
}

static int
zt_ep_setopt(void *arg, int opt, const void *data, size_t size)
{
	zt_ep *ep = arg;
	int    i;
	int    rv = NNG_ENOTSUP;

	if (opt == nng_optid_recvmaxsz) {
		nni_mtx_lock(&zt_lk);
		rv = nni_setopt_size(
		    &ep->ze_rcvmax, data, size, 0, 0xffffffffu);
		nni_mtx_unlock(&zt_lk);
	} else if (opt == nng_optid_zt_home) {
		// XXX: check to make sure not started...
		i = nni_strnlen((const char *) data, size);
		if ((i >= size) || (i >= NNG_MAXADDRLEN)) {
			return (NNG_EINVAL);
		}
		nni_mtx_lock(&zt_lk);
		nni_strlcpy(ep->ze_home, data, sizeof(ep->ze_home));
		rv = zt_node_find(ep);
		if (rv != 0) {
			ep->ze_ztn = NULL;
		}
		nni_mtx_unlock(&zt_lk);
		rv = 0;
	}
	return (rv);
}

static int
zt_ep_getopt(void *arg, int opt, void *data, size_t *sizep)
{
	zt_ep *ep = arg;
	int    rv = NNG_ENOTSUP;

	if (opt == nng_optid_recvmaxsz) {
		nni_mtx_lock(&zt_lk);
		rv = nni_getopt_size(&ep->ze_rcvmax, data, sizep);
		nni_mtx_unlock(&zt_lk);
	} else if (opt == nng_optid_zt_home) {
		nni_mtx_lock(&zt_lk);
		rv = nni_getopt_str(ep->ze_home, data, sizep);
		nni_mtx_unlock(&zt_lk);
	} else if (opt == nng_optid_zt_node) {
		nni_mtx_lock(&zt_lk);
		rv = nni_getopt_u64(ep->ze_ztn->zn_self, data, sizep);
		nni_mtx_unlock(&zt_lk);
	} else if (opt == nng_optid_zt_nwid) {
		nni_mtx_lock(&zt_lk);
		rv = nni_getopt_u64(ep->ze_nwid, data, sizep);
		nni_mtx_unlock(&zt_lk);
	}
	return (rv);
}

static nni_tran_pipe zt_pipe_ops = {
	.p_fini   = zt_pipe_fini,
	.p_start  = zt_pipe_start,
	.p_send   = zt_pipe_send,
	.p_recv   = zt_pipe_recv,
	.p_close  = zt_pipe_close,
	.p_peer   = zt_pipe_peer,
	.p_getopt = zt_pipe_getopt,
};

static nni_tran_ep zt_ep_ops = {
	.ep_init    = zt_ep_init,
	.ep_fini    = zt_ep_fini,
	.ep_connect = zt_ep_connect,
	.ep_bind    = zt_ep_bind,
	.ep_accept  = zt_ep_accept,
	.ep_close   = zt_ep_close,
	.ep_setopt  = zt_ep_setopt,
	.ep_getopt  = zt_ep_getopt,
};

// This is the ZeroTier transport linkage, and should be the
// only global symbol in this entire file.
static struct nni_tran zt_tran = {
	.tran_version = NNI_TRANSPORT_VERSION,
	.tran_scheme  = "zt",
	.tran_ep      = &zt_ep_ops,
	.tran_pipe    = &zt_pipe_ops,
	.tran_chkopt  = zt_chkopt,
	.tran_init    = zt_tran_init,
	.tran_fini    = zt_tran_fini,
};

int
nng_zt_register(void)
{
	return (nni_tran_register(&zt_tran));
}

#endif