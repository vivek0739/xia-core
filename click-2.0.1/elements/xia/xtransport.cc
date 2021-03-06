#include "../../userlevel/xia.pb.h"
#include <click/config.h>
#include <click/glue.hh>
#include <click/error.hh>
#include <click/error-syslog.hh>
#include <click/confparse.hh>
#include <click/packet_anno.hh>
#include <click/packet.hh>
#include <click/vector.hh>

#include <click/xiacontentheader.hh>
#include "xiatransport.hh"
#include "xtransport.hh"
#include <click/xiatransportheader.hh>

/*
** FIXME:
** - why is xia_socket_msg in the class definition and not a local variable?
** - implement a backoff delay on retransmits so we don't flood the connection
** - fix cid header size issue so we work correctly with the linux version
** - migrate from uisng printf and click_chatter to using the click ErrorHandler class
** - there are still some small memory leaks happening when stream sockets are created/used/closed
**   (problem does not happen if sockets are just opened and closed)
** - fix issue in SYN code with XIDPairToConnectPending (see comment in code for details)
** - what is the constant 22 for near line 850? I can't find a 22 anywhere else in the source tree
*/

CLICK_DECLS

XTRANSPORT::XTRANSPORT()
	: _timer(this)
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;
	_id = 0;
	isConnected = false;

	_ackdelay_ms = ACK_DELAY;
	_teardown_wait_ms = TEARDOWN_DELAY;

//	pthread_mutexattr_init(&_lock_attr);
//	pthread_mutexattr_settype(&_lock_attr, PTHREAD_MUTEX_RECURSIVE);
//	pthread_mutex_init(&_lock, &_lock_attr);

	cp_xid_type("SID", &_sid_type); 
}


int
XTRANSPORT::configure(Vector<String> &conf, ErrorHandler *errh)
{
	XIAPath local_addr;
	XID local_4id;
	Element* routing_table_elem;
	bool is_dual_stack_router;
	_is_dual_stack_router = false;

	if (cp_va_kparse(conf, this, errh,
					 "LOCAL_ADDR", cpkP + cpkM, cpXIAPath, &local_addr,
					 "LOCAL_4ID", cpkP + cpkM, cpXID, &local_4id,
					 "ROUTETABLENAME", cpkP + cpkM, cpElement, &routing_table_elem,
					 "IS_DUAL_STACK_ROUTER", 0, cpBool, &is_dual_stack_router,
					 cpEnd) < 0)
		return -1;

	_local_addr = local_addr;
	_local_hid = local_addr.xid(local_addr.destination_node());
	_local_4id = local_4id;
	// IP:0.0.0.0 indicates NULL 4ID
	_null_4id.parse("IP:0.0.0.0");

	_is_dual_stack_router = is_dual_stack_router;

	/*
	// If a valid 4ID is given, it is included (as a fallback) in the local_addr
	if(_local_4id != _null_4id) {
		String str_local_addr = _local_addr.unparse();
		size_t AD_found_start = str_local_addr.find_left("AD:");
		size_t AD_found_end = str_local_addr.find_left(" ", AD_found_start);
		String AD_str = str_local_addr.substring(AD_found_start, AD_found_end - AD_found_start);
		String HID_str = _local_hid.unparse();
		String IP4ID_str = _local_4id.unparse();
		String new_local_addr = "RE ( " + IP4ID_str + " ) " + AD_str + " " + HID_str;
		//click_chatter("new address is - %s", new_local_addr.c_str());
		_local_addr.parse(new_local_addr);	
	}
	*/

#if USERLEVEL
	_routeTable = dynamic_cast<XIAXIDRouteTable*>(routing_table_elem);
#else
	_routeTable = reinterpret_cast<XIAXIDRouteTable*>(routing_table_elem);
#endif

	return 0;
}

XTRANSPORT::~XTRANSPORT()
{
	//Clear all hashtable entries
	XIDtoPort.clear();
	XIDtoPushPort.clear();
	portToDAGinfo.clear();
	XIDpairToPort.clear();
	XIDpairToConnectPending.clear();

	hlim.clear();
	xcmp_listeners.clear();
	nxt_xport.clear();

//	pthread_mutex_destroy(&_lock);
//	pthread_mutexattr_destroy(&_lock_attr);
}


int
XTRANSPORT::initialize(ErrorHandler *)
{
	_errh = (SyslogErrorHandler*)ErrorHandler::default_handler();
	_timer.initialize(this);
	//_timer.schedule_after_msec(1000);
	//_timer.unschedule();
	return 0;
}

char *XTRANSPORT::random_xid(const char *type, char *buf)
{
	// This is a stand-in function until we get certificate based names
	//
	// note: buf must be at least 45 characters long 
	// (longer if the XID type gets longer than 3 characters)
	sprintf(buf, RANDOM_XID_FMT, type, click_random(0, 0xffffffff));

	return buf;
}

void
XTRANSPORT::run_timer(Timer *timer)
{
//	pthread_mutex_lock(&_lock);

	assert(timer == &_timer);

	Timestamp now = Timestamp::now();
	Timestamp earlist_pending_expiry = now;

	WritablePacket *copy;

	bool tear_down;

	for (HashTable<unsigned short, DAGinfo>::iterator iter = portToDAGinfo.begin(); iter != portToDAGinfo.end(); ++iter ) {
		unsigned short _sport = iter->first;
		DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);
		tear_down = false;

		// check if pending
		if (daginfo->timer_on == true) {
			// check if synack waiting
			if (daginfo->synack_waiting == true && daginfo->expiry <= now ) {
				//click_chatter("Timer: synack waiting\n");
		
				if (daginfo->num_connect_tries <= MAX_CONNECT_TRIES) {

					//click_chatter("Timer: SYN RETRANSMIT! \n");
					copy = copy_packet(daginfo->syn_pkt, daginfo);
					// retransmit syn
					XIAHeader xiah(copy);
					// click_chatter("Timer: (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), (char *)xiah.payload(), xiah.plen());
					output(NETWORK_PORT).push(copy);

					daginfo->timer_on = true;
					daginfo->synack_waiting = true;
					daginfo->expiry = now + Timestamp::make_msec(_ackdelay_ms);
					daginfo->num_connect_tries++;

				} else {
					// Stop sending the connection request & Report the failure to the application
					daginfo->timer_on = false;
					daginfo->synack_waiting = false;

					String str = String("^Connection-failed^");
					WritablePacket *ppp = WritablePacket::make (256, str.c_str(), str.length(), 0);


					//_errh->debug("Timer: Sent packet to socket with port %d", _sport);
					output(API_PORT).push(UDPIPPrep(ppp, _sport));
				}

			} else if (daginfo->dataack_waiting == true && daginfo->expiry <= now ) {

				// adding check to see if anything was retransmitted. We can get in here with
				// no packets in the daginfo->sent_pkt array waiting to go and will stay here forever
				bool retransmit_sent = false;

				if (daginfo->num_retransmit_tries < MAX_RETRANSMIT_TRIES) {

				//click_chatter("Timer: DATA RETRANSMIT at from (%s) from_port=%d base=%d next_seq=%d \n\n", (_local_addr.unparse()).c_str(), _sport, daginfo->base, daginfo->next_seqnum );
		
					// retransmit data
					for (unsigned int i = daginfo->base; i < daginfo->next_seqnum; i++) {
						if (daginfo->sent_pkt[i % MAX_WIN_SIZE] != NULL) {
							copy = copy_packet(daginfo->sent_pkt[i % MAX_WIN_SIZE], daginfo);
							XIAHeader xiah(copy);
							//click_chatter("Timer: (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), (char *)xiah.payload(), xiah.plen());
							//click_chatter("pusing the retransmit pkt\n");
							output(NETWORK_PORT).push(copy);
							retransmit_sent = true;
						}
					}
				} else {
					//click_chatter("retransmit counter exceeded\n");
					// FIXME what cleanup should happen here?
					// should we do a NAK?
				}

				if (retransmit_sent) {
					//click_chatter("resetting retransmit timer for %d\n", _sport);
					daginfo->timer_on = true;
					daginfo->dataack_waiting = true;
					daginfo-> num_retransmit_tries++;
					daginfo->expiry = now + Timestamp::make_msec(_ackdelay_ms);
				} else {
					//click_chatter("terminating retransmit timer for %d\n", _sport);
					daginfo->timer_on = false;
					daginfo->dataack_waiting = false;
					daginfo->num_retransmit_tries = 0;
				}

			} else if (daginfo->teardown_waiting == true && daginfo->teardown_expiry <= now) {
				tear_down = true;
				daginfo->timer_on = false;
				portToActive.set(_sport, false);

				//XID source_xid = portToDAGinfo.get(_sport).xid;

				// this check for -1 prevents a segfault cause by bad XIDs
				// it may happen in other cases, but opening a XSOCK_STREAM socket, calling
				// XreadLocalHostAddr and then closing the socket without doing anything else will
				// cause the problem
				// TODO: make sure that -1 is the only condition that will cause us to get a bad XID
				if (daginfo->src_path.destination_node() != -1) {
					XID source_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
					if (!daginfo->isAcceptSocket) {

						//click_chatter("deleting route %s from port %d\n", source_xid.unparse().c_str(), _sport);
						delRoute(source_xid);
						XIDtoPort.erase(source_xid);
					}
				}

				portToDAGinfo.erase(_sport);
				portToActive.erase(_sport);
				hlim.erase(_sport);

				nxt_xport.erase(_sport);
				xcmp_listeners.remove(_sport);
				for (int i = 0; i < MAX_WIN_SIZE; i++) {
					if (daginfo->sent_pkt[i] != NULL) {
						daginfo->sent_pkt[i]->kill();
						daginfo->sent_pkt[i] = NULL;
					}
				}
			}
		}

		if (tear_down == false) {

			// find the (next) earlist expiry
			if (daginfo->timer_on == true && daginfo->expiry > now && ( daginfo->expiry < earlist_pending_expiry || earlist_pending_expiry == now ) ) {
				earlist_pending_expiry = daginfo->expiry;
			}
			if (daginfo->timer_on == true && daginfo->teardown_expiry > now && ( daginfo->teardown_expiry < earlist_pending_expiry || earlist_pending_expiry == now ) ) {
				earlist_pending_expiry = daginfo->teardown_expiry;
			}


			// check for CID request cases
			for (HashTable<XID, bool>::iterator it = daginfo->XIDtoTimerOn.begin(); it != daginfo->XIDtoTimerOn.end(); ++it ) {
				XID requested_cid = it->first;
				bool timer_on = it->second;

				HashTable<XID, Timestamp>::iterator it2;
				it2 = daginfo->XIDtoExpiryTime.find(requested_cid);
				Timestamp cid_req_expiry = it2->second;

				if (timer_on == true && cid_req_expiry <= now) {
					//click_chatter("CID-REQ RETRANSMIT! \n");
					//retransmit cid-request
					HashTable<XID, WritablePacket*>::iterator it3;
					it3 = daginfo->XIDtoCIDreqPkt.find(requested_cid);
					copy = copy_cid_req_packet(it3->second, daginfo);
					XIAHeader xiah(copy);
					//click_chatter("\n\n (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), (char *)xiah.payload(), xiah.plen());
					output(NETWORK_PORT).push(copy);

					cid_req_expiry  = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);
					daginfo->XIDtoExpiryTime.set(requested_cid, cid_req_expiry);
					daginfo->XIDtoTimerOn.set(requested_cid, true);
				}

				if (timer_on == true && cid_req_expiry > now && ( cid_req_expiry < earlist_pending_expiry || earlist_pending_expiry == now ) ) {
					earlist_pending_expiry = cid_req_expiry;
				}
			}

			portToDAGinfo.set(_sport, *daginfo);
		}
	}

	// Set the next timer
	if (earlist_pending_expiry > now) {
		_timer.reschedule_at(earlist_pending_expiry);
	}

