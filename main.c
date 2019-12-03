/* vim: set noet: */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>
#include <inttypes.h>
#include <endian.h>
#include <byteswap.h>
#include <getopt.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <errno.h>
#include <sched.h>

#include <infiniband/verbs.h>

#include "get_clock.h"
#include "sockets.h"


#define DEBUG 0
#define debug_print(fmt, ...) \
	if (DEBUG) { fprintf(stdout, fmt, ##__VA_ARGS__); }

#define data_print(fmt, ...) \
	do { if (!DEBUG) fprintf(stdout, fmt, ##__VA_ARGS__); } while (0)

/* poll CQ timeout in millisec (2 seconds) */
#define MAX_POLL_CQ_TIMEOUT 2000

/* Parameters for cache probing size */
#define CLIENT_MSG_SIZE 64
#define SERVER_COLUMN_COUNT 16384
#define SERVER_ROW_COUNT 128

#define ITERS 10000

#if __BYTE_ORDER == __LITTLE_ENDIAN
	static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
	static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
	static inline uint64_t htonll(uint64_t x) { return x; }
	static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif

#define CACHE_SIZE 64
#define CACHE_LINES (SERVER_ROW_COUNT * SERVER_COLUMN_COUNT / CACHE_SIZE)
#define BM_BITS_PER_WORD (sizeof(uint64_t) * CHAR_BIT)
#define BM_WORDS (CACHE_LINES / BM_BITS_PER_WORD)

uint64_t bm[BM_WORDS] = {0};
#define WORD_OFFSET(b) ((b) / BM_BITS_PER_WORD)
#define BIT_OFFSET(b)  ((b) % BM_BITS_PER_WORD)

void bm_set(unsigned int addr) {
	bm[WORD_OFFSET(addr)] |= 1ull << BIT_OFFSET(addr);
}
void bm_clear(unsigned int addr) {
	bm[WORD_OFFSET(addr)] &= ~(1ull << BIT_OFFSET(addr));
}

bool bm_read(unsigned int addr) {
	return (bm[WORD_OFFSET(addr)] & (1ull << BIT_OFFSET(addr))) != 0;
}

unsigned int rand_line() {
	unsigned int r;
	while (bm_read(r = rand() % CACHE_LINES));
	bm_set(r);
	return r;
}


/* structure of test parameters */
struct config_t {
	const char	*dev_name;	/* IB device name */
	char		*server_name;	/* server host name */
	u_int32_t	tcp_port;	/* server TCP port */
	int		ib_port;	/* local IB port to work with */
	int		gid_idx;	/* gid index to use */
	unsigned int	iters;		/* number of iterations */
	int		mode; /* 0 for seq, 1 for rand */
};


/* structure to exchange data which is needed to connect the QPs */
struct cm_con_data_t {
	uint64_t	addr;		/* Buffer address */
	uint32_t	rkey;		/* Remote key */
	uint32_t	qp_num;		/* QP number */
	uint16_t	lid;		/* LID of the IB port */
	uint8_t		gid[16];	/* gid */
} __attribute__((packed));


/* structure of system resources */
struct resources {
	struct ibv_device_attr 	device_attr;	/* Device attributes */
	struct ibv_port_attr	port_attr;	/* IB port attributes */
	struct cm_con_data_t	remote_props;	/* values to connect to remote side */
	struct ibv_context	*ib_ctx;	/* device handle */
	struct ibv_pd		*pd;		/* PD handle */
	struct ibv_cq		*cq;		/* CQ handle */
	struct ibv_qp		*qp;		/* QP handle */
	struct ibv_mr		*mr;		/* MR handle for buf */
	char			*buf;		/* memory buffer pointer, used for RDMA and send ops */
	int			sock;		/* TCP socket file descriptor */
};

struct config_t config = {
	NULL,	/* dev_name */
	NULL,	/* server_name */
	19875,	/* tcp_port */
	1,	/* ib_port */
	-1,	/* gid_idx */
	ITERS, /* iters */
	0 /* mode */
};

/**
 * Pin all current and future memory pages in memory so that the OS does not
 * swap them to disk.
 *
 * Note that future mapping operations (e.g. mmap, stack expansion, etc)
 * may fail if their memory cannot be pinned due to resource limits. Thus the
 * check below may not capture all possible failures up front. It's probably
 * best to call this at the end of initialisation (after most large allocations
 * have been made).
 */
void pin_all_memory() {
	int r = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (r != 0) {
		fprintf(stderr, "Could not lock all memory pages (%s)\n", strerror(errno));
	}
}


/* poll_completion */
/******************************************************************************
 * *	Function: poll_completion
 * *
 * *	Input
 * *	res	pointer to resources structure
 * *
 * *	Output
 * *	none
 * *
 * *	Returns
 * *	0 on success, 1 on failure
 * *
 * *	Description
 * *	Poll the completion queue for a single event. This function will continue to
 * *	poll the queue until MAX_POLL_CQ_TIMEOUT milliseconds have passed.
 * *
 * ******************************************************************************/

static int poll_completion(struct resources *res)
{
	struct ibv_wc	wc;
	unsigned long	start_time_msec;
	unsigned long	cur_time_msec;
	struct timeval	cur_time;
	int		poll_result;
	int		rc = 0;

	/* poll the completion for a while before giving up of doing it .. */
	gettimeofday(&cur_time, NULL);
	start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);

	do {
		poll_result = ibv_poll_cq(res->cq, 1, &wc);
		gettimeofday(&cur_time, NULL);
		cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
	} while ((poll_result == 0) && ((cur_time_msec - start_time_msec) < MAX_POLL_CQ_TIMEOUT));

	if (poll_result < 0) {
		/* poll CQ failed */
		fprintf(stderr, "poll CQ failed retval = %d, errno: %s\n", poll_result, strerror(errno));
		rc = 1;
	} else if (poll_result == 0) {
		/* the CQ is empty */
		fprintf(stderr, "completion wasn't found in the CQ after timeout. errno: %s\n", strerror(errno));
		rc = 1;
	} else {
		/* CQE found */
		debug_print("completion was found in CQ with status 0x%x\n", wc.status);

		/* check the completion status (here we don't care about the completion opcode */
		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n", wc.status, wc.vendor_err);
			rc = 1;
		}
	}

	return rc;
}



/******************************************************************************
 * *	Function: post_send
 * *
 * *	Input
 * *	res	 pointer to resources structure
 * *	opcode   IBV_WR_SEND, IBV_WR_RDMA_READ or IBV_WR_RDMA_WRITE
 * *
 * *	Output
 * *	none
 * *
 * *	Returns
 * *	0 on success, error code on failure
 * *
 * *	Description
 * *	This function will create and post a send work request
 * ******************************************************************************/

static int post_send(struct resources *res, int opcode)
{
	struct ibv_send_wr	sr;
	struct ibv_sge		sge;
	struct ibv_send_wr	*bad_wr = NULL;
	int			rc;

	/* prepare the scatter/gather entry */
	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)res->buf;
	sge.length = CLIENT_MSG_SIZE;
	sge.lkey = res->mr->lkey;

	/* prepare the send work request */
	memset(&sr, 0, sizeof(sr));
	sr.next = NULL;
	sr.wr_id = 0;
	sr.sg_list = &sge;
	sr.num_sge = 1;
	sr.opcode = opcode;
	sr.send_flags = IBV_SEND_SIGNALED;

	if(opcode != IBV_WR_SEND) {
		sr.wr.rdma.remote_addr = res->remote_props.addr;
		sr.wr.rdma.rkey = res->remote_props.rkey;
	}

	/* there is a Receive Request in the responder side, so we won't get any into RNR flow */
	rc = ibv_post_send(res->qp, &sr, &bad_wr);
	if (rc)
		fprintf(stderr, "failed to post SR\n");

	return rc;
}


/* Time the difference between an post_send and a poll_cq */
static int post_send_poll_complete(struct resources *res, int opcode, uint64_t* cycle_count)
{
	// From post_send
	struct ibv_send_wr	sr;
	struct ibv_sge		sge;
	struct ibv_send_wr	*bad_wr = NULL;
	int			rc;

	// From poll_complete
	struct ibv_wc	wc;
	int		poll_result;

	// Timing variables
	uint64_t start_cycle_count;
	uint64_t end_cycle_count;

	/* prepare the scatter/gather entry */
	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)res->buf;
	sge.length = CLIENT_MSG_SIZE;
	sge.lkey = res->mr->lkey;

	/* prepare the send work request */
	memset(&sr, 0, sizeof(sr));
	sr.next = NULL;
	sr.wr_id = 0;
	sr.sg_list = &sge;
	sr.num_sge = 1;
	sr.opcode = opcode;
	sr.send_flags = IBV_SEND_SIGNALED;

	if(opcode != IBV_WR_SEND) {
		sr.wr.rdma.remote_addr = res->remote_props.addr;
		sr.wr.rdma.rkey = res->remote_props.rkey;
	}

	/* there is a Receive Request in the responder side, so we won't get any into RNR flow */
	start_cycle_count = start_tsc();

	rc = ibv_post_send(res->qp, &sr, &bad_wr);
	if (rc)
		fprintf(stderr, "failed to post SR\n");
	do {
		poll_result = ibv_poll_cq(res->cq, 1, &wc);
	} while (poll_result == 0);

	end_cycle_count = stop_tsc();

	if (poll_result < 0) {
		/* poll CQ failed */
		fprintf(stderr, "poll CQ failed retval = %d, errno: %s\n", poll_result, strerror(errno));
		rc = 1;
	} else if (poll_result == 0) {
		/* the CQ is empty */
		fprintf(stderr, "completion wasn't found in the CQ after timeout. errno: %s\n", strerror(errno));
		rc = 1;
	} else {
		/* CQE found */
		*cycle_count = end_cycle_count - start_cycle_count;

		/* check the completion status (here we don't care about the completion opcode */
		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n", wc.status, wc.vendor_err);
			rc = 1;
		}
	}

	return rc;
}


