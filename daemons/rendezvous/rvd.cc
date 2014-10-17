#include <sys/select.h>
#include <syslog.h>
#include <errno.h>
#include <map>
#include <string>
using namespace std;

#include <stdint.h>
#include "clicknetxia.h"
#include "Xsocket.h"
#include "dagaddr.hpp"
#include "Xkeys.h"

#define DEFAULT_NAME "host0"
#define APPNAME "xrendezvous"

#define RV_MAX_DATA_PACKET_SIZE 16384
#define RV_MAX_CONTROL_PACKET_SIZE 1024

map<std::string, std::string> name_to_dag_db_table; // map name to dag

char *hostname = NULL;
char *ident = NULL;
char *datasid = NULL;
char *controlsid = NULL;
map<string, string> HIDtoAD;

void help(const char *name)
{
	printf("\nusage: %s [-l level] [-v] [-d SID -c SID][-h hostname]\n", name);
	printf("where:\n");
	printf(" -c SID      : SID for rendezvous control plane.\n");
	printf(" -d SID      : SID for rendezvous data plane.\n");
	printf(" -l level    : syslog logging level 0 = LOG_EMERG ... 7 = LOG_DEBUG (default=3:LOG_ERR)\n");
	printf(" -v          : log to the console as well as syslog\n");
	printf(" -h hostname : click device name (default=host0)\n");
	printf("\n");
	exit(0);
}

// Allocates memory and creates a new string containing SID in that space
// Caller must free the returned pointer if not NULL
char *createRandomSIDifNeeded(char *sidptr)
{
	char *sid;
	// In future we can check if keys matching the SID exist
	if(sidptr != NULL) {
		return sidptr;
	}
	syslog(LOG_INFO, "No SID provided. Creating a new one");
	int sidlen = strlen("SID:") + XIA_SHA_DIGEST_STR_LEN;
	sid = (char *)calloc(sidlen, 1);
	if(!sid) {
		syslog(LOG_ERR, "ERROR allocating memory for SID\n");
		return NULL;
	}
	if(XmakeNewSID(sid, sidlen)) {
		syslog(LOG_ERR, "ERROR autogenerating SID\n");
		return NULL;
	}
	syslog(LOG_INFO, "New service ID:%s: created", sid);
	return sid;
}

#define MAX_RV_DAG_SIZE 2048

void config(int argc, char** argv)
{
	int c;
	unsigned level = 3;
	int verbose = 0;

	opterr = 0;

	while ((c = getopt(argc, argv, "h:l:d:c:v")) != -1) {
		switch (c) {
			case 'h':
				hostname = strdup(optarg);
				break;
			case 'l':
				level = MIN(atoi(optarg), LOG_DEBUG);
				break;
			case 'd':
				datasid = strdup(optarg);
				break;
			case 'c':
				controlsid = strdup(optarg);
				break;
			case 'v':
				verbose = LOG_PERROR;
				break;
			case '?':
			default:
				// Help Me!
				help(basename(argv[0]));
				break;
		}
	}

	if (!hostname)
		hostname = strdup(DEFAULT_NAME);

	// Read Data plane SID from resolv.conf, if needed
	if(datasid == NULL) {
		char data_plane_DAG[MAX_RV_DAG_SIZE];
		if(XreadRVServerAddr(data_plane_DAG, MAX_RV_DAG_SIZE) == 0) {
			char *sid = strstr(data_plane_DAG, "SID:");
			datasid = (char *)calloc(strlen(sid) + 1, 1);
			if(!datasid) {
				syslog(LOG_ERR, "ERROR allocating memory for SID\n");
			} else {
				strcpy(datasid, sid);
				syslog(LOG_INFO, "Data plane: %s", datasid);
			}
		}
	}

	// Read Control plane SID from resolv.conf, if needed
	if(controlsid == NULL) {
		char control_plane_DAG[MAX_RV_DAG_SIZE];
		if(XreadRVServerControlAddr(control_plane_DAG, MAX_RV_DAG_SIZE) == 0) {
			char *sid = strstr(control_plane_DAG, "SID:");
			controlsid = (char *)calloc(strlen(sid) + 1, 1);
			if(!controlsid) {
				syslog(LOG_ERR, "ERROR allocating memory for SID\n");
			} else {
				strcpy(controlsid, sid);
				syslog(LOG_INFO, "Control plane: %s", controlsid);
			}
		}
	}

	// Create random SIDs if not provided in resolv.conf or command line
	datasid = createRandomSIDifNeeded(datasid);
	controlsid = createRandomSIDifNeeded(controlsid);

	// Terminate, if we don't have SIDs for control and data plane
	if(datasid == NULL || controlsid == NULL) {
		syslog(LOG_ERR, "ERROR: Unable to generate new service IDs");
		exit(1);
	}

	// load the config setting for this hostname
	set_conf("xsockconf.ini", hostname);

	// note: ident must exist for the life of the app
	ident = (char *)calloc(strlen (APPNAME) + 4, 1);
	sprintf(ident, "%s", APPNAME);
	openlog(ident, LOG_CONS|LOG_NDELAY|verbose, LOG_LOCAL4);
	setlogmask(LOG_UPTO(level));
}