//	pthread_mutex_unlock(&_lock);
}

void
XTRANSPORT::copy_common(DAGinfo *daginfo, XIAHeader &xiahdr, XIAHeaderEncap &xiah) {  

	//Recalculate source path
	XID	source_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
	String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse();
	//Make source DAG _local_addr:SID
	String dagstr = daginfo->src_path.unparse_re();

	//Client Mobility...
	if (dagstr.length() != 0 && dagstr != str_local_addr) {
		//Moved!
		// 1. Update 'daginfo->src_path'
		daginfo->src_path.parse_re(str_local_addr);
	}

	xiah.set_nxt(xiahdr.nxt());
	xiah.set_last(xiahdr.last());
	xiah.set_hlim(xiahdr.hlim());
	xiah.set_dst_path(daginfo->dst_path);
	xiah.set_src_path(daginfo->src_path);
	xiah.set_plen(xiahdr.plen());
}

WritablePacket *
XTRANSPORT::copy_packet(Packet *p, DAGinfo *daginfo) {  

	XIAHeader xiahdr(p);
	XIAHeaderEncap xiah;
	copy_common(daginfo, xiahdr, xiah);

	TransportHeader thdr(p);
	TransportHeaderEncap *new_thdr = new TransportHeaderEncap(thdr.type(), thdr.pkt_info(), thdr.seq_num(), thdr.ack_num(), thdr.length());

	WritablePacket *copy = WritablePacket::make(256, thdr.payload(), xiahdr.plen() - thdr.hlen(), 20);

	copy = new_thdr->encap(copy);
	copy = xiah.encap(copy, false);
	delete new_thdr;

	return copy;
}


WritablePacket *
XTRANSPORT::copy_cid_req_packet(Packet *p, DAGinfo *daginfo) {

	XIAHeader xiahdr(p);
	XIAHeaderEncap xiah;
	copy_common(daginfo, xiahdr, xiah);

	WritablePacket *copy = WritablePacket::make(256, xiahdr.payload(), xiahdr.plen(), 20);

	ContentHeaderEncap *chdr = ContentHeaderEncap::MakeRequestHeader();

	copy = chdr->encap(copy);
	copy = xiah.encap(copy, false);
	delete chdr;
	xiah.set_plen(xiahdr.plen());

	return copy;
}


WritablePacket *
XTRANSPORT::copy_cid_response_packet(Packet *p, DAGinfo *daginfo) {

	XIAHeader xiahdr(p);
	XIAHeaderEncap xiah;
	copy_common(daginfo, xiahdr, xiah);

	WritablePacket *copy = WritablePacket::make(256, xiahdr.payload(), xiahdr.plen(), 20);

	ContentHeader chdr(p);
	ContentHeaderEncap *new_chdr = new ContentHeaderEncap(chdr.opcode(), chdr.chunk_offset(), chdr.length());

	copy = new_chdr->encap(copy);
	copy = xiah.encap(copy, false);
	delete new_chdr;
	xiah.set_plen(xiahdr.plen());

	return copy;
}

void XTRANSPORT::ProcessAPIPacket(WritablePacket *p_in)
{
		
	//Extract the destination port
	unsigned short _sport = SRC_PORT_ANNO(p_in);


//	_errh->debug("\nPush: Got packet from API sport:%d",ntohs(_sport));


	std::string p_buf;
	p_buf.assign((const char*)p_in->data(), (const char*)p_in->end_data());

	//protobuf message parsing
	xia_socket_msg.ParseFromString(p_buf);
	

	switch(xia_socket_msg.type()) {
	case xia::XSOCKET:
		Xsocket(_sport);
		break;
	case xia::XSETSOCKOPT:
		Xsetsockopt(_sport);
		break;
	case xia::XGETSOCKOPT:
		Xgetsockopt(_sport);
		break;
	case xia::XBIND:
		Xbind(_sport);
		break;
	case xia::XCLOSE:
		Xclose(_sport);
		break;
	case xia::XCONNECT:
		Xconnect(_sport);
		break;
	case xia::XACCEPT:
		Xaccept(_sport);
		break;
	case xia::XCHANGEAD:
		Xchangead(_sport);
		break;
	case xia::XREADLOCALHOSTADDR:
		Xreadlocalhostaddr(_sport);
		break;
	case xia::XUPDATENAMESERVERDAG:
		Xupdatenameserverdag(_sport);
		break;
	case xia::XREADNAMESERVERDAG:
		Xreadnameserverdag(_sport);
		break;
	case xia::XISDUALSTACKROUTER:
		Xisdualstackrouter(_sport);	
		break;
	case xia::XSEND:
		Xsend(_sport, p_in);
		break;
	case xia::XSENDTO:
		Xsendto(_sport, p_in);
		break;
	case xia::XREQUESTCHUNK:
		XrequestChunk(_sport, p_in);
		break;
	case xia::XGETCHUNKSTATUS:
		XgetChunkStatus(_sport);
		break;
	case xia::XREADCHUNK:
		XreadChunk(_sport);
		break;
	case xia::XREMOVECHUNK:
		XremoveChunk(_sport);
		break;
	case xia::XPUTCHUNK:
		XputChunk(_sport);
		break;
	case xia::XGETPEERNAME:
		Xgetpeername(_sport);
		break;
	case xia::XGETSOCKNAME:
		Xgetsockname(_sport);
		break;
	case xia::XPUSHCHUNKTO:
		XpushChunkto(_sport, p_in);
		break;
	case xia::XBINDPUSH:
		XbindPush(_sport);
		break;
	default:
		click_chatter("\n\nERROR: API TRAFFIC !!!\n\n");
		break;
	}

	p_in->kill();
}

