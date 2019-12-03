/* vim: set noet: */
/******************************************************************************
 * Socket operations
 *
 * For simplicity, the example program uses TCP sockets to exchange control information. If a TCP/IP stack/connection is not available,
 * connection manager (CM) may be used to pass this information. Use of CM is beyond the scope of this example
 *
 * ******************************************************************************/

#ifndef SOCKETS_H_
#define SOCKETS_H_

/******************************************************************************
 * *	Function: sock_connect
 * *
 *  *
 *  *	Input
 *  *	servername    URL of server to connect to (NULL for server mode)
 *  *	port	port of service
 *  *
 *  *	Output
 *  *	none
 *  *
 *  *	Returns
 *  *	socket (fd) on success, negative error code on failure
 *  *
 *  *	Description
 *  *	Connect a socket.  If servername is specified a client connection will be
 *  *	initiated to the indicated server and port.  Otherwise listen on the
 *  *	indicated port for an incoming connection.
 *  *
 *  ******************************************************************************/
int sock_connect(const char *servername, int port);


/******************************************************************************
 * *	Function: sock_sync_data
 * *
 *  *	Input
 *  *	sock		socket to transfer data on
 *  *	xfer_size	size of data to transfer
 *  *	local_data	pointer to data to be sent to remote
 *  *
 *  *	Output
 *  *	remote_data	pointer to buffer to receive remote data
 *  *
 *  *	Returns
 *  *	0 on success, negative error code on failure
 *  *
 *  *	Description
 *  *	Sync data across a socket. The indicated local data will be sent to the
 *  *	remote. It will then wait for the remote to send its data back. It is
 *  *	assumed that the two sides are in sync and call this function in the proper
 *  *	order.  Chaos will ensue if they are not. :)
 *  *
 *  *	Also note this is a blocking function and will wait for the full data to be
 *  *	received from the remote.
 *  *
 *  ******************************************************************************/
int sock_sync_data(int sock, int xfer_size, char *local_data, char *remote_data);

#endif // SOCKETS_H_
