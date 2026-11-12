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
#include <ctype.h>
#include <stdint.h>
#include <strings.h>
#include <sys/stat.h>
#include <openssl/evp.h>


//#define SOCK_PORT "3492"  // the port users will be connecting to
#define SOCK_PORT "3590"  // the port users will be connecting to
#define BACKLOG 3   // how many pending connections queue will hold
#define BLOCKLEN 1024 	// Length of chunk in bytes

/*   Prototypes  */

int storeblock(char * filename, char * block, int blocksize);

#define CLIP_NAME_LEN 22

/*  The name comes from the network, so it must be exactly a clip name:
 *    D (or E, for a name moved on after a clash), 6 lowercase hex digits from the MAC,
 *    11 timestamp digits (yy ddd hh mm ss), then ".wav". That is always 22 bytes.
 *
 *  TEMPORARY: names starting "D_" come from units still on the old firmware
 *  (D_4c30), whose 24-byte names arrive cut to 22. They are accepted as before,
 *  as long as they are plain file names. Remove this once D_4c30 is updated.  */
static bool valid_clip_name(const char *name, int length)
{
	if (length != CLIP_NAME_LEN) {
		return false;
	}
	if (name[0] == 'D' && name[1] == '_') {
		for (int i = 0; i < length; i++) {
			unsigned char c = (unsigned char) name[i];
			if (!(isalnum(c) || c == '_' || c == '.' || c == '-')) {
				return false;
			}
		}
		return true;
	}
	if (name[0] != 'D' && name[0] != 'E') {
		return false;
	}
	for (int i = 1; i <= 6; i++) {
		if (!isdigit((unsigned char) name[i]) && !(name[i] >= 'a' && name[i] <= 'f')) {
			return false;
		}
	}
	for (int i = 7; i <= 17; i++) {
		if (!isdigit((unsigned char) name[i])) {
			return false;
		}
	}
	return memcmp(name + 18, ".wav", 4) == 0;
}


/* ------------------------------------------------------------------------------
 *  Firmware check and download (OTA phase 2; see docs/ota-phase2-design.md)
 *
 *  unit -> server   signature (8)   19 57 55 aa c0 03 f0 1f
 *  unit -> server   HELLO (64)      "FWH1", id[8], version[32], battery mV (u16),
 *                                   flags (u16: bit 0 on trial, bits 8-15 reset reason),
 *                                   rolled-back version[16]
 *  server -> unit   OFFER (72)      "NONE" | "OFFR", version[32], size (u32), sha256[32]
 *  unit -> server   GET (8)         "GET1", offset (u32)
 *  server -> unit   image bytes from offset to the end, then close
 *
 *  All integers are little-endian; strings are NUL-padded.
 *  Firmware lives in $BW_FIRMWARE_DIR (default ~/firmware):
 *    bird_watch.bin   the signed image on offer
 *    devices.txt      optional rollout list: only these ids are offered it
 *    status/<id>      the latest HELLO from each unit
 * ------------------------------------------------------------------------------ */

static const unsigned char fw_signature[8] = {0x19, 0x57, 0x55, 0xaa, 0xc0, 0x03, 0xf0, 0x1f};

#define FW_HELLO_LEN   64
#define FW_OFFER_LEN   72
#define FW_GET_LEN     8
#define FW_MAX_IMAGE   (3 * 1024 * 1024)        /*  One app partition  */
#define FW_DESC_OFFSET 32                       /*  esp_app_desc_t, after the image and segment headers  */
#define FW_DESC_MAGIC  0xABCD5432u

static bool recv_exact(int fd, void *buf, size_t len)
{
	size_t got = 0;
	while (got < len) {
		ssize_t n = recv(fd, (char *) buf + got, len - got, 0);
		if (n <= 0) {
			return false;
		}
		got += (size_t) n;
	}
	return true;
}

static bool send_all(int fd, const void *buf, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(fd, (const char *) buf + sent, len - sent, 0);
		if (n <= 0) {
			return false;
		}
		sent += (size_t) n;
	}
	return true;
}