void XTRANSPORT::ProcessNetworkPacket(WritablePacket *p_in)
{

//	_errh->debug("Got packet from network");


	//Extract the SID/CID
	XIAHeader xiah(p_in->xia_header());
	XIAPath dst_path = xiah.dst_path();
	XID _destination_xid(xiah.hdr()->node[xiah.last()].xid);
	//TODO:In case of stream use source AND destination XID to find port, if not found use source. No TCP like protocol exists though
	//TODO:pass dag back to recvfrom. But what format?

	XIAPath src_path = xiah.src_path();
	XID	_source_xid = src_path.xid(src_path.destination_node());
	
// 	click_chatter("NetworkPacket, Src: %s, Dest: %s", xiah.dst_path().unparse().c_str(), xiah.src_path().unparse().c_str());

	unsigned short _dport = XIDtoPort.get(_destination_xid);  // This is to be updated for the XSOCK_STREAM type connections below

	bool sendToApplication = true;
	//String pld((char *)xiah.payload(), xiah.plen());
	//click_chatter("\n\n 1. (%s) Received=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), pld.c_str(), xiah.plen());

	TransportHeader thdr(p_in);

	if (xiah.nxt() == CLICK_XIA_NXT_XCMP) {
		// FIXME: This shouldn't strip off the header. raw sockets return raw packets. 
		// (Matt): I've tweaked this to work properly
		// strip off the header and make a writable packet

		String src_path = xiah.src_path().unparse();
		String header((const char*)xiah.hdr(), xiah.hdr_size());
		String payload((const char*)xiah.payload(), xiah.plen());//+xiah.hdr_size());
		//String payload((const char*)thdr.payload(), xiah.plen() - thdr.hlen());
		String str = header + payload;

		xia::XSocketMsg xsm;
		xsm.set_type(xia::XRECV);
		xia::X_Recv_Msg *x_recv_msg = xsm.mutable_x_recv();
		x_recv_msg->set_dag(src_path.c_str());
		x_recv_msg->set_payload(str.c_str(), str.length());

		std::string p_buf;
		xsm.SerializeToString(&p_buf);

		WritablePacket *xcmp_pkt = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);

		list<int>::iterator i;

		for (i = xcmp_listeners.begin(); i != xcmp_listeners.end(); i++) {
			int port = *i;
			output(API_PORT).push(UDPIPPrep(xcmp_pkt, port));
		}

		return;

	} else if (thdr.type() == TransportHeader::XSOCK_STREAM) {

		//click_chatter("stream socket dport = %d\n", _dport);
		if (thdr.pkt_info() == TransportHeader::SYN) {
			//click_chatter("syn dport = %d\n", _dport);
			// Connection request from client...

			// First, check if this request is already in the pending queue
			XIDpair xid_pair;
			xid_pair.set_src(_destination_xid);
			xid_pair.set_dst(_source_xid);

			HashTable<XIDpair , bool>::iterator it;
			it = XIDpairToConnectPending.find(xid_pair);

			// FIXME:
			// XIDpairToConnectPending never gets cleared, and will cause problems if matching XIDs
			// were used previously. Commenting out the check for now. Need to look into whether
			// or not we can just get rid of this logic? probably neede for retransmit cases
			// if needed, where should it be cleared???
			if (1) {
//			if (it == XIDpairToConnectPending.end()) {
				// if this is new request, put it in the queue

				// Todo: 1. prepare new Daginfo and store it
				//	 2. send SYNACK to client
				//	   3. Notify the api of SYN reception

				//1. Prepare new DAGinfo for this connection
				DAGinfo daginfo;
				daginfo.port = -1; // just for now. This will be updated via Xaccept call

				daginfo.sock_type = 0; // 0: Reliable transport, 1: Unreliable transport

				daginfo.dst_path = src_path;
				daginfo.src_path = dst_path;
				daginfo.isConnected = true;
				daginfo.initialized = true;
				daginfo.nxt = LAST_NODE_DEFAULT;
				daginfo.last = LAST_NODE_DEFAULT;
				daginfo.hlim = HLIM_DEFAULT;
				daginfo.seq_num = 0;
				daginfo.ack_num = 0;

				pending_connection_buf.push(daginfo);

				// Mark these src & dst XID pair
				XIDpairToConnectPending.set(xid_pair, true);

				//portToDAGinfo.set(-1, daginfo);	// just for now. This will be updated via Xaccept call

			} else {
				// If already in the pending queue, just send back SYNACK to the requester
			
				// if this case is hit, we won't trigger the accept, and the connection will get be left
				// in limbo. see above for whether or not we should even be checking.
				// click_chatter("%06d conn request already pending\n", _dport);
				sendToApplication = false;
			}


			//2. send SYNACK to client
			//Add XIA headers
			XIAHeaderEncap xiah_new;
			xiah_new.set_nxt(CLICK_XIA_NXT_TRN);
			xiah_new.set_last(LAST_NODE_DEFAULT);
			xiah_new.set_hlim(HLIM_DEFAULT);
			xiah_new.set_dst_path(src_path);
			xiah_new.set_src_path(dst_path);

			const char* dummy = "Connection_granted";
			WritablePacket *just_payload_part = WritablePacket::make(256, dummy, strlen(dummy), 0);

			WritablePacket *p = NULL;

			xiah_new.set_plen(strlen(dummy));
			//click_chatter("Sent packet to network");

			TransportHeaderEncap *thdr_new = TransportHeaderEncap::MakeSYNACKHeader( 0, 0, 0); // #seq, #ack, length
			p = thdr_new->encap(just_payload_part);

			thdr_new->update();
			xiah_new.set_plen(strlen(dummy) + thdr_new->hlen()); // XIA payload = transport header + transport-layer data

			p = xiah_new.encap(p, false);

			delete thdr_new;
			XIAHeader xiah1(p);
			//String pld((char *)xiah1.payload(), xiah1.plen());
			//click_chatter("\n\n (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), pld.c_str(), xiah1.plen());
			output(NETWORK_PORT).push(p);


			// 3. Notify the api of SYN reception
			//   Done below (via port#5005)

		} else if (thdr.pkt_info() == TransportHeader::SYNACK) {
			XIDpair xid_pair;
			xid_pair.set_src(_destination_xid);
			xid_pair.set_dst(_source_xid);

			// Get the dst port from XIDpair table
			_dport = XIDpairToPort.get(xid_pair);

			DAGinfo *daginfo = portToDAGinfo.get_pointer(_dport);

			if(!daginfo->synack_waiting) {
				// Fix for synack storm sending messages up to the API
				// still need to fix the root cause, but this prevents the API from 
				// getting multiple connection granted messages
				sendToApplication = false;
			}

			// Clear timer
			daginfo->timer_on = false;
			daginfo->synack_waiting = false;
			//daginfo->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

		} else if (thdr.pkt_info() == TransportHeader::DATA) {
			XIDpair xid_pair;
			xid_pair.set_src(_destination_xid);
			xid_pair.set_dst(_source_xid);

			// Get the dst port from XIDpair table
			_dport = XIDpairToPort.get(xid_pair);

			//click_chatter("(%s) my_sport=%d  my_sid=%s  his_sid=%s\n", (_local_addr.unparse()).c_str(),  _dport,  _destination_xid.unparse().c_str(), _source_xid.unparse().c_str());
			HashTable<unsigned short, bool>::iterator it1;
			it1 = portToActive.find(_dport);

			if(it1 != portToActive.end() ) {

				DAGinfo *daginfo = portToDAGinfo.get_pointer(_dport);

				if (thdr.seq_num() == daginfo->expected_seqnum) {
					daginfo->expected_seqnum++;
					//click_chatter("(%s) Accept Received data (now expected seq=%d)\n", (_local_addr.unparse()).c_str(), daginfo->expected_seqnum);
				} else {
					sendToApplication = false;
					click_chatter("expected sequence # %d, received %d\n", daginfo->expected_seqnum, thdr.seq_num());
					click_chatter("(%s) Discarded Received data\n", (_local_addr.unparse()).c_str());
				}

				portToDAGinfo.set(_dport, *daginfo);
			
				//In case of Client Mobility...	 Update 'daginfo->dst_path'
				daginfo->dst_path = src_path;		

				// send the cumulative ACK to the sender
				//Add XIA headers
				XIAHeaderEncap xiah_new;
				xiah_new.set_nxt(CLICK_XIA_NXT_TRN);
				xiah_new.set_last(LAST_NODE_DEFAULT);
				xiah_new.set_hlim(HLIM_DEFAULT);
				xiah_new.set_dst_path(src_path);
				xiah_new.set_src_path(dst_path);

				const char* dummy = "cumulative_ACK";
				WritablePacket *just_payload_part = WritablePacket::make(256, dummy, strlen(dummy), 0);

				WritablePacket *p = NULL;

				xiah_new.set_plen(strlen(dummy));
				//click_chatter("Sent packet to network");

				TransportHeaderEncap *thdr_new = TransportHeaderEncap::MakeACKHeader( 0, daginfo->expected_seqnum, 0); // #seq, #ack, length
				p = thdr_new->encap(just_payload_part);

				thdr_new->update();
				xiah_new.set_plen(strlen(dummy) + thdr_new->hlen()); // XIA payload = transport header + transport-layer data

				p = xiah_new.encap(p, false);
				delete thdr_new;

				XIAHeader xiah1(p);
				String pld((char *)xiah1.payload(), xiah1.plen());
				//click_chatter("\n\n (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), pld.c_str(), xiah1.plen());

				output(NETWORK_PORT).push(p);

			} else {
				click_chatter("destination port not found: %d\n", _dport);
				sendToApplication = false;
			}

		} else if (thdr.pkt_info() == TransportHeader::ACK) {
			sendToApplication = false;

			XIDpair xid_pair;
			xid_pair.set_src(_destination_xid);
			xid_pair.set_dst(_source_xid);

			// Get the dst port from XIDpair table
			_dport = XIDpairToPort.get(xid_pair);

			HashTable<unsigned short, bool>::iterator it1;
			it1 = portToActive.find(_dport);

			if(it1 != portToActive.end() ) {
				DAGinfo *daginfo = portToDAGinfo.get_pointer(_dport);
			
				//In case of Client Mobility...	 Update 'daginfo->dst_path'
				daginfo->dst_path = src_path;					

				int expected_seqnum = thdr.ack_num();

				bool resetTimer = false;

				// Clear all Acked packets
				for (int i = daginfo->base; i < expected_seqnum; i++) {
					int idx = i % MAX_WIN_SIZE;
					if (daginfo->sent_pkt[idx]) {
						daginfo->sent_pkt[idx]->kill();
						daginfo->sent_pkt[idx] = NULL;
					}
				
					resetTimer = true;
				}

				// Update the variables
				daginfo->base = expected_seqnum;

				// Reset timer
				if (resetTimer) {
					daginfo->timer_on = true;
					daginfo->dataack_waiting = true;
					// FIXME: should we reset retransmit_tries here?
					daginfo->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

					if (! _timer.scheduled() || _timer.expiry() >= daginfo->expiry )
						_timer.reschedule_at(daginfo->expiry);

					if (daginfo->base == daginfo->next_seqnum) {

						// Clear timer
						daginfo->timer_on = false;
						daginfo->dataack_waiting = false;
						daginfo->num_retransmit_tries = 0;
						//daginfo->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);
					}
				}

				portToDAGinfo.set(_dport, *daginfo);

			} else {
				//click_chatter("port not found\n");
			}

		} else if (thdr.pkt_info() == TransportHeader::FIN) {
			//click_chatter("FIN received, doing nothing\n");
		}
		else {
			click_chatter("UNKNOWN dport = %d send = %d hdr=%d\n", _dport, sendToApplication, thdr.pkt_info());		
		}

	} else if (thdr.type() == TransportHeader::XSOCK_DGRAM) {

		_dport = XIDtoPort.get(_destination_xid);
		DAGinfo *daginfo = portToDAGinfo.get_pointer(_dport);
		// check if _destination_sid is of XSOCK_DGRAM
		if (daginfo->sock_type != XSOCKET_DGRAM) {
			sendToApplication = false;
		}
	
	} else {
		click_chatter("UNKNOWN!!!!! dport = %d\n", _dport);
	}


	if(_dport && sendToApplication) {
		//TODO: Refine the way we change DAG in case of migration. Use some control bits. Add verification
		DAGinfo daginfo = portToDAGinfo.get(_dport);

		if(daginfo.initialized == false) {
			daginfo.dst_path = xiah.src_path();
			daginfo.initialized = true;
			portToDAGinfo.set(_dport, daginfo);
		}

		// FIXME: what is this? need constant here
		if(xiah.nxt() == 22 && daginfo.isConnected == true)
		{
			//Verify mobility info
			daginfo.dst_path = xiah.src_path();
			portToDAGinfo.set(_dport, daginfo);
			click_chatter("Sender moved, update to the new DAG");

		} else {
			//Unparse dag info
			String src_path = xiah.src_path().unparse();
			String payload((const char*)thdr.payload(), xiah.plen() - thdr.hlen());

			xia::XSocketMsg xsm;
			xsm.set_type(xia::XRECV);
			xia::X_Recv_Msg *x_recv_msg = xsm.mutable_x_recv();
			x_recv_msg->set_dag(src_path.c_str());
			x_recv_msg->set_payload(payload.c_str(), payload.length());

			std::string p_buf;
			xsm.SerializeToString(&p_buf);

			WritablePacket *p2 = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);

			//_errh->debug("Sent packet to socket with port %d", _dport);
			output(API_PORT).push(UDPIPPrep(p2, _dport));
		}

	} else {
		if (!_dport) {
			click_chatter("Packet to unknown port %d XID=%s, sendToApp=%d", _dport, _destination_xid.unparse().c_str(), sendToApplication );
		}
	}
}

void XTRANSPORT::ProcessCachePacket(WritablePacket *p_in)
{

 	_errh->debug("Got packet from cache");		

	
	//Extract the SID/CID
	XIAHeader xiah(p_in->xia_header());
	XIAPath dst_path = xiah.dst_path();
	XIAPath src_path = xiah.src_path();
	XID	destination_sid = dst_path.xid(dst_path.destination_node());
	XID	source_cid = src_path.xid(src_path.destination_node());	
	
        ContentHeader ch(p_in);
	
// 	click_chatter("dest %s, src_cid %s, dst_path: %s, src_path: %s\n", 
// 		      destination_sid.unparse().c_str(), source_cid.unparse().c_str(), dst_path.unparse().c_str(), src_path.unparse().c_str());
// 	click_chatter("dst_path: %s, src_path: %s, OPCode: %d\n", dst_path.unparse().c_str(), src_path.unparse().c_str(), ch.opcode());
	
	
        if(ch.opcode()==ContentHeader::OP_PUSH){
		// compute the hash and verify it matches the CID
		String hash = "CID:";
		char hexBuf[3];
		int i = 0;
		SHA1_ctx sha_ctx;
		unsigned char digest[HASH_KEYSIZE];
		SHA1_init(&sha_ctx);
		SHA1_update(&sha_ctx, (unsigned char *)xiah.payload(), xiah.plen());
		SHA1_final(digest, &sha_ctx);
		for(i = 0; i < HASH_KEYSIZE; i++) {
			sprintf(hexBuf, "%02x", digest[i]);
			hash.append(const_cast<char *>(hexBuf), 2);
		}

// 		int status = READY_TO_READ;
		if (hash != source_cid.unparse()) {
			click_chatter("CID with invalid hash received: %s\n", source_cid.unparse().c_str());
// 			status = INVALID_HASH;
		}
		
		unsigned short _dport = XIDtoPushPort.get(destination_sid);
		if(!_dport){
			click_chatter("Couldn't find SID to send to: %s\n", destination_sid.unparse().c_str());
			return;
		}
		
		DAGinfo *daginfo = portToDAGinfo.get_pointer(_dport);
		// check if _destination_sid is of XSOCK_DGRAM
		if (daginfo->sock_type != XSOCKET_CHUNK) {
			click_chatter("This is not a chunk socket. dport: %i, Socktype: %i", _dport, daginfo->sock_type);
		}
		
		// Send pkt up

		//Unparse dag info
		String src_path = xiah.src_path().unparse();

		xia::XSocketMsg xia_socket_msg;
		xia_socket_msg.set_type(xia::XPUSHCHUNKTO);
		xia::X_Pushchunkto_Msg *x_pushchunkto_msg = xia_socket_msg.mutable_x_pushchunkto();
		x_pushchunkto_msg->set_cid(source_cid.unparse().c_str());
		x_pushchunkto_msg->set_payload((const char*)xiah.payload(), xiah.plen());
		x_pushchunkto_msg->set_cachepolicy(ch.cachePolicy());
		x_pushchunkto_msg->set_ttl(ch.ttl());
		x_pushchunkto_msg->set_cachesize(ch.cacheSize());
		x_pushchunkto_msg->set_contextid(ch.contextID());
		x_pushchunkto_msg->set_length(ch.length());
 		x_pushchunkto_msg->set_ddag(dst_path.unparse().c_str());

		std::string p_buf;
		xia_socket_msg.SerializeToString(&p_buf);

		WritablePacket *p2 = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);

		//click_chatter("FROM CACHE. data length = %d  \n", str.length());
		_errh->debug("Sent packet to socket: sport %d dport %d", _dport, _dport);

		output(API_PORT).push(UDPIPPrep(p2, _dport));
		return;
		
		
	}

	XIDpair xid_pair;
	xid_pair.set_src(destination_sid);
	xid_pair.set_dst(source_cid);

	unsigned short _dport = XIDpairToPort.get(xid_pair);
	
// 	click_chatter(">>packet from processCACHEpackets %d\n", _dport);
// 	click_chatter("CachePacket, Src: %s, Dest: %s, Local: %s", xiah.dst_path().unparse().c_str(),
// 			      xiah.src_path().unparse().c_str(), _local_addr.unparse_re().c_str());
	click_chatter("CachePacket, dest: %s, src_cid %s OPCode: %d \n", destination_sid.unparse().c_str(), source_cid.unparse().c_str(), ch.opcode());