/******************************************************************************
 * *	Function: post_receive
 * *
 * *	Input
 * *	res	pointer to resources structure
 * *
 *  *	Output
 *  *	none
 *  *
 *  *	Returns
 *  *	0 on success, error code on failure
 *  *
 *  *	Description
 *  *
 *  ******************************************************************************/

static int post_receive(struct resources *res)
{
	struct ibv_recv_wr	rr;
	struct ibv_sge		sge;
	struct ibv_recv_wr	*bad_wr;
	int			rc;

	/* prepare the scatter/gather entry */
	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)res->buf;
	sge.length = CLIENT_MSG_SIZE;
	sge.lkey = res->mr->lkey;

	/* prepare the receive work request */
	memset(&rr, 0, sizeof(rr));
	rr.next = NULL;
	rr.wr_id = 0;
	rr.sg_list = &sge;
	rr.num_sge = 1;

	/* post the Receive Request to the RQ */
	rc = ibv_post_recv(res->qp, &rr, &bad_wr);
	if (rc)
		fprintf(stderr, "failed to post RR\n");
	else
		debug_print("Receive Request was posted\n");

	return rc;
}



/******************************************************************************
 * *	Function: resources_init
 * *
 * *	Input
 * *	res	pointer to resources structure
 * *
 * *	Output
 * *	res	is initialized
 * *
 *  *	Returns
 *  *	none
 *  *
 *  *	Description
 *  *	res is initialized to default values
 *  ******************************************************************************/
