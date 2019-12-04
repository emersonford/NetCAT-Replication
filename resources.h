/* vim: set noet: */

#ifndef RESOURCES_H_
#define RESOURCES_H_

#include <stdint.h>
#include <byteswap.h>
#include <infiniband/verbs.h>

#include "print.h"

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif


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

/* structure of test parameters */
struct config_t {
	const char	*dev_name;	/* IB device name */
	char		*server_name;	/* server host name */
	u_int32_t	tcp_port;	/* server TCP port */
	int		ib_port;	/* local IB port to work with */
	int		gid_idx;	/* gid index to use */
	unsigned int	iters;		/* number of iterations */
	int		mode; /* 0 for seq, 1 for rand */
	int		msg_size; /* size of client buffer */
	int		column_count; /* number of columns in the 2D array, size of one row is msg_size * column_count */
	int		row_count; /* number of rows in the 2D array */
};

extern struct config_t config;

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
void resources_init(struct resources *res);


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
int resources_create(struct resources *res);


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
int modify_qp_to_init(struct ibv_qp *qp);


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
int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, const uint8_t *dgid);


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
int modify_qp_to_rts(struct ibv_qp *qp);


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
int connect_qp(struct resources *res);


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
int resources_destroy(struct resources *res);

#endif // RESOURCES_H_