// 	click_chatter("dst_path: %s, src_path: %s, OPCode: %d\n", dst_path.unparse().c_str(), src_path.unparse().c_str(), ch.opcode());

	if(_dport)
	{
		//TODO: Refine the way we change DAG in case of migration. Use some control bits. Add verification
		//DAGinfo daginfo=portToDAGinfo.get(_dport);
		//daginfo.dst_path=xiah.src_path();
		//portToDAGinfo.set(_dport,daginfo);
		//ENDTODO

		DAGinfo *daginfo = portToDAGinfo.get_pointer(_dport);

		// Reset timer or just Remove the corresponding entry in the hash tables (Done below)
		HashTable<XID, WritablePacket*>::iterator it1;
		it1 = daginfo->XIDtoCIDreqPkt.find(source_cid);

		if(it1 != daginfo->XIDtoCIDreqPkt.end() ) {
			// Remove the entry
			daginfo->XIDtoCIDreqPkt.erase(it1);
		}

		HashTable<XID, Timestamp>::iterator it2;
		it2 = daginfo->XIDtoExpiryTime.find(source_cid);

		if(it2 != daginfo->XIDtoExpiryTime.end()) {
			// Remove the entry
			daginfo->XIDtoExpiryTime.erase(it2);
		}

		HashTable<XID, bool>::iterator it3;
		it3 = daginfo->XIDtoTimerOn.find(source_cid);

		if(it3 != daginfo->XIDtoTimerOn.end()) {
			// Remove the entry
			daginfo->XIDtoTimerOn.erase(it3);
		}

		// compute the hash and verify it matches the CID
		String hash = "CID:";
		char hexBuf[3];
		int i = 0;
		SHA1_ctx sha_ctx;
		unsigned char digest[HASH_KEYSIZE];
		SHA1_init(&sha_ctx);
		SHA1_update(&sha_ctx, (unsigned char *)xiah.payload(), xiah.plen());
		SHA1_final(digest, &sha_ctx);
		for(i = 0; i < HASH_KEYSIZE; i++) {
			sprintf(hexBuf, "%02x", digest[i]);
			hash.append(const_cast<char *>(hexBuf), 2);
		}

		int status = READY_TO_READ;
		if (hash != source_cid.unparse()) {
			click_chatter("CID with invalid hash received: %s\n", source_cid.unparse().c_str());
			status = INVALID_HASH;
		}

		// Update the status of CID request
		daginfo->XIDtoStatus.set(source_cid, status);

		// Check if the ReadCID() was called for this CID
		HashTable<XID, bool>::iterator it4;
		it4 = daginfo->XIDtoReadReq.find(source_cid);

		if(it4 != daginfo->XIDtoReadReq.end()) {
			// There is an entry
			bool read_cid_req = it4->second;

			if (read_cid_req == true) {
				// Send pkt up
				daginfo->XIDtoReadReq.erase(it4);

				portToDAGinfo.set(_dport, *daginfo);

				//Unparse dag info
				String src_path = xiah.src_path().unparse();

				xia::XSocketMsg xia_socket_msg;
				xia_socket_msg.set_type(xia::XREADCHUNK);
				xia::X_Readchunk_Msg *x_readchunk_msg = xia_socket_msg.mutable_x_readchunk();
				x_readchunk_msg->set_dag(src_path.c_str());
				x_readchunk_msg->set_payload((const char*)xiah.payload(), xiah.plen());

				std::string p_buf;
				xia_socket_msg.SerializeToString(&p_buf);

				WritablePacket *p2 = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);

				//click_chatter("FROM CACHE. data length = %d  \n", str.length());
				_errh->debug("Sent packet to socket: sport %d dport %d \n", _dport, _dport);

				output(API_PORT).push(UDPIPPrep(p2, _dport));

			} else {
				// Store the packet into temp buffer (until ReadCID() is called for this CID)
				WritablePacket *copy_response_pkt = copy_cid_response_packet(p_in, daginfo);
				daginfo->XIDtoCIDresponsePkt.set(source_cid, copy_response_pkt);

				portToDAGinfo.set(_dport, *daginfo);
			}

		} else {
			WritablePacket *copy_response_pkt = copy_cid_response_packet(p_in, daginfo);
			daginfo->XIDtoCIDresponsePkt.set(source_cid, copy_response_pkt);
			portToDAGinfo.set(_dport, *daginfo);
		}
	}
	else
	{
		click_chatter("Case 2. Packet to unknown %s, src_cid %s\n", destination_sid.unparse().c_str(), source_cid.unparse().c_str());
// 		click_chatter("Case 2. Packet to unknown dest %s, src_cid %s, dst_path: %s, src_path: %s\n", 
// 		      destination_sid.unparse().c_str(), source_cid.unparse().c_str(), dst_path.unparse().c_str(), src_path.unparse().c_str());
// 		click_chatter("Case 2. Packet to unknown  dst_path: %s, src_path: %s\n", dst_path.unparse().c_str(), src_path.unparse().c_str());

	  
	}
	
	
	
	

	
}

void XTRANSPORT::ProcessXhcpPacket(WritablePacket *p_in)
{
	XIAHeader xiah(p_in->xia_header());
	String temp = _local_addr.unparse();
	Vector<String> ids;
	cp_spacevec(temp, ids);;
	if (ids.size() < 3) {
		String new_route((char *)xiah.payload());
		String new_local_addr = new_route + " " + ids[1];
		click_chatter("new address is - %s", new_local_addr.c_str());
		_local_addr.parse(new_local_addr);
	}
}


void XTRANSPORT::push(int port, Packet *p_input)
{
//	pthread_mutex_lock(&_lock);

	WritablePacket *p_in = p_input->uniqueify();
	//Depending on which CLICK-module-port it arrives at it could be control/API traffic/Data traffic

	switch(port) { // This is a "CLICK" port of UDP module.
	case API_PORT:	// control packet from socket API
		ProcessAPIPacket(p_in);
		break;

	case BAD_PORT: //packet from ???
		_errh->debug("\n\nERROR: BAD INPUT PORT TO XTRANSPORT!!!\n\n");
		break;

	case NETWORK_PORT: //Packet from network layer
		ProcessNetworkPacket(p_in);
		p_in->kill();
		break;

	case CACHE_PORT:	//Packet from cache
//  		click_chatter(">>>>> Cache Port packet from socket Cache %d\n", port);
		ProcessCachePacket(p_in);
		p_in->kill();
		break;

	case XHCP_PORT:		//Packet with DHCP information
		ProcessXhcpPacket(p_in);
		p_in->kill();
		break;

	default:
		click_chatter("packet from unknown port: %d\n", port);
		break;
	}

//	pthread_mutex_unlock(&_lock);
}

void XTRANSPORT::ReturnResult(int sport, xia::XSocketCallType type, int rc, int err)
{
//	click_chatter("sport=%d type=%d rc=%d err=%d\n", sport, type, rc, err);
	xia::XSocketMsg xia_socket_msg_response;
	xia_socket_msg_response.set_type(xia::XRESULT);
	xia::X_Result_Msg *x_result = xia_socket_msg_response.mutable_x_result();
	x_result->set_type(type);
	x_result->set_return_code(rc);
	x_result->set_err_code(err);

	std::string p_buf;
	xia_socket_msg_response.SerializeToString(&p_buf);
	WritablePacket *reply = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, sport));
}

Packet *
XTRANSPORT::UDPIPPrep(Packet *p_in, int dport)
{
    p_in->set_dst_ip_anno(IPAddress("127.0.0.1"));
    SET_DST_PORT_ANNO(p_in, dport);

	return p_in;
}


enum {H_MOVE};

int XTRANSPORT::write_param(const String &conf, Element *e, void *vparam,
							ErrorHandler *errh)
{
	XTRANSPORT *f = static_cast<XTRANSPORT *>(e);
	switch(reinterpret_cast<intptr_t>(vparam)) {
	case H_MOVE:
	{
		XIAPath local_addr;
		if (cp_va_kparse(conf, f, errh,
						 "LOCAL_ADDR", cpkP + cpkM, cpXIAPath, &local_addr,
						 cpEnd) < 0)
			return -1;
		f->_local_addr = local_addr;
		click_chatter("Moved to %s", local_addr.unparse().c_str());
		f->_local_hid = local_addr.xid(local_addr.destination_node());

	}
	break;
	default:
		break;
	}
	return 0;
}

void XTRANSPORT::add_handlers() {
	add_write_handler("local_addr", write_param, (void *)H_MOVE);
}

/*
** Handler for the Xsocket API call
**
** FIXME: why is xia_socket_msg part of the xtransport class and not a local variable?????
*/
void XTRANSPORT::Xsocket(unsigned short _sport) {
	//Open socket.
	//click_chatter("Xsocket: create socket %d\n", _sport);

	xia::X_Socket_Msg *x_socket_msg = xia_socket_msg.mutable_x_socket();
	int sock_type = x_socket_msg->type();

	//Set the source port in DAGinfo
	DAGinfo daginfo;
	daginfo.port = _sport;
	daginfo.timer_on = false;
	daginfo.synack_waiting = false;
	daginfo.dataack_waiting = false;
	daginfo.num_retransmit_tries = 0;
	daginfo.teardown_waiting = false;
	daginfo.isAcceptSocket = false;
	daginfo.num_connect_tries = 0; // number of xconnect tries (Xconnect will fail after MAX_CONNECT_TRIES trials)
	memset(daginfo.sent_pkt, 0, MAX_WIN_SIZE * sizeof(WritablePacket*));

	//Set the socket_type (reliable or not) in DAGinfo
	daginfo.sock_type = sock_type;

	// Map the source port to DagInfo
	portToDAGinfo.set(_sport, daginfo);

	portToActive.set(_sport, true);

	hlim.set(_sport, HLIM_DEFAULT);
	nxt_xport.set(_sport, CLICK_XIA_NXT_TRN);

	// click_chatter("XSOCKET: sport=%hu\n", _sport);

	// (for Ack purpose) Reply with a packet with the destination port=source port
	ReturnResult(_sport, xia::XSOCKET, 0);
	// output(API_PORT).push(UDPIPPrep(p_in,_sport));
}

/*
** Xsetsockopt API handler
*/
void XTRANSPORT::Xsetsockopt(unsigned short _sport) {

	// click_chatter("\nSet Socket Option\n");
	xia::X_Setsockopt_Msg *x_sso_msg = xia_socket_msg.mutable_x_setsockopt();

	switch (x_sso_msg->opt_type())
	{
		// FIXME: need real opt type for protobufs
	case 1:
	{
		int hl = x_sso_msg->int_opt();
	
		hlim.set(_sport, hl);
		//click_chatter("sso:hlim:%d\n",hl);
	}
	break;

	case 2:
	{
		int nxt = x_sso_msg->int_opt();
		nxt_xport.set(_sport, nxt);
		if (nxt == CLICK_XIA_NXT_XCMP)
			xcmp_listeners.push_back(_sport);
		else
			xcmp_listeners.remove(_sport);
	}
	break;

	default:
		// unsupported option
		break;
	}
	ReturnResult(_sport, xia::XSETSOCKOPT);
}