static void resources_init(struct resources *res)
{
	memset(res, 0, sizeof *res);
	res->sock = -1;
}

/******************************************************************************
 * *	Function: resources_create
 * *
 * *	Input
 * *	res	pointer to resources structure to be filled in
 * *
 * *	Output
 * *	res	filled in with resources
 * *
 * *	Returns
 * *	0 on success, 1 on failure
 * *
 * *	Description
 * *
 * *	This function creates and allocates all necessary system resources. These
 * *	are stored in res.
 * *****************************************************************************/

static int resources_create(struct resources *res)
{
	struct ibv_device	 **dev_list = NULL;
	struct ibv_qp_init_attr  qp_init_attr;
	struct ibv_device	 *ib_dev = NULL;
	size_t			 size;
	int		 	 i, j;
	int			 mr_flags = 0;
	int			 cq_size = 0;
	int			 num_devices;
	int			 rc = 0;
	char			 curr_num = 0;


	if (config.server_name)	{
		/* Client side */
		res->sock = sock_connect(config.server_name, config.tcp_port);
		if (res->sock < 0) {
			fprintf(stderr, "[Client only] failed to establish TCP connection to server %s, port %d\n", config.server_name, config.tcp_port);
			rc = -1;
			goto resources_create_exit;
		}
	} else {
		/* server side */
		debug_print("[Server only] waiting on port %d for TCP connection\n", config.tcp_port);
		res->sock = sock_connect(NULL, config.tcp_port);
		if (res->sock < 0) {
			fprintf(stderr, "[Server only] failed to establish TCP connection with client on port %d\n", config.tcp_port);
			rc = -1;
			goto resources_create_exit;
		}
	}

	debug_print("TCP connection was established\n");
	debug_print("searching for IB devices in host\n");

	/* get device names in the system */
	dev_list = ibv_get_device_list(&num_devices);
	if (!dev_list) {
		fprintf(stderr, "failed to get IB devices list\n");
		rc = 1;
		goto resources_create_exit;
	}

	/* if there isn't any IB device in host */
	if (!num_devices) {
		fprintf(stderr, "found %d device(s)\n", num_devices);
		rc = 1;
		goto resources_create_exit;
	}

	debug_print("found %d device(s)\n", num_devices);

	/* search for the specific device we want to work with */
	for (i = 0; i < num_devices; i ++) {
		if (!config.dev_name) {
			config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
			debug_print("device not specified, using first one found: %s\n", config.dev_name);
		}
		if (!strcmp(ibv_get_device_name(dev_list[i]), config.dev_name)) {
			ib_dev = dev_list[i];
			break;
		}
	}

	/* if the device wasn't found in host */
	if (!ib_dev) {
		fprintf(stderr, "IB device %s wasn't found\n", config.dev_name);
		rc = 1;
		goto resources_create_exit;
	}

	/* get device handle */
	res->ib_ctx = ibv_open_device(ib_dev);
	if (!res->ib_ctx) {
		fprintf(stderr, "failed to open device %s\n", config.dev_name);
		rc = 1;
		goto resources_create_exit;
	}

	/* We are now done with device list, free it */
	ibv_free_device_list(dev_list);
	dev_list = NULL;
	ib_dev = NULL;

	/* query port properties */
	if (ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr)) {
		fprintf(stderr, "ibv_query_port on port %u failed\n", config.ib_port);
		rc = 1;
		goto resources_create_exit;
	}

	/* allocate Protection Domain */
	res->pd = ibv_alloc_pd(res->ib_ctx);
	if (!res->pd) {
		fprintf(stderr, "ibv_alloc_pd failed\n");
		rc = 1;
		goto resources_create_exit;
	}

	/* each side will send only one WR, so Completion Queue with 1 entry is enough */
	cq_size = 1;
	res->cq = ibv_create_cq(res->ib_ctx, cq_size, NULL, NULL, 0);
	if (!res->cq) {
		fprintf(stderr, "failed to create CQ with %u entries\n", cq_size);
		rc = 1;
		goto resources_create_exit;
	}

	/* allocate the memory buffer that will hold the data */
	if (!config.server_name)
		size = SERVER_COLUMN_COUNT * SERVER_ROW_COUNT;
	else
		size = CLIENT_MSG_SIZE;

	res->buf = (char *) malloc(size);
	pin_all_memory();

	if (!res->buf) {
		fprintf(stderr, "failed to malloc %Zu bytes to memory buffer\n", size);
		rc = 1;
		goto resources_create_exit;
	}

	if (!config.server_name) {
		for (i = 0; i < SERVER_ROW_COUNT; i++) {
			for (j = 0; j < SERVER_COLUMN_COUNT; j++) {
				res->buf[i * SERVER_COLUMN_COUNT + j] = curr_num;
				curr_num++;
			}
		}
	}
	else
		memset(res->buf, 0 , size);

	/* register the memory buffer */
	mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
	res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags);
	if (!res->mr) {
		fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
		rc = 1;
		goto resources_create_exit;
	}

	debug_print("MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n", res->buf, res->mr->lkey, res->mr->rkey, mr_flags);

	/* create the Queue Pair */
	memset(&qp_init_attr, 0, sizeof(qp_init_attr));

	qp_init_attr.qp_type = IBV_QPT_RC;
	qp_init_attr.sq_sig_all = 0;
	qp_init_attr.send_cq = res->cq;
	qp_init_attr.recv_cq = res->cq;
	qp_init_attr.cap.max_send_wr  = 1;
	qp_init_attr.cap.max_recv_wr  = 1;
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.cap.max_recv_sge = 1;

	res->qp = ibv_create_qp(res->pd, &qp_init_attr);
	if (!res->qp) {
		fprintf(stderr, "failed to create QP\n");
		rc = 1;
		goto resources_create_exit;
	}

	debug_print("QP was created, QP number=0x%x\n", res->qp->qp_num);

