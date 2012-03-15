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
** @file XreadCID.c
** @brief implements XreadCID
*/

#include<errno.h>
#include "Xsocket.h"
#include "Xinit.h"
#include "Xutil.h"

/*!
** @brief Reads the contents of the specified CID into rbuf. Must be called
** after XgetCID() or XgetCIDList().
**
** @param sockfd - the control socket (must be of type XSOCK_CHUNK)
** @param rbuf - buffer to receive the data
** @param len - length of rbuf
** @param flags - currently unused
** @param cDAG - the CID to retrieve
** @param dlen - length of cDAG (currently unused)
**
** @returns number of bytes in the CID
** @returns -1 on error with errno set
*/
int XreadCID(int sockfd, void *rbuf, size_t len, int /* flags */, 
		char * cDAG, size_t /* dlen */)
{
	int rc;
	char UDPbuf[MAXBUFLEN];

	if (validateSocket(sockfd, XSOCK_CHUNK, EAFNOSUPPORT) < 0) {
		LOGF("Socket %d must be a chunk socket\n", sockfd);
		return -1;
	}

	if (len == 0)
		return 0;

	if (!rbuf || !cDAG) {
		LOG("null pointer error!");
		errno = EFAULT;
		return -1;
	}

	xia::XSocketMsg xsm;
	xsm.set_type(xia::XREADCID);

	xia::X_Readcid_Msg *x_readcid_msg = xsm.mutable_x_readcid();
  
	x_readcid_msg->set_dag(cDAG);

	std::string p_buf;
	xsm.SerializeToString(&p_buf);

	if ((rc = click_data(sockfd, &xsm)) < 0) {
		LOGF("Error talking to Click: %s", strerror(errno));
		return -1;
	}

	if ((rc = click_reply(sockfd, UDPbuf, sizeof(UDPbuf))) < 0) {
		LOGF("Error retrieving status from Click: %s", strerror(errno));
		return -1;
	}

	// FIXME: change to protobuf so we don't need the ^ hack

	size_t paylen = 0, i = 0;
	char *tmpbuf = (char*)UDPbuf;
	while (tmpbuf[i] != '^')
		i++;
	paylen = rc - i - 1;
	int offset = i + 1;

	if (paylen > len) {
		LOGF("CID is %d bytes, but rbuf is only %d bytes", paylen, len);
		errno = EFAULT;
		return -1;
	}

	memcpy(rbuf, UDPbuf + offset, paylen);
	return paylen;
}