/*
** Xgetsockopt API handler
*/
void XTRANSPORT::Xgetsockopt(unsigned short _sport) {
	// click_chatter("\nGet Socket Option\n");
	xia::X_Getsockopt_Msg *x_sso_msg = xia_socket_msg.mutable_x_getsockopt();

	// click_chatter("opt = %d\n", x_sso_msg->opt_type());
	switch (x_sso_msg->opt_type())
	{
	// FIXME: need real opt type for protobufs
	case 1:
	{
		x_sso_msg->set_int_opt(hlim.get(_sport));
		//click_chatter("gso:hlim:%d\n", hlim.get(_sport));
	}
	break;

	case 2:
	{
		x_sso_msg->set_int_opt(nxt_xport.get(_sport));
	}
	break;

	default:
		// unsupported option
		break;
	}
	std::string p_buf;
	xia_socket_msg.SerializeToString(&p_buf);

	WritablePacket *reply = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

void XTRANSPORT::Xbind(unsigned short _sport) {

	int rc = 0, ec = 0;

	//Bind XID
	//click_chatter("\n\nOK: SOCKET BIND !!!\\n");
	//get source DAG from protobuf message

	xia::X_Bind_Msg *x_bind_msg = xia_socket_msg.mutable_x_bind();

	String sdag_string(x_bind_msg->sdag().c_str(), x_bind_msg->sdag().size());

	//String sdag_string((const char*)p_in->data(),(const char*)p_in->end_data());
//	_errh->debug("\nbind requested to %s, length=%d\n", sdag_string.c_str(), (int)p_in->length());

	//String str_local_addr=_local_addr.unparse();
	//str_local_addr=str_local_addr+" "+xid_string;//Make source DAG _local_addr:SID

	//Set the source DAG in DAGinfo
	DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);
	if (daginfo->src_path.parse(sdag_string)) {
		daginfo->nxt = LAST_NODE_DEFAULT;
		daginfo->last = LAST_NODE_DEFAULT;
		daginfo->hlim = hlim.get(_sport);
		daginfo->isConnected = false;
		daginfo->initialized = true;
		daginfo->sdag = sdag_string;

		//Check if binding to full DAG or just to SID only
		Vector<XIAPath::handle_t> xids = daginfo->src_path.next_nodes( daginfo->src_path.source_node() );		
		XID front_xid = daginfo->src_path.xid( xids[0] );
		struct click_xia_xid head_xid = front_xid.xid();
		uint32_t head_xid_type = head_xid.type;
		if(head_xid_type == _sid_type) {
			daginfo->full_src_dag = false; 
		} else {
			daginfo->full_src_dag = true;
		}

		XID	source_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
		//XID xid(xid_string);
		//TODO: Add a check to see if XID is already being used

		// Map the source XID to source port (for now, for either type of tranports)
		XIDtoPort.set(source_xid, _sport);
		addRoute(source_xid);

		portToDAGinfo.set(_sport, *daginfo);

		//click_chatter("Bound");
		//click_chatter("set %d %d",_sport, __LINE__);

	} else {
		rc = -1;
		ec = EADDRNOTAVAIL;
	}

	// (for Ack purpose) Reply with a packet with the destination port=source port
	ReturnResult(_sport, xia::XBIND, rc, ec);
}

// FIXME: This way of doing things is a bit hacky.
void XTRANSPORT::XbindPush(unsigned short _sport) {

	int rc = 0, ec = 0;

	//Bind XID
// 	click_chatter("\n\nOK: SOCKET BIND !!!\\n");
	//get source DAG from protobuf message

	xia::X_BindPush_Msg *x_bindpush_msg = xia_socket_msg.mutable_x_bindpush();

	String sdag_string(x_bindpush_msg->sdag().c_str(), x_bindpush_msg->sdag().size());

	//String sdag_string((const char*)p_in->data(),(const char*)p_in->end_data());

//	_errh->debug("\nbind requested to %s, length=%d\n", sdag_string.c_str(), (int)p_in->length());

	//String str_local_addr=_local_addr.unparse();
	//str_local_addr=str_local_addr+" "+xid_string;//Make source DAG _local_addr:SID

	//Set the source DAG in DAGinfo
	DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);
	if (daginfo->src_path.parse(sdag_string)) {
		daginfo->nxt = LAST_NODE_DEFAULT;
		daginfo->last = LAST_NODE_DEFAULT;
		daginfo->hlim = hlim.get(_sport);
		daginfo->isConnected = false;
		daginfo->initialized = true;
		daginfo->sdag = sdag_string;

		//Check if binding to full DAG or just to SID only
		Vector<XIAPath::handle_t> xids = daginfo->src_path.next_nodes( daginfo->src_path.source_node() );		
		XID front_xid = daginfo->src_path.xid( xids[0] );
		struct click_xia_xid head_xid = front_xid.xid();
		uint32_t head_xid_type = head_xid.type;
		if(head_xid_type == _sid_type) {
			daginfo->full_src_dag = false; 
		} else {
			daginfo->full_src_dag = true;
		}

		XID	source_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
		//XID xid(xid_string);
		//TODO: Add a check to see if XID is already being used

		// Map the source XID to source port (for now, for either type of tranports)
		XIDtoPushPort.set(source_xid, _sport);
		addRoute(source_xid);

		portToDAGinfo.set(_sport, *daginfo);

		//click_chatter("Bound");
		//click_chatter("set %d %d",_sport, __LINE__);

	} else {
		rc = -1;
		ec = EADDRNOTAVAIL;
		click_chatter("\n\nERROR: SOCKET PUSH BIND !!!\\n");
	}
	
	// (for Ack purpose) Reply with a packet with the destination port=source port
// 	click_chatter("\n\nPUSHBIND: DONE, SENDING ACK !!!\\n");
	ReturnResult(_sport, xia::XBINDPUSH, rc, ec);
// 	click_chatter("\n\nAFTER PUSHBIND: DONE, SENDING ACK !!!\\n");
}

void XTRANSPORT::Xclose(unsigned short _sport)
{
	// Close port
	//click_chatter("Xclose: closing %d\n", _sport);

	DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);

	// Set timer
	daginfo->timer_on = true;
	daginfo->teardown_waiting = true;
	daginfo->teardown_expiry = Timestamp::now() + Timestamp::make_msec(_teardown_wait_ms);

	if (! _timer.scheduled() || _timer.expiry() >= daginfo->teardown_expiry )
		_timer.reschedule_at(daginfo->teardown_expiry);

	portToDAGinfo.set(_sport, *daginfo);

	xcmp_listeners.remove(_sport);

	// (for Ack purpose) Reply with a packet with the destination port=source port
	ReturnResult(_sport, xia::XCLOSE);
}

void XTRANSPORT::Xconnect(unsigned short _sport)
{
	//click_chatter("Xconect: connecting %d\n", _sport);

	//isConnected=true;
	//String dest((const char*)p_in->data(),(const char*)p_in->end_data());
	//click_chatter("\nconnect to %s, length=%d\n",dest.c_str(),(int)p_in->length());

	xia::X_Connect_Msg *x_connect_msg = xia_socket_msg.mutable_x_connect();

	String dest(x_connect_msg->ddag().c_str());

	//String sdag_string((const char*)p_in->data(),(const char*)p_in->end_data());
	//click_chatter("\nconnect requested to %s, length=%d\n",dest.c_str(),(int)p_in->length());

	XIAPath dst_path;
	dst_path.parse(dest);

	DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);
	//click_chatter("connect %d %x",_sport, daginfo);

	if(!daginfo) {
		//click_chatter("Create DAGINFO connect %d %x",_sport, daginfo);
		//No local SID bound yet, so bind ephemeral one
		daginfo = new DAGinfo();
	}

	daginfo->dst_path = dst_path;
	daginfo->port = _sport;
	daginfo->isConnected = true;
	daginfo->initialized = true;
	daginfo->ddag = dest;
	daginfo->seq_num = 0;
	daginfo->ack_num = 0;
	daginfo->base = 0;
	daginfo->next_seqnum = 0;
	daginfo->expected_seqnum = 0;
	daginfo->num_connect_tries++; // number of xconnect tries (Xconnect will fail after MAX_CONNECT_TRIES trials)

	String str_local_addr = _local_addr.unparse_re();
	//String dagstr = daginfo->src_path.unparse_re();

	/* Use src_path set by Xbind() if exists */
	if(daginfo->sdag.length() == 0) {
		char xid_string[50];
		random_xid("SID", xid_string);

//		String rand(click_random(1000000, 9999999));
//		String xid_string = "SID:20000ff00000000000000000000000000" + rand;
//		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
		daginfo->src_path.parse_re(str_local_addr);
	}

	daginfo->nxt = LAST_NODE_DEFAULT;
	daginfo->last = LAST_NODE_DEFAULT;
	daginfo->hlim = hlim.get(_sport);

	XID source_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
	XID destination_xid = daginfo->dst_path.xid(daginfo->dst_path.destination_node());

	XIDpair xid_pair;
	xid_pair.set_src(source_xid);
	xid_pair.set_dst(destination_xid);

	// Map the src & dst XID pair to source port
	//click_chatter("setting pair to port1 %d\n", _sport);

	XIDpairToPort.set(xid_pair, _sport);

	// Map the source XID to source port
	XIDtoPort.set(source_xid, _sport);
	addRoute(source_xid);

	// click_chatter("XCONNECT: set %d %x",_sport, daginfo);

	// Prepare SYN packet

	//Add XIA headers
	XIAHeaderEncap xiah;
	xiah.set_nxt(CLICK_XIA_NXT_TRN);
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim.get(_sport));
	xiah.set_dst_path(dst_path);
	xiah.set_src_path(daginfo->src_path);

	//click_chatter("Sent packet to network");
	const char* dummy = "Connection_request";
	WritablePacket *just_payload_part = WritablePacket::make(256, dummy, strlen(dummy), 20);

	WritablePacket *p = NULL;

	TransportHeaderEncap *thdr = TransportHeaderEncap::MakeSYNHeader( 0, -1, 0); // #seq, #ack, length

	p = thdr->encap(just_payload_part);

	thdr->update();
	xiah.set_plen(strlen(dummy) + thdr->hlen()); // XIA payload = transport header + transport-layer data

	p = xiah.encap(p, false);

	delete thdr;

	// Set timer
	daginfo->timer_on = true;
	daginfo->synack_waiting = true;
	daginfo->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

	if (! _timer.scheduled() || _timer.expiry() >= daginfo->expiry )
		_timer.reschedule_at(daginfo->expiry);

	// Store the syn packet for potential retransmission
	daginfo->syn_pkt = copy_packet(p, daginfo);

	portToDAGinfo.set(_sport, *daginfo);
	XIAHeader xiah1(p);
	//String pld((char *)xiah1.payload(), xiah1.plen());
	// click_chatter("XCONNECT: %d: %s\n", _sport, (_local_addr.unparse()).c_str());
	output(NETWORK_PORT).push(p);

	//daginfo=portToDAGinfo.get_pointer(_sport);
	//click_chatter("\nbound to %s\n",portToDAGinfo.get_pointer(_sport)->src_path.unparse().c_str());

	// (for Ack purpose) Reply with a packet with the destination port=source port
	//output(API_PORT).push(UDPIPPrep(p_in,_sport));
}