resources_create_exit:
	if (rc) {
		/* Error encountered, cleanup */

		if (res->qp) {
			ibv_destroy_qp(res->qp);
			res->qp = NULL;
		}

		if (res->mr) {
			ibv_dereg_mr(res->mr);
			res->mr = NULL;
		}

		if (res->buf) {
			free(res->buf);
			res->buf = NULL;
		}

		if (res->cq) {
			ibv_destroy_cq(res->cq);
			res->cq = NULL;
		}

		if (res->pd) {
			ibv_dealloc_pd(res->pd);
			res->pd = NULL;
		}

		if (res->ib_ctx) {
			ibv_close_device(res->ib_ctx);
			res->ib_ctx = NULL;
		}

		if (dev_list) {
			ibv_free_device_list(dev_list);
			dev_list = NULL;
		}

		if (res->sock >= 0) {
			if (close(res->sock))
				fprintf(stderr, "failed to close socket\n");
			res->sock = -1;
		}
	}

	return rc;
}

/******************************************************************************
 * *	Function: modify_qp_to_init
 * *
 * *	Input
 * *	qp	QP to transition
 * *
 * *	Output
 * *	none
 * *
 * *	Returns
 * *	0 on success, ibv_modify_qp failure code on failure
 * *
 * *	Description
 * *	Transition a QP from the RESET to INIT state
 * ******************************************************************************/