int getServerSocket(char *sid, int type)
{
	int sockfd = Xsocket(AF_XIA, type, 0);
	if (sockfd < 0) {
   		syslog(LOG_ALERT, "Unable to create a socket for %s", sid);
   		return -1;
	}

	struct addrinfo *ai;
	if (Xgetaddrinfo(NULL, sid, NULL, &ai) != 0) {
   		syslog(LOG_ALERT, "unable to get local address");
		return -2;
	}

	sockaddr_x *sa = (sockaddr_x*)ai->ai_addr;
	Graph g(sa);
	syslog(LOG_INFO, "binding to local DAG: %s", g.dag_string().c_str());

	// Data plane socket binding to the SID
	if (Xbind(sockfd, (struct sockaddr*)sa, sizeof(sockaddr_x)) < 0) {
   		syslog(LOG_ALERT, "unable to bind to local DAG : %s", g.dag_string().c_str());
   		return -3;
	}
	return sockfd;
}

void print_packet_contents(char *packet, int len)
{
	int hex_string_len = (len*2) + 1;
	char hex_string[hex_string_len];
    int i;
    for(i=0;i<len;i++) {
        sprintf(&hex_string[2*i], "%02x", (unsigned int)packet[i]);
    }
    hex_string[hex_string_len-1] = '\0';
	syslog(LOG_INFO, "Packet contents|%s|", hex_string);
}

void process_data(int datasock)
{
	char packet[RV_MAX_DATA_PACKET_SIZE];
	sockaddr_x rdag;
	socklen_t rdaglen = sizeof(rdag);
	bzero(packet, RV_MAX_DATA_PACKET_SIZE);

	syslog(LOG_INFO, "Reading data packet");
	int retval = Xrecvfrom(datasock, packet, RV_MAX_DATA_PACKET_SIZE, 0, (struct sockaddr *)&rdag, &rdaglen);
	if(retval < 0) {
		syslog(LOG_WARNING, "WARN: No data(%s)", strerror(errno));
		return;
	}
	Graph g(&rdag);
	syslog(LOG_INFO, "Packet of size:%d received from %s:", retval, g.dag_string().c_str());
	print_packet_contents(packet, retval);
	click_xia *xiah = reinterpret_cast<struct click_xia *>(packet);
	syslog(LOG_INFO, "======= RAW PACKET HEADER ========");
	syslog(LOG_INFO, "ver:%d", xiah->ver);
	syslog(LOG_INFO, "nxt:%d", xiah->nxt);
	syslog(LOG_INFO, "plen:%d", htons(xiah->plen));
	syslog(LOG_INFO, "hlim:%d", xiah->hlim);
	syslog(LOG_INFO, "dnode:%d", xiah->dnode);
	syslog(LOG_INFO, "snode:%d", xiah->snode);
	syslog(LOG_INFO, "last:%d", xiah->last);
	int total_nodes = xiah->dnode + xiah->snode;
	for(int i=0;i<total_nodes;i++) {
		uint8_t id[20];
		char hex_string[41];
		bzero(hex_string, 41);
		memcpy(id, xiah->node[i].xid.id, 20);
		for(int j=0;j<20;j++) {
			sprintf(&hex_string[2*j], "%02x", (unsigned int)id[j]);
		}
		char type[10];
		bzero(type, 10);
		switch (htonl(xiah->node[i].xid.type)) {
			case CLICK_XIA_XID_TYPE_AD:
				strcpy(type, "AD");
				break;
			case CLICK_XIA_XID_TYPE_HID:
				strcpy(type, "HID");
				break;
			case CLICK_XIA_XID_TYPE_SID:
				strcpy(type, "SID");
				break;
			case CLICK_XIA_XID_TYPE_CID:
				strcpy(type, "CID");
				break;
			case CLICK_XIA_XID_TYPE_IP:
				strcpy(type, "4ID");
				break;
			default:
				sprintf(type, "%d", xiah->node[i].xid.type);
		};
		syslog(LOG_INFO, "%s:%s", type, hex_string);
	}
	// Find the AD->HID->SID this packet was destined to
	// Verify HID is in table and find newAD
	// If newAD is different from the AD in XIP header, update it
	// If not, drop the packet
	// Reset the next pointer in the XIP header
	// Send packet back on the network
}