void XTRANSPORT::Xaccept(unsigned short _sport)
{
	//click_chatter("Xaccept: on %d\n", _sport);
	hlim.set(_sport, HLIM_DEFAULT);
	nxt_xport.set(_sport, CLICK_XIA_NXT_TRN);

	if (!pending_connection_buf.empty()) {

		DAGinfo daginfo = pending_connection_buf.front();
		daginfo.port = _sport;

		daginfo.seq_num = 0;
		daginfo.ack_num = 0;
		daginfo.base = 0;
		daginfo.hlim = hlim.get(_sport);
		daginfo.next_seqnum = 0;
		daginfo.expected_seqnum = 0;
		daginfo.isAcceptSocket = true;
		memset(daginfo.sent_pkt, 0, MAX_WIN_SIZE * sizeof(WritablePacket*));

		portToDAGinfo.set(_sport, daginfo);

		XID source_xid = daginfo.src_path.xid(daginfo.src_path.destination_node());
		XID destination_xid = daginfo.dst_path.xid(daginfo.dst_path.destination_node());

		XIDpair xid_pair;
		xid_pair.set_src(source_xid);
		xid_pair.set_dst(destination_xid);

		// Map the src & dst XID pair to source port
		XIDpairToPort.set(xid_pair, _sport);

		portToActive.set(_sport, true);

		// click_chatter("XACCEPT: (%s) my_sport=%d  my_sid=%s  his_sid=%s \n\n", (_local_addr.unparse()).c_str(), _sport, source_xid.unparse().c_str(), destination_xid.unparse().c_str());

		pending_connection_buf.pop();


 		xia::XSocketMsg xsm;
 		xsm.set_type(xia::XACCEPT);

		xia::X_Accept_Msg *msg = xsm.mutable_x_accept();
		msg->set_dag(daginfo.dst_path.unparse().c_str());

 		std::string s;
 		xsm.SerializeToString(&s);
		WritablePacket *reply = WritablePacket::make(256, s.c_str(), s.size(), 0);
		output(API_PORT).push(UDPIPPrep(reply, _sport));
		
//		ReturnResult(_sport, xia::XACCEPT);

	} else {
		// FIXME: what error code should be returned?
		ReturnResult(_sport, xia::XACCEPT, -1, ECONNABORTED);
		click_chatter("\n Xaccept: error\n");
	}
}

void XTRANSPORT::Xchangead(unsigned short _sport)
{
	UNUSED(_sport);

	xia::X_Changead_Msg *x_changead_msg = xia_socket_msg.mutable_x_changead();
	//String tmp = _local_addr.unparse();
	//Vector<String> ids;
	//cp_spacevec(tmp, ids);
	String AD_str(x_changead_msg->ad().c_str());
	String HID_str = _local_hid.unparse();
	String IP4ID_str(x_changead_msg->ip4id().c_str());
	_local_4id.parse(IP4ID_str);
	String new_local_addr;
	// If a valid 4ID is given, it is included (as a fallback) in the local_addr
	if(_local_4id != _null_4id) {		
		new_local_addr = "RE ( " + IP4ID_str + " ) " + AD_str + " " + HID_str;
	} else {
		new_local_addr = "RE " + AD_str + " " + HID_str;
	}
	click_chatter("new address is - %s", new_local_addr.c_str());
	_local_addr.parse(new_local_addr);		
}

void XTRANSPORT::Xreadlocalhostaddr(unsigned short _sport)
{
	// read the localhost AD and HID
	String local_addr = _local_addr.unparse();
	size_t AD_found_start = local_addr.find_left("AD:");
	size_t AD_found_end = local_addr.find_left(" ", AD_found_start);
	String AD_str = local_addr.substring(AD_found_start, AD_found_end - AD_found_start);
	String HID_str = _local_hid.unparse();
	String IP4ID_str = _local_4id.unparse();
	// return a packet containing localhost AD and HID
	xia::XSocketMsg _Response;
	_Response.set_type(xia::XREADLOCALHOSTADDR);
	xia::X_ReadLocalHostAddr_Msg *_msg = _Response.mutable_x_readlocalhostaddr();
	_msg->set_ad(AD_str.c_str());
	_msg->set_hid(HID_str.c_str());
	_msg->set_ip4id(IP4ID_str.c_str());
	std::string p_buf1;
	_Response.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

void XTRANSPORT::Xupdatenameserverdag(unsigned short _sport)
{
	UNUSED(_sport);

	xia::X_Updatenameserverdag_Msg *x_updatenameserverdag_msg = xia_socket_msg.mutable_x_updatenameserverdag();
	String ns_dag(x_updatenameserverdag_msg->dag().c_str());
	//click_chatter("new nameserver address is - %s", ns_dag.c_str());
	_nameserver_addr.parse(ns_dag);
}

void XTRANSPORT::Xreadnameserverdag(unsigned short _sport)
{
	// read the nameserver DAG
	String ns_addr = _nameserver_addr.unparse();
	// return a packet containing the nameserver DAG
	xia::XSocketMsg _Response;
	_Response.set_type(xia::XREADNAMESERVERDAG);
	xia::X_ReadNameServerDag_Msg *_msg = _Response.mutable_x_readnameserverdag();
	_msg->set_dag(ns_addr.c_str());
	std::string p_buf1;
	_Response.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

void XTRANSPORT::Xisdualstackrouter(unsigned short _sport)
{
	// return a packet indicating whether this node is an XIA-IPv4 dual-stack router
	xia::XSocketMsg _Response;
	_Response.set_type(xia::XISDUALSTACKROUTER);
	xia::X_IsDualStackRouter_Msg *_msg = _Response.mutable_x_isdualstackrouter();
	_msg->set_flag(_is_dual_stack_router);
	std::string p_buf1;
	_Response.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

void XTRANSPORT::Xgetpeername(unsigned short _sport)
{
	xia::XSocketMsg _xsm;
	_xsm.set_type(xia::XGETPEERNAME);
	xia::X_GetPeername_Msg *_msg = _xsm.mutable_x_getpeername();

	DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);

	_msg->set_dag(daginfo->dst_path.unparse().c_str());

	std::string p_buf1;
	_xsm.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}
					

void XTRANSPORT::Xgetsockname(unsigned short _sport)
{
	xia::XSocketMsg _xsm;
	_xsm.set_type(xia::XGETSOCKNAME);
	xia::X_GetSockname_Msg *_msg = _xsm.mutable_x_getsockname();

	DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);

	_msg->set_dag(daginfo->src_path.unparse().c_str());

	std::string p_buf1;
	_xsm.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}


void XTRANSPORT::Xsend(unsigned short _sport, WritablePacket *p_in)
{
	//click_chatter("Xsend on %d\n", _sport);

	xia::X_Send_Msg *x_send_msg = xia_socket_msg.mutable_x_send();

	String pktPayload(x_send_msg->payload().c_str(), x_send_msg->payload().size());

	int pktPayloadSize = pktPayload.length();
	//click_chatter("pkt %s port %d", pktPayload.c_str(), _sport);
	//click_chatter("XSEND: %d bytes from (%d)\n", pktPayloadSize, _sport);

	//Find DAG info for that stream
	DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);
	if (daginfo && daginfo->isConnected) {

		//Recalculate source path
		XID	source_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
		String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse();
		//Make source DAG _local_addr:SID
		String dagstr = daginfo->src_path.unparse_re();

		//Client Mobility...
		if (dagstr.length() != 0 && dagstr != str_local_addr) {
			//Moved!
			// 1. Update 'daginfo->src_path'
			daginfo->src_path.parse_re(str_local_addr);
		}
	
		// Case of initial binding to only SID
		if(daginfo->full_src_dag == false) {
			daginfo->full_src_dag = true;
			String str_local_addr = _local_addr.unparse_re();
			XID front_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
			String xid_string = front_xid.unparse();
			str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
			daginfo->src_path.parse_re(str_local_addr);
		}

		//Add XIA headers
		XIAHeaderEncap xiah;
		xiah.set_nxt(CLICK_XIA_NXT_TRN);
		xiah.set_last(LAST_NODE_DEFAULT);
		xiah.set_hlim(hlim.get(_sport));
		xiah.set_dst_path(daginfo->dst_path);
		xiah.set_src_path(daginfo->src_path);
		xiah.set_plen(pktPayloadSize);


		_errh->debug("XSEND: (%d) sent packet to %s, from %s\n", _sport, daginfo->dst_path.unparse_re().c_str(), daginfo->src_path.unparse_re().c_str());

		WritablePacket *just_payload_part = WritablePacket::make(p_in->headroom() + 1, (const void*)x_send_msg->payload().c_str(), pktPayloadSize, p_in->tailroom());

		WritablePacket *p = NULL;

		//Add XIA Transport headers
		TransportHeaderEncap *thdr = TransportHeaderEncap::MakeDATAHeader(daginfo->next_seqnum, daginfo->ack_num, 0 ); // #seq, #ack, length
		p = thdr->encap(just_payload_part);

		thdr->update();
		xiah.set_plen(pktPayloadSize + thdr->hlen()); // XIA payload = transport header + transport-layer data

		p = xiah.encap(p, false);

		delete thdr;

		// Store the packet into buffer
		WritablePacket *tmp = daginfo->sent_pkt[daginfo->seq_num % MAX_WIN_SIZE];
		daginfo->sent_pkt[daginfo->seq_num % MAX_WIN_SIZE] = copy_packet(p, daginfo);
		if (tmp)
			tmp->kill();

		// click_chatter("XSEND: SENT DATA at (%s) seq=%d \n\n", dagstr.c_str(), daginfo->seq_num%MAX_WIN_SIZE);

		daginfo->seq_num++;
		daginfo->next_seqnum++;

		// Set timer
		daginfo->timer_on = true;
		daginfo->dataack_waiting = true;
		daginfo->num_retransmit_tries = 0;
		daginfo->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

		if (! _timer.scheduled() || _timer.expiry() >= daginfo->expiry )
			_timer.reschedule_at(daginfo->expiry);

		portToDAGinfo.set(_sport, *daginfo);

		//click_chatter("Sent packet to network");
		XIAHeader xiah1(p);
		String pld((char *)xiah1.payload(), xiah1.plen());
		//click_chatter("\n\n (%s) send (timer set at %f) =%s  len=%d \n\n", (_local_addr.unparse()).c_str(), (daginfo->expiry).doubleval(), pld.c_str(), xiah1.plen());
		output(NETWORK_PORT).push(p);

		// REMOVED STATUS RETURNS AS WE RAN INTO SEQUENCING ERRORS
		// WHERE IT INTERLEAVED WITH RECEIVE PACKETS
		// (for Ack purpose) Reply with a packet with the destination port=source port
//				ReturnResult(_sport, xia::XSEND);

	} else {
	
//				ReturnResult(_sport, xia::XSEND, -1, ENOTCONN);
		click_chatter("Not 'connect'ed: you may need to use 'sendto()'");
	}
}

void XTRANSPORT::Xsendto(unsigned short _sport, WritablePacket *p_in)
{
	xia::X_Sendto_Msg *x_sendto_msg = xia_socket_msg.mutable_x_sendto();

	String dest(x_sendto_msg->ddag().c_str());
	String pktPayload(x_sendto_msg->payload().c_str(), x_sendto_msg->payload().size());
	int pktPayloadSize = pktPayload.length();
	//click_chatter("\n SENDTO ddag:%s, payload:%s, length=%d\n",xia_socket_msg.ddag().c_str(), xia_socket_msg.payload().c_str(), pktPayloadSize);

	XIAPath dst_path;
	dst_path.parse(dest);

	//Find DAG info for this DGRAM
	DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);

	if(!daginfo) {
		//No local SID bound yet, so bind one
		daginfo = new DAGinfo();
	}

	if (daginfo->initialized == false) {
		daginfo->initialized = true;
		daginfo->full_src_dag = true;
		daginfo->port = _sport;
		String str_local_addr = _local_addr.unparse_re();

		char xid_string[50];
		random_xid("SID", xid_string);
//		String rand(click_random(1000000, 9999999));
//		String xid_string = "SID:20000ff00000000000000000000000000" + rand;
		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID

		daginfo->src_path.parse_re(str_local_addr);

		daginfo->last = LAST_NODE_DEFAULT;
		daginfo->hlim = hlim.get(_sport);

		XID	source_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());

		XIDtoPort.set(source_xid, _sport); //Maybe change the mapping to XID->DAGinfo?
		addRoute(source_xid);
	}

	// Case of initial binding to only SID
	if(daginfo->full_src_dag == false) {
		daginfo->full_src_dag = true;
		String str_local_addr = _local_addr.unparse_re();
		XID front_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
		String xid_string = front_xid.unparse();
		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
		daginfo->src_path.parse_re(str_local_addr);
	}


	if(daginfo->src_path.unparse_re().length() != 0) {
		//Recalculate source path
		XID	source_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
		String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse(); //Make source DAG _local_addr:SID
		daginfo->src_path.parse(str_local_addr);
	}

	portToDAGinfo.set(_sport, *daginfo);

	daginfo = portToDAGinfo.get_pointer(_sport);