static int modify_qp_to_init(struct ibv_qp *qp)
{
	struct ibv_qp_attr	attr;
	int			flags;
	int			rc;

	memset(&attr, 0, sizeof(attr));

	attr.qp_state = IBV_QPS_INIT;
	attr.port_num = config.ib_port;
	attr.pkey_index = 0;
	attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
	flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to INIT (%s)\n", strerror(errno));

	return rc;
}


/******************************************************************************
 * *	Function: modify_qp_to_rtr
 * *
 * *	Input
 * *	qp		QP to transition
 * *	remote_qpn	remote QP number
 * *	dlid		destination LID
 * *	dgid		destination GID (mandatory for RoCEE)
 * *
 * *	Output
 * *	none
 * *
 * *	Returns
 * *	0 on success, ibv_modify_qp failure code on failure
 * *
 * *	Description
 * *	Transition a QP from the INIT to RTR state, using the specified QP number
 * ******************************************************************************/

static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, const uint8_t *dgid)
{
	struct ibv_qp_attr	attr;
	int			flags;
	int			rc;

	memset(&attr, 0, sizeof(attr));

	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_2048;
	attr.dest_qp_num = remote_qpn;
	attr.rq_psn = 0;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 0x12;
	attr.ah_attr.is_global = 0;
	attr.ah_attr.dlid = dlid;
	attr.ah_attr.sl = 0;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = config.ib_port;
	if (config.gid_idx >= 0) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.port_num = 1;
		memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
		attr.ah_attr.grh.flow_label = 0;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.sgid_index = config.gid_idx;
		attr.ah_attr.grh.traffic_class = 0;
	}

	flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to RTR (%s)\n", strerror(errno));

	return rc;
}



/******************************************************************************
 * *	Function: modify_qp_to_rts
 * *
 * *	Input
 * *	qp	QP to transition
 * *
 * *	Output
 * *	none
 * *
 * *	Returns
 * *	0 on success, ibv_modify_qp failure code on failure
 * *
 * *	Description
 * *	Transition a QP from the RTR to RTS state
 * ******************************************************************************/

static int modify_qp_to_rts(struct ibv_qp *qp)
{
	struct ibv_qp_attr	attr;
	int			flags;
	int			rc;

	memset(&attr, 0, sizeof(attr));

	attr.qp_state	= IBV_QPS_RTS;
	attr.timeout	= 0x12;
	attr.retry_cnt	= 6;
	attr.rnr_retry	= 0;
	attr.sq_psn	= 0;
	attr.max_rd_atomic = 1;

	flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to RTS (%s)\n", strerror(errno));

	return rc;
}


/******************************************************************************
 * *	Function: connect_qp
 * *
 * *	Input
 * *	res	pointer to resources structure
 * *
 * *	Output
 * *	none
 * *
 * *	Returns
 * *	0 on success, error code on failure
 * *
 * *	Description
 * *	Connect the QP. Transition the server side to RTR, sender side to RTS
 * ******************************************************************************/