#define MAX_XID_STR_SIZE 64
#define MAX_HID_DAG_STR_SIZE 256

void process_control_message(int controlsock)
{
	char packet[RV_MAX_CONTROL_PACKET_SIZE];
	sockaddr_x ddag;
	socklen_t ddaglen = sizeof(ddag);
	bzero(packet, RV_MAX_CONTROL_PACKET_SIZE);

	syslog(LOG_INFO, "Reading control packet");
	int retval = Xrecvfrom(controlsock, packet, RV_MAX_CONTROL_PACKET_SIZE, 0, (struct sockaddr *)&ddag, &ddaglen);
	if(retval < 0) {
		syslog(LOG_WARNING, "WARN: No control message(%s)", strerror(errno));
		return;
	}
	syslog(LOG_INFO, "Control packet of size:%d received", retval);

	// Extract HID and DAG from the message
	int index = 0;
	char hid[MAX_XID_STR_SIZE];
	char dag[MAX_HID_DAG_STR_SIZE];
	strcpy(hid, &packet[index]);
	index = strlen(hid) + 1;
	strcpy(dag, &packet[index]);
	index += strlen(dag);
	syslog(LOG_INFO, "New DAG for %s is %s", hid, dag);
	syslog(LOG_INFO, "Total data was %d bytes", index+1);

	// Extract AD from DAG
	int ADstringLength = strlen("AD:") + 40 + 1;
	char ADstring[ADstringLength];
	bzero(ADstring, ADstringLength);
	strncpy(ADstring, strstr(dag, "AD:"), ADstringLength-1);
	HIDtoAD[hid] = ADstring;
	syslog(LOG_INFO, "Added %s:%s to table", hid, ADstring);

	// Registration message
	// Extract HID, newAD, timestamp, Signature, Pubkey
	// Verify HID not already in table
	// Verify HID matches Pubkey
	// Verify Signature(HID, newAD, timestamp) using Pubkey
	// Set lastTimestamp for this HID
	//
	//
	// UpdateAD message
	// Extract HID, newAD, timestamp, Signature, Pubkey
	// Verify HID is in table
	// Verify HID matches Pubkey
	// Verify Signature(HID, newAD, timestamp) using Pubkey
	// Verify timestamp > lastTimestamp
	//
	// Heartbeat message?
}

int main(int argc, char *argv[]) {

	// Parse command-line arguments
	config(argc, argv);
	syslog(LOG_NOTICE, "%s started on %s", APPNAME, hostname);

	// Data plane socket used to rendezvous clients with services
	int datasock = getServerSocket(datasid, SOCK_RAW);
	int controlsock = getServerSocket(controlsid, SOCK_DGRAM);
	if(datasock < 0 || controlsock < 0) {
		syslog(LOG_ERR, "ERROR creating a server socket");
		return 1;
	}

	// Main loop checks data and control sockets for activity
	while(1) {
		fd_set read_fds;
		FD_ZERO(&read_fds);
		FD_SET(datasock, &read_fds);
		FD_SET(controlsock, &read_fds);
		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 1000;
		int retval = select(controlsock+1, &read_fds, NULL, NULL, &timeout);
		if(retval == -1) {
			syslog(LOG_ERR, "ERROR waiting for data to arrive. Exiting");
			return 2;
		}
		if(retval == 0) {
			// No data on control/data sockets, loop again
			// This is the place to add any actions between loop iterations
			continue;
		}
		// Check for control messages first
		if(FD_ISSET(controlsock, &read_fds)) {
			process_control_message(controlsock);
		}
		// Handle data packets
		if(FD_ISSET(datasock, &read_fds)) {
			process_data(datasock);
		}
	}
	return 0;

}