//	_errh->debug("sent packet from %s, to %s\n", daginfo->src_path.unparse_re().c_str(), dest.c_str());

	//Add XIA headers
	XIAHeaderEncap xiah;

	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim.get(_sport));
	xiah.set_dst_path(dst_path);
	xiah.set_src_path(daginfo->src_path);

	WritablePacket *just_payload_part = WritablePacket::make(p_in->headroom() + 1, (const void*)x_sendto_msg->payload().c_str(), pktPayloadSize, p_in->tailroom());

	WritablePacket *p = NULL;

	// FIXME: shouldn't be a raw number
	if (daginfo->sock_type == 3) {
		xiah.set_nxt(nxt_xport.get(_sport));

		xiah.set_plen(pktPayloadSize);
		p = xiah.encap(just_payload_part, false);

	} else {
		xiah.set_nxt(CLICK_XIA_NXT_TRN);
		xiah.set_plen(pktPayloadSize);

		//p = xiah.encap(just_payload_part, true);
		//click_chatter("\n\nSEND: %s ---> %s\n\n", daginfo->src_path.unparse_re().c_str(), dest.c_str());
		//click_chatter("payload=%s len=%d \n\n", x_sendto_msg->payload().c_str(), pktPayloadSize);

		//Add XIA Transport headers
		TransportHeaderEncap *thdr = TransportHeaderEncap::MakeDGRAMHeader(0); // length
		p = thdr->encap(just_payload_part);

		thdr->update();
		xiah.set_plen(pktPayloadSize + thdr->hlen()); // XIA payload = transport header + transport-layer data

		p = xiah.encap(p, false);
		delete thdr;
	}

	output(NETWORK_PORT).push(p);

	// removed due to multi peer collision problem
	// (for Ack purpose) Reply with a packet with the destination port=source port
//				ReturnResult(_sport, xia::XSENDTO);
}

void XTRANSPORT::XrequestChunk(unsigned short _sport, WritablePacket *p_in)
{
	xia::X_Requestchunk_Msg *x_requestchunk_msg = xia_socket_msg.mutable_x_requestchunk();

	String pktPayload(x_requestchunk_msg->payload().c_str(), x_requestchunk_msg->payload().size());
	int pktPayloadSize = pktPayload.length();

	// send CID-Requests

	for (int i = 0; i < x_requestchunk_msg->dag_size(); i++) {
		String dest = x_requestchunk_msg->dag(i).c_str();
		//click_chatter("CID-Request for %s  (size=%d) \n", dest.c_str(), dag_size);
		//click_chatter("\n\n (%s) hi 3 \n\n", (_local_addr.unparse()).c_str());
		XIAPath dst_path;
		dst_path.parse(dest);

		//Find DAG info for this DGRAM
		DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);

		if(!daginfo) {
			//No local SID bound yet, so bind one
			daginfo = new DAGinfo();
		}

		if (daginfo->initialized == false) {
			daginfo->initialized = true;
			daginfo->full_src_dag = true;
			daginfo->port = _sport;
			String str_local_addr = _local_addr.unparse_re();

			char xid_string[50];
			random_xid("SID", xid_string);
//			String rand(click_random(1000000, 9999999));
//			String xid_string = "SID:20000ff00000000000000000000000000" + rand;
			str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID

			daginfo->src_path.parse_re(str_local_addr);

			daginfo->last = LAST_NODE_DEFAULT;
			daginfo->hlim = hlim.get(_sport);

			XID	source_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());

			XIDtoPort.set(source_xid, _sport); //Maybe change the mapping to XID->DAGinfo?
			addRoute(source_xid);

		}
	
		// Case of initial binding to only SID
		if(daginfo->full_src_dag == false) {
			daginfo->full_src_dag = true;
			String str_local_addr = _local_addr.unparse_re();
			XID front_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
			String xid_string = front_xid.unparse();
			str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
			daginfo->src_path.parse_re(str_local_addr);
		}
	
		if(daginfo->src_path.unparse_re().length() != 0) {
			//Recalculate source path
			XID	source_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
			String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse(); //Make source DAG _local_addr:SID
			daginfo->src_path.parse(str_local_addr);
		}

		portToDAGinfo.set(_sport, *daginfo);

		daginfo = portToDAGinfo.get_pointer(_sport);

		_errh->debug("sent packet to %s, from %s\n", dest.c_str(), daginfo->src_path.unparse_re().c_str());

		//Add XIA headers
		XIAHeaderEncap xiah;
		xiah.set_nxt(CLICK_XIA_NXT_CID);
		xiah.set_last(LAST_NODE_DEFAULT);
		xiah.set_hlim(hlim.get(_sport));
		xiah.set_dst_path(dst_path);
		xiah.set_src_path(daginfo->src_path);
		xiah.set_plen(pktPayloadSize);

		WritablePacket *just_payload_part = WritablePacket::make(p_in->headroom() + 1, (const void*)x_requestchunk_msg->payload().c_str(), pktPayloadSize, p_in->tailroom());

		WritablePacket *p = NULL;

		//Add Content header
		ContentHeaderEncap *chdr = ContentHeaderEncap::MakeRequestHeader();
		p = chdr->encap(just_payload_part);
		p = xiah.encap(p, true);
		delete chdr;

		XID	source_sid = daginfo->src_path.xid(daginfo->src_path.destination_node());
		XID	destination_cid = dst_path.xid(dst_path.destination_node());

		XIDpair xid_pair;
		xid_pair.set_src(source_sid);
		xid_pair.set_dst(destination_cid);

		// Map the src & dst XID pair to source port
		XIDpairToPort.set(xid_pair, _sport);

		// Store the packet into buffer
		WritablePacket *copy_req_pkt = copy_cid_req_packet(p, daginfo);
		daginfo->XIDtoCIDreqPkt.set(destination_cid, copy_req_pkt);

		// Set the status of CID request
		daginfo->XIDtoStatus.set(destination_cid, WAITING_FOR_CHUNK);

		// Set the status of ReadCID reqeust
		daginfo->XIDtoReadReq.set(destination_cid, false);

		// Set timer
		Timestamp cid_req_expiry  = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);
		daginfo->XIDtoExpiryTime.set(destination_cid, cid_req_expiry);
		daginfo->XIDtoTimerOn.set(destination_cid, true);

		if (! _timer.scheduled() || _timer.expiry() >= cid_req_expiry )
			_timer.reschedule_at(cid_req_expiry);

		portToDAGinfo.set(_sport, *daginfo);

		output(NETWORK_PORT).push(p);
	}
}

void XTRANSPORT::XgetChunkStatus(unsigned short _sport)
{
	xia::X_Getchunkstatus_Msg *x_getchunkstatus_msg = xia_socket_msg.mutable_x_getchunkstatus();

	int numCids = x_getchunkstatus_msg->dag_size();
	String pktPayload(x_getchunkstatus_msg->payload().c_str(), x_getchunkstatus_msg->payload().size());

	// send CID-Requests
	for (int i = 0; i < numCids; i++) {
		String dest = x_getchunkstatus_msg->dag(i).c_str();
		//click_chatter("CID-Request for %s  (size=%d) \n", dest.c_str(), dag_size);
		//click_chatter("\n\n (%s) hi 3 \n\n", (_local_addr.unparse()).c_str());
		XIAPath dst_path;
		dst_path.parse(dest);

		//Find DAG info for this DGRAM
		DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);

		XID	destination_cid = dst_path.xid(dst_path.destination_node());

		// Check the status of CID request
		HashTable<XID, int>::iterator it;
		it = daginfo->XIDtoStatus.find(destination_cid);

		if(it != daginfo->XIDtoStatus.end()) {
			// There is an entry
			int status = it->second;

			if(status == WAITING_FOR_CHUNK) {
				x_getchunkstatus_msg->add_status("WAITING");

			} else if(status == READY_TO_READ) {
				x_getchunkstatus_msg->add_status("READY");

			} else if(status == INVALID_HASH) {
				x_getchunkstatus_msg->add_status("INVALID_HASH");

			} else if(status == REQUEST_FAILED) {
				x_getchunkstatus_msg->add_status("FAILED");
			}

		} else {
			// Status query for the CID that was not requested...
			x_getchunkstatus_msg->add_status("FAILED");
		}
	}

	// Send back the report

	const char *buf = "CID request status response";
	x_getchunkstatus_msg->set_payload((const char*)buf, strlen(buf) + 1);

	std::string p_buf;
	xia_socket_msg.SerializeToString(&p_buf);

	WritablePacket *reply = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

void XTRANSPORT::XreadChunk(unsigned short _sport)
{
	
	click_chatter(">>READ chunk message from API %d\n", _sport);
	
	
	xia::X_Readchunk_Msg *x_readchunk_msg = xia_socket_msg.mutable_x_readchunk();

	String dest = x_readchunk_msg->dag().c_str();
	WritablePacket *copy;
	//click_chatter("CID-Request for %s  (size=%d) \n", dest.c_str(), dag_size);
	//click_chatter("\n\n (%s) hi 3 \n\n", (_local_addr.unparse()).c_str());
	XIAPath dst_path;
	dst_path.parse(dest);

	//Find DAG info for this DGRAM
	DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);

	XID	destination_cid = dst_path.xid(dst_path.destination_node());

	// Update the status of ReadCID reqeust
	daginfo->XIDtoReadReq.set(destination_cid, true);
	portToDAGinfo.set(_sport, *daginfo);

	// Check the status of CID request
	HashTable<XID, int>::iterator it;
	it = daginfo->XIDtoStatus.find(destination_cid);

	if(it != daginfo->XIDtoStatus.end()) {
		// There is an entry
		int status = it->second;

		if (status != READY_TO_READ  &&
			status != INVALID_HASH) {
			// Do nothing

		} else {
			// Send the buffered pkt to upper layer

			daginfo->XIDtoReadReq.set(destination_cid, false);
			portToDAGinfo.set(_sport, *daginfo);

			HashTable<XID, WritablePacket*>::iterator it2;
			it2 = daginfo->XIDtoCIDresponsePkt.find(destination_cid);
			copy = copy_cid_response_packet(it2->second, daginfo);

			XIAHeader xiah(copy->xia_header());

			//Unparse dag info
			String src_path = xiah.src_path().unparse();

			xia::XSocketMsg xia_socket_msg;
			xia_socket_msg.set_type(xia::XREADCHUNK);
			xia::X_Readchunk_Msg *x_readchunk_msg = xia_socket_msg.mutable_x_readchunk();
			x_readchunk_msg->set_dag(src_path.c_str());
			x_readchunk_msg->set_payload((const char *)xiah.payload(), xiah.plen());

			std::string p_buf;
			xia_socket_msg.SerializeToString(&p_buf);

			WritablePacket *p2 = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);

			//click_chatter("FROM CACHE. data length = %d  \n", str.length());
			_errh->debug("Sent packet to socket: sport %d dport %d", _sport, _sport);
			
			//TODO: remove
			click_chatter(">>send chunk to API after read %d\n", _sport);
			output(API_PORT).push(UDPIPPrep(p2, _sport));

			it2->second->kill();
			daginfo->XIDtoCIDresponsePkt.erase(it2);

			portToDAGinfo.set(_sport, *daginfo);
		}
	}

}