static uint16_t get_u16(const unsigned char *p) { return (uint16_t) (p[0] | (p[1] << 8)); }
static uint32_t get_u32(const unsigned char *p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
static void put_u32(unsigned char *p, uint32_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff; }

/*  Copies a NUL-padded field, making sure the result is terminated and printable  */
static void copy_field(char *out, size_t out_size, const unsigned char *in, size_t in_len)
{
	size_t i;
	for (i = 0; i < in_len && i + 1 < out_size && in[i] != '\0'; i++) {
		out[i] = isprint(in[i]) ? (char) in[i] : '?';
	}
	out[i] = '\0';
}

/*  The id becomes a file name, so it must be 1 to 8 letters or digits  */
static bool valid_device_id(const char *id)
{
	size_t len = strlen(id);
	if (len == 0 || len > 8) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (!isalnum((unsigned char) id[i])) {
			return false;
		}
	}
	return true;
}

static void firmware_dir(char *out, size_t size)
{
	const char *dir = getenv("BW_FIRMWARE_DIR");
	if (dir != NULL && dir[0] != '\0') {
		snprintf(out, size, "%s", dir);
	} else {
		const char *home = getenv("HOME");
		snprintf(out, size, "%s/firmware", home != NULL ? home : ".");
	}
}

/*  No devices.txt means every unit is offered the image  */
static bool in_rollout(const char *dir, const char *id)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/devices.txt", dir);
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return true;
	}
	char line[128];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f) != NULL) {
		char *p = line;
		while (isspace((unsigned char) *p)) p++;
		if (*p == '#') continue;
		char *end = p;
		while (*end != '\0' && !isspace((unsigned char) *end) && *end != '#') end++;
		*end = '\0';
		found = strcasecmp(p, id) == 0;
	}
	fclose(f);
	return found;
}

/*  Loads the image, reads its version from its own app description, and hashes it  */
static unsigned char *load_image(const char *dir, size_t *size, char *version, size_t version_size, unsigned char sha[32])
{
	char path[512];
	snprintf(path, sizeof(path), "%s/bird_watch.bin", dir);
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	unsigned char *buf = (unsigned char *) malloc(FW_MAX_IMAGE);
	size_t n = (buf != NULL) ? fread(buf, 1, FW_MAX_IMAGE, f) : 0;
	bool too_big = (buf != NULL && n == FW_MAX_IMAGE && fgetc(f) != EOF);
	fclose(f);
	if (buf == NULL || n < FW_DESC_OFFSET + 48 || too_big ||
	    get_u32(buf + FW_DESC_OFFSET) != FW_DESC_MAGIC) {
		printf("Firmware %s is missing, too big, or not an ESP32 app image\n", path);
		free(buf);
		return NULL;
	}
	copy_field(version, version_size, buf + FW_DESC_OFFSET + 16, 32);

	unsigned int sha_len = 0;
	if (EVP_Digest(buf, n, sha, &sha_len, EVP_sha256(), NULL) != 1 || sha_len != 32) {
		printf("Unable to hash %s\n", path);
		free(buf);
		return NULL;
	}
	*size = n;
	return buf;
}

/*  HELLO flags bits 8-15: why the unit's current boot started (ESP-IDF esp_reset_reason_t).
 *  Units before 1.4.5 send 0 here, which reads as "unknown".  */
static const char *reset_reason_name(unsigned reason)
{
	static const char *names[] = {
		"unknown", "power-on", "reset pin", "software restart", "crash", "interrupt watchdog",
		"task watchdog", "watchdog", "deep sleep wake", "brownout", "SDIO", "USB", "JTAG",
		"eFuse error", "power glitch", "CPU lockup"
	};
	return reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "unknown";
}

static void write_status(const char *dir, const char *id, const char *ip, const char *version,
                         unsigned battery_mv, unsigned flags, const char *rolled_back)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/status", dir);
	mkdir(dir, 0755);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/status/%s", dir, id);

	time_t now = time(NULL);
	char when[32];
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));

	FILE *f = fopen(path, "w");
	if (f == NULL) {
		printf("Unable to write %s\n", path);
		return;
	}
	fprintf(f, "time=%s\nip=%s\nversion=%s\nbattery_mv=%u\non_trial=%u\nrolled_back=%s\nlast_reset=%s\n",
	        when, ip, version, battery_mv, flags & 1u, rolled_back, reset_reason_name(flags >> 8));
	fclose(f);
}

