/*
** Copyright 2011 Carnegie Mellon University
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**    http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/
/*!
** @file Xrecv.c
** @brief implements Xrecv()
*/

#include <errno.h>
#include "Xsocket.h"
#include "Xinit.h"
#include "Xutil.h"

/*!
** @brief Receive data from an Xsocket
**
** Xrecv() retrieves data from a connected Xsocket of type XSOCK_STREAM.
** sockfd must have previously been connected via Xaccept() or
** Xconnect().
**
** Xrecv() does not currently have a non-blocking mode, and will block
** until a data is available on sockfd. However, the standard socket API
** calls select and poll may be used with the Xsocket. Either function
** will deliver a readable event when a new connection is attempted and
** you may then call Xrecv() to get the data.
**
** NOTE: in cases where more data is received than specified by the caller,
** the excess data will be stored at the API level. Subsequent Xrecv calls
** return the stored data until it is drained, and will then resume requesting
** data from the transport.
**
** @param sockfd The socket to receive with
** @param rbuf where to put the received data
** @param len maximum amount of data to receive. the amount of data
** returned may be less than len bytes.
** @param flags (This is not currently used but is kept to be compatible
** with the standard sendto socket call).
**
** @returns the number of bytes received, which may be less than the number
** requested by the caller
** @returns -1 on failure with errno set to an error compatible with the standard
** recv call.
*/
int Xrecv(int sockfd, void *rbuf, size_t len, int flags)
{
	int numbytes;

	if (flags) {
		errno = EOPNOTSUPP;
		return -1;
	}

	if (len == 0)
		return 0;

	if (!rbuf) {
		LOG("buffer pointer is null!\n");
		errno = EFAULT;
		return -1;
	}

	if (validateSocket(sockfd, XSOCK_STREAM, EOPNOTSUPP) < 0) {
		LOGF("Socket %d must be a stream socket", sockfd);
		return -1;
	}

	if (getConnState(sockfd) != CONNECTED) {
		LOGF("Socket %d is not connected", sockfd);
		errno = ENOTCONN;
		return -1;
	}

	// see if we have bytes leftover from a previous Xrecv call
	if ((numbytes = getSocketData(sockfd, (char *)rbuf, len)) > 0)
		return numbytes;

	xia::XSocketMsg xsm;
	xsm.set_type(xia::XRECV);
	unsigned seq = seqNo(sockfd);
	xsm.set_sequence(seq);

	xia::X_Recv_Msg *xrm = xsm.mutable_x_recv();
	xrm->set_bytes_requested(len);

	if (click_send(sockfd, &xsm) < 0) {
		LOGF("Error talking to Click: %s", strerror(errno));
		return -1;
	}

	xsm.Clear();
	if ((numbytes = click_reply(sockfd, seq, &xsm)) < 0) {
		LOGF("Error retrieving recv data from Click: %s", strerror(errno));
		return -1;
	}

	xrm = xsm.mutable_x_recv();
	const char *payload = xrm->payload().c_str();

	xia::X_Result_Msg *r = xsm.mutable_x_result();
	int paylen = r->return_code();

	if (paylen < 0) {
		errno = r->err_code();
	}
	else if (paylen <= len)
		memcpy(rbuf, payload, paylen);
	else {
		// we got back more data than the caller requested
		// stash the extra away for subsequent Xrecv calls
		memcpy(rbuf, payload, len);
		paylen -= len;
		setSocketData(sockfd, payload + len, paylen);
		paylen = len;
	}
	return paylen;
}