void XTRANSPORT::XremoveChunk(unsigned short _sport)
{
	xia::X_Removechunk_Msg *x_rmchunk_msg = xia_socket_msg.mutable_x_removechunk();

	int32_t contextID = x_rmchunk_msg->contextid();
	String src(x_rmchunk_msg->cid().c_str(), x_rmchunk_msg->cid().size());
	//append local address before CID
	String str_local_addr = _local_addr.unparse_re();
	str_local_addr = "RE " + str_local_addr + " CID:" + src;
	XIAPath src_path;
	src_path.parse(str_local_addr);

	//Add XIA headers
	XIAHeaderEncap xiah;
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(HLIM_DEFAULT);
	xiah.set_dst_path(_local_addr);
	xiah.set_src_path(src_path);
	xiah.set_nxt(CLICK_XIA_NXT_CID);

	WritablePacket *just_payload_part = WritablePacket::make(256, (const void*)NULL, 0, 0);

	WritablePacket *p = NULL;
	ContentHeaderEncap  contenth(0, 0, 0, 0, ContentHeader::OP_LOCAL_REMOVECID, contextID);
	p = contenth.encap(just_payload_part);
	p = xiah.encap(p, true);

	_errh->debug("sent remove cid packet to cache");
	output(CACHE_PORT).push(p);

	// (for Ack purpose) Reply with a packet with the destination port=source port
	xia::XSocketMsg _socketResponse;
	_socketResponse.set_type(xia::XREMOVECHUNK);
	xia::X_Removechunk_Msg *_msg = _socketResponse.mutable_x_removechunk();
	_msg->set_contextid(contextID);
	_msg->set_cid(src.c_str());
	_msg->set_status(0);

	std::string p_buf1;
	_socketResponse.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}

void XTRANSPORT::XputChunk(unsigned short _sport)
{
	
	
	click_chatter(">>putchunk message from API %d\n", _sport);
	
	
	
	xia::X_Putchunk_Msg *x_putchunk_msg = xia_socket_msg.mutable_x_putchunk();
//			int hasCID = x_putchunk_msg->hascid();
	int32_t contextID = x_putchunk_msg->contextid();
	int32_t ttl = x_putchunk_msg->ttl();
	int32_t cacheSize = x_putchunk_msg->cachesize();
	int32_t cachePolicy = x_putchunk_msg->cachepolicy();

	String pktPayload(x_putchunk_msg->payload().c_str(), x_putchunk_msg->payload().size());
	String src;

	/* Computes SHA1 Hash if user does not supply it */
	char hexBuf[3];
	int i = 0;
	SHA1_ctx sha_ctx;
	unsigned char digest[HASH_KEYSIZE];
	SHA1_init(&sha_ctx);
	SHA1_update(&sha_ctx, (unsigned char *)pktPayload.c_str() , pktPayload.length() );
	SHA1_final(digest, &sha_ctx);
	for(i = 0; i < HASH_KEYSIZE; i++) {
		sprintf(hexBuf, "%02x", digest[i]);
		src.append(const_cast<char *>(hexBuf), 2);
	}

	_errh->debug("ctxID=%d, length=%d, ttl=%d cid=%s\n", contextID, x_putchunk_msg->payload().size(), ttl, src.c_str());

	//append local address before CID
	String str_local_addr = _local_addr.unparse_re();
	str_local_addr = "RE " + str_local_addr + " CID:" + src;
	XIAPath src_path;
	src_path.parse(str_local_addr);

	_errh->debug("DAG: %s\n", str_local_addr.c_str());

	/*TODO: The destination dag of the incoming packet is local_addr:XID
	 * Thus the cache thinks it is destined for local_addr and delivers to socket
	 * This must be ignored. Options
	 * 1. Use an invalid SID
	 * 2. The cache should only store the CID responses and not forward them to
	 *	local_addr when the source and the destination HIDs are the same.
	 * 3. Use the socket SID on which putCID was issued. This will
	 *	result in a reply going to the same socket on which the putCID was issued.
	 *	Use the response to return 1 to the putCID call to indicate success.
	 *	Need to add daginfo/ephemeral SID generation for this to work.
	 * 4. Special OPCODE in content extension header and treat it specially in content module (done below)
	 */

	//Add XIA headers
	XIAHeaderEncap xiah;
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim.get(_sport));
	xiah.set_dst_path(_local_addr);
	xiah.set_src_path(src_path);
	xiah.set_nxt(CLICK_XIA_NXT_CID);

	//Might need to remove more if another header is required (eg some control/DAG info)

	WritablePacket *just_payload_part = WritablePacket::make(256, (const void*)pktPayload.c_str(), pktPayload.length(), 0);

	WritablePacket *p = NULL;
	int chunkSize = pktPayload.length();
	ContentHeaderEncap  contenth(0, 0, pktPayload.length(), chunkSize, ContentHeader::OP_LOCAL_PUTCID,
								 contextID, ttl, cacheSize, cachePolicy);
	p = contenth.encap(just_payload_part);
	p = xiah.encap(p, true);

	_errh->debug("sent packet to cache");
	
	output(CACHE_PORT).push(p);

	// (for Ack purpose) Reply with a packet with the destination port=source port
	struct timeval timestamp;
	gettimeofday(&timestamp, NULL);
	xia::XSocketMsg _socketResponse;
	_socketResponse.set_type(xia::XPUTCHUNK);
	xia::X_Putchunk_Msg *_msg = _socketResponse.mutable_x_putchunk();
	_msg->set_contextid(contextID);
	_msg->set_cid(src.c_str());
	_msg->set_ttl(ttl);
	_msg->set_timestamp(timestamp.tv_sec);
//	_msg->set_hascid(1);
	_msg->set_cachepolicy(0);
	_msg->set_cachesize(0);
	_msg->set_payload(x_putchunk_msg->payload().c_str(), x_putchunk_msg->payload().size());

	std::string p_buf1;
	_socketResponse.SerializeToString(&p_buf1);
	WritablePacket *reply = WritablePacket::make(256, p_buf1.c_str(), p_buf1.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, _sport));
}




void XTRANSPORT::XpushChunkto(unsigned short _sport, WritablePacket *p_in)
{
	xia::X_Pushchunkto_Msg *x_pushchunkto_msg= xia_socket_msg.mutable_x_pushchunkto();

	
	int32_t contextID = x_pushchunkto_msg->contextid();
	int32_t ttl = x_pushchunkto_msg->ttl();
	int32_t cacheSize = x_pushchunkto_msg->cachesize();
	int32_t cachePolicy = x_pushchunkto_msg->cachepolicy();
	
	String pktPayload(x_pushchunkto_msg->payload().c_str(), x_pushchunkto_msg->payload().size());
	int pktPayloadSize = pktPayload.length();
	

	// send CID-Requests

	String dest = x_pushchunkto_msg->ddag().c_str();
	//click_chatter("CID-Request for %s  (size=%d) \n", dest.c_str(), dag_size);
	//click_chatter("\n\n (%s) hi 3 \n\n", (_local_addr.unparse()).c_str());
	XIAPath dst_path;
	dst_path.parse(dest);

	//Find DAG info for this DGRAM
	DAGinfo *daginfo = portToDAGinfo.get_pointer(_sport);

	if(!daginfo) {
		//No local SID bound yet, so bind one
		daginfo = new DAGinfo();
	}

	if (daginfo->initialized == false) {
		daginfo->initialized = true;
		daginfo->full_src_dag = true;
		daginfo->port = _sport;
		String str_local_addr = _local_addr.unparse_re();
		

		
		//TODO: AD->HID->SID->CID We can add SID here (AD->HID->SID->CID) if SID is passed or generate randomly
		// Contentmodule forwarding needs to be fixed to get this to work. (wrong comparison there)
		// Since there is no way to dictate policy to cache which content to accept right now this wasn't added.
// 		char xid_string[50];
// 		random_xid("SID", xid_string);
//		String rand(click_random(1000000, 9999999));
//		String xid_string = "SID:20000ff00000000000000000000000000" + rand;
// 		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID

		daginfo->src_path.parse_re(str_local_addr);

		daginfo->last = LAST_NODE_DEFAULT;
		daginfo->hlim = hlim.get(_sport);

		XID source_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());

		XIDtoPort.set(source_xid, _sport); //Maybe change the mapping to XID->DAGinfo?
		addRoute(source_xid);

	}

	// Case of initial binding to only SID
	if(daginfo->full_src_dag == false) {
		daginfo->full_src_dag = true;
		String str_local_addr = _local_addr.unparse_re();
		XID front_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
		String xid_string = front_xid.unparse();
		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
		click_chatter("str_local_addr: %s", str_local_addr.c_str() );
		daginfo->src_path.parse_re(str_local_addr);
	}

	if(daginfo->src_path.unparse_re().length() != 0) {
		//Recalculate source path
		XID	source_xid = daginfo->src_path.xid(daginfo->src_path.destination_node());
		String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse(); //Make source DAG _local_addr:SID
		click_chatter("str_local_addr: %s", str_local_addr.c_str() );
		daginfo->src_path.parse(str_local_addr);
	}

	portToDAGinfo.set(_sport, *daginfo);

	daginfo = portToDAGinfo.get_pointer(_sport);

	_errh->debug("sent packet to %s, from %s\n", dest.c_str(), daginfo->src_path.unparse_re().c_str());

	click_chatter("PUSHCID: %s",x_pushchunkto_msg->cid().c_str());
	String src(x_pushchunkto_msg->cid().c_str(), x_pushchunkto_msg->cid().size());
	//append local address before CID
	String cid_str_local_addr = daginfo->src_path.unparse_re();
	cid_str_local_addr = "RE " + cid_str_local_addr + " CID:" + src;
	XIAPath cid_src_path;
	cid_src_path.parse(cid_str_local_addr);
	click_chatter("cid_local_addr: %s", cid_str_local_addr.c_str() );
	
	
	//Add XIA headers	
	XIAHeaderEncap xiah;
	xiah.set_nxt(CLICK_XIA_NXT_CID);
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim.get(_sport));
	xiah.set_dst_path(dst_path);
	xiah.set_src_path(cid_src_path); //FIXME: is this the correct way to do it? Do we need SID? AD->HID->SID->CID 
	xiah.set_plen(pktPayloadSize);

// 	WritablePacket *just_payload_part = WritablePacket::make(p_in->headroom() + 1, (const void*)x_pushchunkto_msg->payload().c_str(), pktPayloadSize, p_in->tailroom());
	WritablePacket *just_payload_part = WritablePacket::make(256, (const void*)x_pushchunkto_msg->payload().c_str(), pktPayloadSize, 0);

	WritablePacket *p = NULL;

	//Add Content header
// 	ContentHeaderEncap *chdr = ContentHeaderEncap::MakePushHeader();
	int chunkSize = pktPayload.length();
	ContentHeaderEncap  contenth(0, 0, pktPayload.length(), chunkSize, ContentHeader::OP_PUSH,
								 contextID, ttl, cacheSize, cachePolicy);
	
	
	
	p = contenth.encap(just_payload_part);
	p = xiah.encap(p, true);
	

// 	XID	source_sid = daginfo->src_path.xid(daginfo->src_path.destination_node());
// 	XID	destination_cid = dst_path.xid(dst_path.destination_node());
	//FIXME this is wrong
	XID	source_cid = cid_src_path.xid(cid_src_path.destination_node());
// 	XID	source_cid = daginfo->src_path.xid(cid_src_path.destination_node());
	XID	destination_sid = dst_path.xid(dst_path.destination_node());

	XIDpair xid_pair;
	xid_pair.set_src(source_cid);
	xid_pair.set_dst(destination_sid);

	// Map the src & dst XID pair to source port
	XIDpairToPort.set(xid_pair, _sport);


	portToDAGinfo.set(_sport, *daginfo);

	output(NETWORK_PORT).push(p);
}


CLICK_ENDDECLS

EXPORT_ELEMENT(XTRANSPORT)
ELEMENT_REQUIRES(userlevel)
ELEMENT_REQUIRES(XIAContentModule)
ELEMENT_MT_SAFE(XTRANSPORT)