static int connect_qp(struct resources *res)
{
	struct cm_con_data_t	local_con_data;
	struct cm_con_data_t	remote_con_data;
	struct cm_con_data_t	tmp_con_data;
	int			rc = 0;
	char 			temp_char;
	union ibv_gid		my_gid;


	if (config.gid_idx >= 0) {
		rc = ibv_query_gid(res->ib_ctx, config.ib_port, config.gid_idx, &my_gid);
		if (rc) {
			fprintf(stderr, "could not get gid for port %d, index %d\n", config.ib_port, config.gid_idx);
			return rc;
		}
	} else
		memset(&my_gid, 0, sizeof my_gid);

	/* exchange using TCP sockets info required to connect QPs */
	local_con_data.addr = htonll((uintptr_t)res->buf);
	local_con_data.rkey = htonl(res->mr->rkey);
	local_con_data.qp_num = htonl(res->qp->qp_num);
	local_con_data.lid = htons(res->port_attr.lid);
	memcpy(local_con_data.gid, &my_gid, sizeof(my_gid));

	debug_print("\nLocal LID	= 0x%x\n", res->port_attr.lid);
	if (sock_sync_data(res->sock, sizeof(struct cm_con_data_t), (char *) &local_con_data, (char *) &tmp_con_data) < 0) {
		fprintf(stderr, "failed to exchange connection data between sides\n");
		return 1;
	}

	remote_con_data.addr = ntohll(tmp_con_data.addr);
	remote_con_data.rkey = ntohl(tmp_con_data.rkey);
	remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
	remote_con_data.lid = ntohs(tmp_con_data.lid);
	memcpy(remote_con_data.gid, tmp_con_data.gid, sizeof(my_gid));

	/* save the remote side attributes, we will need it for the post SR */
	res->remote_props = remote_con_data;

	debug_print("Remote address = 0x%"PRIx64"\n", remote_con_data.addr);
	debug_print("Remote rkey = 0x%x\n", remote_con_data.rkey);
	debug_print("Remote QP number = 0x%x\n", remote_con_data.qp_num);
	debug_print("Remote LID = 0x%x\n", remote_con_data.lid);
	if (config.gid_idx >= 0) {
		const uint8_t *p = remote_con_data.gid;

		debug_print("Remote GID = %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	}

	/* modify the QP to init */
	rc = modify_qp_to_init(res->qp);
	if (rc) {
		fprintf(stderr, "change QP state to INIT failed\n");
		return rc;
	}

	/* let the client post RR to be prepared for incoming messages */
	if (config.server_name) {
		rc = post_receive(res);
		if (rc) {
			fprintf(stderr, "failed to post RR\n");
			return rc;
		}
	}


	/* modify the QP to RTR */
	rc = modify_qp_to_rtr(res->qp, remote_con_data.qp_num, remote_con_data.lid, remote_con_data.gid);
	if (rc) {
		fprintf(stderr, "failed to modify QP state to RTR (%s)\n", strerror(errno));
		return rc;
	}

	rc = modify_qp_to_rts(res->qp);
	if (rc) {
		fprintf(stderr, "failed to modify QP state to RTS (%s)\n", strerror(errno));
		return rc;
	}

	debug_print("QP state was change to RTS\n");

	/* sync to make sure that both sides are in states that they can connect to prevent packet loose */
	if (sock_sync_data(res->sock, 1, "Q", &temp_char)) { /* just send a dummy char back and forth */
		fprintf(stderr, "sync error after QPs were moved to RTS\n");
		return rc;
	}

	return 0;
}


/******************************************************************************
 * *	Function: resources_destroy
 * *
 * *	Input
 * *	res	pointer to resources structure
 * *
 * *	Output
 * *	none
 * *
 * *	Returns
 * *	0 on success, 1 on failure
 * *
 * *	Description
 * *	Cleanup and deallocate all resources used
 * ******************************************************************************/

static int resources_destroy(struct resources *res)
{
	int rc = 0;

	if (res->qp)
		if (ibv_destroy_qp(res->qp)) {
			fprintf(stderr, "failed to destroy QP\n");
			rc = 1;
		}

	if (res->mr)
		if (ibv_dereg_mr(res->mr)) {
			fprintf(stderr, "failed to deregister MR\n");
			rc = 1;
		}

	if (res->buf)
		free(res->buf);

	if (res->cq)
		if (ibv_destroy_cq(res->cq)) {
			fprintf(stderr, "failed to destroy CQ\n");
			rc = 1;
		}

	if (res->pd)
		if (ibv_dealloc_pd(res->pd)) {
			fprintf(stderr, "failed to deallocate PD\n");
			rc = 1;
		}

	if (res->ib_ctx)
		if (ibv_close_device(res->ib_ctx)) {
			fprintf(stderr, "failed to close device context\n");
			rc = 1;
		}

	if (res->sock >= 0)
		if (close(res->sock)) {
			fprintf(stderr, "failed to close socket\n");
			rc = 1;
		}

	return rc;
}

/******************************************************************************
 * *	Function: print_config
 * *
 * *	Description
 * *	Print out config information
 *  ******************************************************************************/
static void print_config(void)
{
	debug_print(" ------------------------------------------------\n");
	debug_print(" Device name	: \"%s\"\n", config.dev_name);
	debug_print(" IB port	: %u\n", config.ib_port);
	if (config.server_name)
		debug_print("[client only] IP	: %s\n", config.server_name);
	debug_print(" TCP port	: %u\n", config.tcp_port);
	if (config.gid_idx >= 0)
		debug_print(" GID index	: %u\n", config.gid_idx);
	debug_print(" ------------------------------------------------\n\n");
}


/******************************************************************************
 * *	Function: usage
 * *
 * *	Input
 * *	argv0	command line arguments
 * *
 * *	Output
 * *	none
 * *
 * *	Returns
 * *	none
 * *
 * *	Description
 * *	print a description of command line syntax
 * ******************************************************************************/

static void usage(const char *argv0)
{
	fprintf(stdout, "Usage:\n");
	fprintf(stdout, " %s start a server and wait for connection\n", argv0);
	fprintf(stdout, " %s <host> connect to server at <host>\n", argv0);
	fprintf(stdout, "\n");
	fprintf(stdout, "Options:\n");
	fprintf(stdout, " -p, --port <port> listen on/connect to port <port> (default 18515)\n");
	fprintf(stdout, " -d, --ib-dev <dev> use IB device <dev> (default first device found)\n");
	fprintf(stdout, " -i, --ib-port <port>   use port <port> of IB device (default 1)\n");
	fprintf(stdout, " -g, --gid_idx <gid index>   gid index to be used in GRH (default not used)\n");
#define L(x) #x
	fprintf(stdout, " -n, --iterations <iterations>  "
			"Number of iterations to perform in the test "
			"(default "L(ITERS)")\n");
#undef L
	fprintf(stdout, " -m, --mode <mode>  set to 0 for seq or 1 for rand (default 0)\n");
}

int read_write_read(struct resources *res, uint64_t target_addr, double cycles_to_usec) {
	uint64_t write_cyclces, orig_addr, read1_cycles, read2_cycles;
	int64_t delta;

	/* Store the original addr so we can change back to it after we're done. */
	orig_addr = res->remote_props.addr;
	res->remote_props.addr = target_addr;

	/* First read the contents of the server's buffer.
	 * This should be a cache miss. */
	if (post_send_poll_complete(res, IBV_WR_RDMA_READ, &read1_cycles)) {
		fprintf(stderr, "failed to post SR 2\n");
		return 1;
	}
	debug_print("[READ]  Contents of server's buffer: '%hhu', it took %lu cycles\n", res->buf[0], read1_cycles);

	/* Now we replace what's in the server's buffer.
	 * This should pull this bit of memory into cache. */
	// res->buf[0] = res->buf[0] + 2;
	debug_print("[WRITE] Now replacing it with: '%hhu',", res->buf[0]);
	if (post_send_poll_complete(res, IBV_WR_RDMA_WRITE, &write_cyclces)) {
		fprintf(stderr, "failed to post SR 3\n");
		return 1;
	}
	debug_print("it took %lu cycles\n", write_cyclces);

	/* Then we read contents of server's buffer again.
	 * This should be a cache hit. */
	if (post_send_poll_complete(res, IBV_WR_RDMA_READ, &read2_cycles)) {
		fprintf(stderr, "failed to post SR 2\n");
		return 1;
	}
	delta = read1_cycles - read2_cycles;

	data_print("%lu,%lu,%f,%f\n", read1_cycles, read2_cycles, (read1_cycles * 1000) / cycles_to_usec, (read2_cycles * 1000) / cycles_to_usec);
	debug_print("[READ]  Contents of server's buffer: '%hhu', it took %lu cycles\n", res->buf[0], read2_cycles);
	debug_print("[DIFF]  %5ld cycles = %06.1f nsec\n", delta, delta / cycles_to_usec);

	/* Restore the original addr */
	res->remote_props.addr = orig_addr;

	return 0;
}

/******************************************************************************
 * *	Function: main
 *  *
 *  *	Input
 *  *	argc   number of items in argv
 *  *	argv   command line parameters
 *  *
 *  *	Output
 *  *	none
 *  *
 *  *	Returns
 *  *	0 on success, 1 on failure
 *  *
 *  *	Description
 *  *	Main program code
 *  ******************************************************************************/

int main(int argc, char *argv[])
{
	struct resources	res;
	int			rc = 1;
	char		temp_char;
	unsigned int		i, j;
	uint64_t start_addr;

	/* parse the command line parameters */
	while (1) {
		int c;

		static struct option long_options[] = {
			{.name = "port",	.has_arg = 1,  .val = 'p' },
			{.name = "ib-dev",	.has_arg = 1,  .val = 'd' },
			{.name = "ib-port",     .has_arg = 1,  .val = 'i' },
			{.name = "gid-idx",     .has_arg = 1,  .val = 'g' },
			{.name = "iterations",  .has_arg = 1,  .val = 'n' },
			{.name = "mode",		.has_arg = 1,  .val = 'm'},
			{.name = NULL,		.has_arg = 0,  .val = '\0'}
		};

		c = getopt_long(argc, argv, "p:d:i:g:n:m:", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
			case 'p':
				config.tcp_port = strtoul(optarg, NULL, 0);
				break;

			case 'd':
				config.dev_name = strdup(optarg);
				break;

			case 'i':
				config.ib_port = strtoul(optarg, NULL, 0);
				if (config.ib_port < 0) {
					usage(argv[0]);
					return 1;
				}
				break;

			case 'g':
				config.gid_idx = strtoul(optarg, NULL, 0);
				if (config.gid_idx < 0) {
					usage(argv[0]);
					return 1;
				}
				break;

			case 'n':
				config.iters = strtoul(optarg, NULL, 0);
				break;

			case 'm':
				config.mode = strtoul(optarg, NULL, 0);
				break;

			default:
				usage(argv[0]);
				return 1;
		}
	}

	/* parse the last parameter (if exists) as the server name */
	if (optind == argc - 1)
		config.server_name = argv[optind];
	else if (optind < argc) {
		usage(argv[0]);
		return 1;
	}

	/* set cpu affinity for client */
	if (config.server_name) {
		cpu_set_t s;
		CPU_ZERO(&s);
		CPU_SET(sched_getcpu(), &s);
		sched_setaffinity(0, sizeof(cpu_set_t), &s);
	}

	/* print the used parameters for info */
	print_config();

	/* init all of the resources, so cleanup will be easy */
	resources_init(&res);

	/* create resources before using them */
	if (resources_create(&res)) {
		fprintf(stderr, "failed to create resources\n");
		goto main_exit;
	}

	/* connect the QPs */
	if (connect_qp(&res)) {
		fprintf(stderr, "failed to connect QPs\n");
		goto main_exit;
	}

	/* let the server post the sr */
	if (!config.server_name)
		if (post_send(&res, IBV_WR_SEND)) {
			fprintf(stderr, "failed to post sr\n");
			goto main_exit;
		}

	/* in both sides we expect to get a completion */
	if (poll_completion(&res)) {
		fprintf(stderr, "poll completion failed\n");
		goto main_exit;
	}

	/* after polling the completion we have the message in the client buffer too */
	if (config.server_name)
		debug_print("[Client only] Message is: '%hhu'\n", res.buf[0]);

	/* Sync so we are sure server side has data ready before client tries to read it */
	if (sock_sync_data(res.sock, 1, "R", &temp_char)) {  /* just send a dummy char back and forth */
		fprintf(stderr, "sync error before RDMA ops\n");
		rc = 1;
		goto main_exit;
	}

	if (config.server_name)
		debug_print("Beginning tests...\n----------------------------\n\n");

	double cycles_to_usec = get_cpu_mhz(false);

	/*  Now the client performs an RDMA read and then write on server.
	 *  Note that the server has no idea these events have occured */
	if (config.server_name) {
		start_addr = res.remote_props.addr;

		switch (config.mode) {
			case 0: /* seq */
				for (i = 0; i < SERVER_COLUMN_COUNT; i+=CLIENT_MSG_SIZE) {
					for (j = 0; j < SERVER_ROW_COUNT; ++j) {
						if (read_write_read(&res, start_addr + j * SERVER_COLUMN_COUNT + i, cycles_to_usec)) {
							rc = 1;
							goto main_exit;
						}
					}
				}
				break;

			case 1: /* rand */
				for (i = 0; i < config.iters; ++i) {
					if (read_write_read(&res, start_addr + rand_line(), cycles_to_usec)) {
						rc = 1;
						goto main_exit;
					}

					if (i == CACHE_LINES) {
						memset(bm, 0, sizeof(bm));
					}
				}
				break;
		}
	}

	/* Sync so server will know that client is done mucking with its memory */
	if (sock_sync_data(res.sock, 1, "W", &temp_char)) {  /* just send a dummy char back and forth */
		fprintf(stderr, "sync error after RDMA ops\n");
		rc = 1;
		goto main_exit;
	}

	rc = 0;

main_exit:
	if (resources_destroy(&res)) {
		fprintf(stderr, "failed to destroy resources\n");
		rc = 1;
	}

	if(config.dev_name)
		free((char *) config.dev_name);

	debug_print("\ntest result is %d\n", rc);

	return rc;
}
