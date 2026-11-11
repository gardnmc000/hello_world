/*
 * main.cpp
 *
 *  Created on: 30 Oct 2026
 *      Author: mike
 */

/*
** server.c -- a stream socket server demo
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>


#define SOCK_PORT "3591"  // the port users will be connecting to


#define BACKLOG 3   // how many pending connections queue will hold
#define BLOCKLEN 1024 	// Length of chunk in bytes

/*   Prototypes  */

int storeblock(char * filename, char * block, int blocksize);


void sigchld_handler(int s)
{
	(void)s; // quiet unused variable warning

	// waitpid() might overwrite errno, so we save and restore it:
	int saved_errno = errno;

	while(waitpid(-1, NULL, WNOHANG) > 0);

	errno = saved_errno;
}


// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(void)
{
        // listen on sock_fd, new connection on new_fd
        int sockfd, new_fd;
        struct addrinfo hints, *servinfo, *p;
        struct sockaddr_storage their_addr; // connector's address info
        socklen_t sin_size;
        struct sigaction sa;
        int yes=1;
        char s[INET6_ADDRSTRLEN];
        int rv;

        // These can remain here as they are duplicated upon fork()
        char inbuf[BLOCKLEN];
        char nambuf[32];
        char sigbuf[16];
        char sigbytes[8] = {0x19,0x57,0x55, 0xaa, 0xc0, 0x03,0xF0, 0x0f};
        char outbuf[32];
        int i, val;
        char filename[64];
        bool connection_ok;

        time_t rawtime;
        struct tm *loctim;
        char sztime[64];

        struct timeval timeout;
        timeout.tv_sec = 15;  // 10 seconds timeout
        timeout.tv_usec = 0;  // 0 microseconds

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE; // use my IP

        if ((rv = getaddrinfo(NULL, SOCK_PORT, &hints, &servinfo)) != 0) {
                fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
                return 1;
        }

        // loop through all the results and bind to the first we can
        for(p = servinfo; p != NULL; p = p->ai_next) {
                if ((sockfd = socket(p->ai_family, p->ai_socktype,
                                p->ai_protocol)) == -1) {
                        perror("server: socket");
                        continue;
                }

                if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                                sizeof(int)) == -1) {
                        perror("setsockopt");
                        exit(1);
                }

                if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
                        close(sockfd);
                        perror("server: bind");
                        continue;
                }

                break;
        }

        freeaddrinfo(servinfo); // all done with this structure

        if (p == NULL)  {
                fprintf(stderr, "server: failed to bind\n");
                exit(1);
        }

        if (listen(sockfd, BACKLOG) == -1) {
                perror("listen");
                exit(1);
        }

        sa.sa_handler = sigchld_handler; // reap all dead processes
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        if (sigaction(SIGCHLD, &sa, NULL) == -1) {
                perror("sigaction");
                exit(1);
        }

        printf("server: waiting for connections on port: %s...\n", SOCK_PORT);

        while(1) {  // main accept() loop
                sin_size = sizeof their_addr;
                new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);

                if (new_fd == -1) {
                        perror("accept");
                        continue;
                }

                /*  Make a note of the time of this connection  */
                time(&rawtime);
                loctim = localtime(&rawtime);
                sprintf(sztime, "%04d:%02d:%02d %02d:%02d", loctim->tm_year+1900, loctim->tm_mon, loctim->tm_mday, loctim->tm_hour, loctim->tm_min);

                sockaddr_in * p_addr = (sockaddr_in *) get_in_addr((struct sockaddr *)&their_addr);
                inet_ntop(their_addr.ss_family, p_addr, s, sizeof s);
                printf("%s: Incoming connection from %s\n", sztime, s);

                if (!fork()) { // CHILD PROCESS WORKER
                        close(sockfd); // Child doesn't need the listener

                        // Prevent crash if writing to a suddenly disconnected socket
                        signal(SIGPIPE, SIG_IGN);

                        // Attach the 10-second timeout
                        if (setsockopt(new_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
                                perror("setsockopt SO_RCVTIMEO failed");
                        }


                        memset(sigbuf, 0, 16);
                        val = recv(new_fd, sigbuf, 8, 0);

                        if (val <= 0) {
                                if (val < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                        printf("Timeout waiting for filename\n");
                                } else {
                                        perror("recv filename error or disconnect");
                                }
                                close(new_fd);
                                exit(1);
                        }

                        bool miss = false;
                        for (i = 0; i < 8; i++){
                        	miss = sigbytes[i] != sigbuf[i];
                        	if (miss) {
                        		printf("Signature mis-match\r\nClosing socket\r\n");
                                close(new_fd);
                                exit(1);
                        	}
                        }

                        uint16_t clip_len = 0x8888, clip_interval = 0x7777, freq_slope = 0x6666, vol_threshold = 0x5555, res_1 = 0x4444, res_2 = 0x3333;
                        memset(nambuf, 0, 32);
                        memcpy(&nambuf[0], "CFG_", 4);
                        memcpy(&nambuf[4], &clip_len, 2);
                        memcpy(&nambuf[6], &clip_interval, 2);
                        memcpy(&nambuf[8], &freq_slope, 2);
                        memcpy(&nambuf[10], &vol_threshold, 2);
                        memcpy(&nambuf[12], &res_1, 2);
                        memcpy(&nambuf[14], &res_2, 2);

                        if (send(new_fd, nambuf, 16, 0) == -1) {
                                 perror("send CFG_ failed");
                                 close(new_fd);
                                 exit(1);
                         }

                        memset(nambuf, 0, 32);
                        /*   Receive the configuration file - mandatory file name: "configuration.cfg"  */
                        val = recv(new_fd, nambuf, 17, 0);

                        if (val <= 0) {
                                if (val < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                        printf("Timeout waiting for filename\n");
                                } else {
                                        perror("recv filename error or disconnect");
                                }
                                close(new_fd);
                                exit(1);
                        }

                        memset(filename, 0, 64);
                        sprintf(filename, "%s", nambuf);
                        printf("Filename is %s\r\n", filename);

                        if (send(new_fd, "GO", 2, 0) == -1) {
                                perror("send GO failed");
                                close(new_fd);
                                exit(1);
                        }

                        printf("Waiting for %s body\r\n", filename);
                        i = 0;
                        connection_ok = true;

                        // Corrected & safe data stream loop
                        if (connection_ok) {
							memset(inbuf, 0, BLOCKLEN);
							val = recv(new_fd, inbuf, BLOCKLEN, 0);

							if (val < 0) {
									if (errno == EAGAIN || errno == EWOULDBLOCK) {
											printf("Timeout reached reading file %s body.\n", filename);
									} else {
											perror("recv body error");
									}
									connection_ok = false;
									break;
							}
							if (val == 0) {
									printf("Client closed connection gracefully.\n");
									break;
							}

							// Process valid data inside the loop
							storeblock(filename, inbuf, val);
							i++;

							sprintf(outbuf, "OK%03d", i);
							if (send(new_fd, outbuf, 5, 0) == -1) {
									perror("send OK ack failed");
									connection_ok = false;
									break;
							}
							if ((i % 55) == 0) {
								printf("%s:%03d\r\n", filename, i);
							}
                        }

                        // Only send DONE if the connection didn't experience an error/timeout
                        if (connection_ok) {
                                send(new_fd, "DONE", 4, 0);
                                time(&rawtime);
                                loctim = localtime(&rawtime);
                                sprintf(sztime, "%04d/%02d/%02d %02d:%02d", loctim->tm_year+1900, loctim->tm_mon, loctim->tm_mday, loctim->tm_hour, loctim->tm_min);
                                printf("%s Received %d packets total for file %s\n\n", sztime, i, filename);
                        }

                        // Guaranteed child cleanup
                        close(new_fd);
                        exit(0);
                } // <--- CHILD PROCESS ENDS HERE

                // PARENT PROCESS CONTINUES HERE
                close(new_fd); // Parent closes its reference to hand it completely to the child
        } // <--- MAIN ACCEPT LOOP ENDS HERE

        return 0;
}

int storeblock(char * filename, char * block, int blocksize) {

	int ret = -1;
	FILE * f_out = NULL;

	f_out = fopen(filename, "a" );
	if (f_out != NULL) {
		ret = fwrite(block, 1, blocksize, f_out);
		fclose(f_out);
	}
	else {
		printf("Unable to open file %s\r\n", filename);
	}
	return(ret);
}



