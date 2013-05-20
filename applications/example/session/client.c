#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "session.h"

#ifdef DEBUG
#define LOG(s) fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, s)
#define LOGF(fmt, ...) fprintf(stderr, "%s:%d: " fmt"\n", __FILE__, __LINE__, __VA_ARGS__) 
#else
#define LOG(s)
#define LOGF(fmt, ...)
#endif


int main(int argc, char *argv[])
{
    int ctx, n;
    char reply[128];
    char buffer[2048];
    
	// Initiate session
	ctx = SnewContext();
	if (ctx < 0) {
		LOG("Error creating new context");
		exit(-1);
	}
	if (Sinit(ctx, "service, server", "client", "client") < 0 ) {
		LOG("Error initiating session");
		exit(-1);
	}

     
    while(1)
    {
		printf("\nPlease enter the message (0 to exit): ");
		bzero(buffer,2048);
		fgets(buffer,2048,stdin);
		if (buffer[0]=='0'&&strlen(buffer)==2)
		    break;
		    
		Ssend(ctx, buffer, strlen(buffer));
		
		//Process reply from server
		n = Srecv(ctx, reply, 128);
		if (n < 0) 
		    printf("Error receiving data");
		write(1,reply,n);
		printf("\n");
    }

	Sclose(ctx);
    return 0;
}