static void handle_firmware(int fd, const char *ip)
{
	unsigned char hello[FW_HELLO_LEN];
	if (!recv_exact(fd, hello, sizeof(hello)) || memcmp(hello, "FWH1", 4) != 0) {
		printf("FW %s: no valid HELLO\n", ip);
		return;
	}

	char id[16], version[40], rolled_back[24];
	copy_field(id, sizeof(id), hello + 4, 8);
	copy_field(version, sizeof(version), hello + 12, 32);
	unsigned battery_mv = get_u16(hello + 44);
	unsigned flags = get_u16(hello + 46);
	copy_field(rolled_back, sizeof(rolled_back), hello + 48, 16);

	if (!valid_device_id(id)) {
		printf("FW %s: rejected device id \"%s\"\n", ip, id);
		return;
	}
	printf("FW %s: unit %s running %s, battery %u mV, last reset %s%s%s%s\n", ip, id, version, battery_mv,
	       reset_reason_name(flags >> 8),
	       (flags & 1u) ? ", on trial" : "",
	       rolled_back[0] ? ", rolled back " : "", rolled_back);

	char dir[400];
	firmware_dir(dir, sizeof(dir));
	write_status(dir, id, ip, version, battery_mv, flags, rolled_back);

	unsigned char offer[FW_OFFER_LEN];
	memset(offer, 0, sizeof(offer));
	memcpy(offer, "NONE", 4);

	size_t size = 0;
	char image_version[40];
	unsigned char sha[32];
	unsigned char *image = NULL;
	if (in_rollout(dir, id)) {
		image = load_image(dir, &size, image_version, sizeof(image_version), sha);
	}
	if (image != NULL && strcmp(image_version, version) != 0) {
		memcpy(offer, "OFFR", 4);
		memcpy(offer + 4, image_version, strnlen(image_version, 31));
		put_u32(offer + 36, (uint32_t) size);
		memcpy(offer + 40, sha, 32);
	}

	bool offering = memcmp(offer, "OFFR", 4) == 0;
	if (!send_all(fd, offer, sizeof(offer)) || !offering) {
		free(image);
		return;
	}
	printf("FW %s: offered %s (%zu bytes)\n", id, image_version, size);

	/*  The unit closes here if it decides not to download (battery, blocked version)  */
	unsigned char get[FW_GET_LEN];
	if (!recv_exact(fd, get, sizeof(get)) || memcmp(get, "GET1", 4) != 0) {
		printf("FW %s: declined the offer\n", id);
		free(image);
		return;
	}
	uint32_t offset = get_u32(get + 4);
	if (offset > size) {
		printf("FW %s: invalid offset %u\n", id, offset);
		free(image);
		return;
	}

	/*  A stalled unit must not hold this process forever  */
	struct timeval send_timeout = {30, 0};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

	/*  TEST ONLY: a byte count in test_cutoff makes the server stop there, as if the
	 *  connection had dropped, so a unit's resume can be tested. Delete the file after.  */
	size_t end = size;
	char cutoff_path[512];
	snprintf(cutoff_path, sizeof(cutoff_path), "%s/test_cutoff", dir);
	FILE *cf = fopen(cutoff_path, "r");
	if (cf != NULL) {
		unsigned long cutoff = 0;
		if (fscanf(cf, "%lu", &cutoff) == 1 && cutoff > offset && cutoff < size) {
			end = cutoff;
			printf("FW %s: TEST cutoff at %lu bytes\n", id, cutoff);
		}
		fclose(cf);
	}

	size_t pos = offset;
	while (pos < end) {
		size_t chunk = (end - pos < 4096) ? end - pos : 4096;
		if (!send_all(fd, image + pos, chunk)) {
			break;
		}
		pos += chunk;
	}
	printf("FW %s: sent %zu of %zu bytes from offset %u%s\n", id, pos - offset, size - offset, offset,
	       pos == size ? "" : " (connection lost)");
	free(image);
}


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
		sprintf(sztime, "%04d:%02d:%02d %02d:%02d", loctim->tm_year+1900, loctim->tm_mon+1, loctim->tm_mday, loctim->tm_hour, loctim->tm_min);

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

			/*  The signature selects the conversation: clip upload, or firmware check  */
			if (val == 8 && memcmp(sigbuf, fw_signature, 8) == 0) {
				handle_firmware(new_fd, s);
				close(new_fd);
				exit(0);
			}

			bool miss = false;
			for (i = 0; i < 8; i++){
				miss = sigbytes[i] != sigbuf[i];
				if (miss) {
					printf("Signature mis-match\n");
					close(new_fd);
					exit(1);
				}
			}





				//			if (!fork()) { // this is the (new) child process
				//				close(sockfd); // child (original process) doesn't need the listener
				//				/*  Indicate server is ready to receive the file  */
				//				if (send(new_fd, "READY", 5, 0) == -1) {
				//					perror("send");
				//				}
				//				else {
				//					/*  Expect filename "D_xxxx_dddhhmmss.wav"  */
				//					memset(nambuf, 0, 32);
				//					val = recv(new_fd, nambuf, 20, 0);
				//					sprintf(filename, "%s", nambuf);
				//					printf("Filename is %s\r\n", filename);
				//					send(new_fd, "GO", 2, 0);
				//					printf("Waiting for %s body\r\n", filename);
				//					val = BLOCKLEN;
				//					i = 0;
				//					while (val > 0) {
				//						memset(inbuf, 0, BLOCKLEN);
				//						val = recv(new_fd, inbuf, BLOCKLEN, 0);
				//						if (val > 0) {
				//							storeblock(filename, inbuf, val);
				//							i++;
				//		//					sleep(1);
				//							//sprintf(outbuf, "OK%03d", i);
				//							//send(new_fd, outbuf, 5, 0);
				//							printf("Received block %03d, %04d bytes\r\n", i, val);
				//						}
				//						else {
				//							printf("Receive reports : %d\r\n", val);
				//						}
				//					}
				//					//printf ("Sending DONE\r\n", i);
				//					send(new_fd, "DONE", 4, 0);
				//					time(&rawtime);
				//					loctim = localtime(&rawtime);
				//					sprintf(sztime, "%04d/%02d/%02d %02d:%02d", loctim->tm_year+1900, loctim->tm_mon, loctim->tm_mday, loctim->tm_hour, loctim->tm_min);
				//					printf ("%s  Received %d packets total for file %s\n\n", sztime, i, filename);
				//				}
				//






















			/*  Indicate server is ready to receive the file  */
			if (send(new_fd, "READY", 5, 0) == -1) {
				perror("send");
			}
			else {

				/*  Expect filename "Dxxxxxxyydddhhmmss.wav" (older units: "D_xxxx_yydddhhmmss.wav")  */
				/*  A name can arrive split across packets, so collect all 22 bytes;
				 *  a shorter one ends with the 15 s timeout and is refused below  */
				memset(nambuf, 0, 32);
				val = 0;
				while (val < CLIP_NAME_LEN) {
					int got = recv(new_fd, nambuf + val, CLIP_NAME_LEN - val, 0);
					if (got <= 0) {
						break;
					}
					val += got;
				}
				if (val <= 0) {
					printf("No filename received\n");
					close(new_fd);
					exit(1);
				}
				if (!valid_clip_name(nambuf, val)) {
					printf("Rejected filename from %s: ", s);
					for (i = 0; i < val; i++) {
						printf(isprint((unsigned char) nambuf[i]) ? "%c" : "\\x%02x", (unsigned char) nambuf[i]);
					}
					printf("\n");
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
				while (connection_ok) {
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
					sprintf(sztime, "%04d/%02d/%02d %02d:%02d", loctim->tm_year+1900, loctim->tm_mon+1, loctim->tm_mday, loctim->tm_hour, loctim->tm_min);
					printf("%s Received %d packets total for file %s\n\n", sztime, i, filename);
				}

				// Guaranteed child cleanup
				close(new_fd);
				exit(0);
			}	// <--- CHILD PROCESS ENDS HERE
		}
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

