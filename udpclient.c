/* mp3server.c
 * CS423-MP3
 * NETID: herga2, rmohiud2
 * The file which implements all the functionality required in the MP3
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <linux/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include "mp3.hh"
#define LEN_DATA 36
typedef int boolean;
uint16_t udp_port = 0;
uint16_t rem_port = 0;
uint16_t send_udp_port = 0;
struct sockaddr rem_addr = {0};
char remote_hostname[100] = {0};
struct pollfd fdinfo[2] = {0};
uint16_t last_data_req = 0;
FILE* fp;
#define handle_error(msg) \
        do { perror(msg); exit(EXIT_FAILURE); } while (0)

#define MAXDATASIZE 100
#define PAC(X) &pack_array[X]
struct packet_gen {
	uint16_t opcode;
	uint16_t seq_off;
	uint32_t seq;
	char buf[LEN_DATA];
} __attribute__ ((packed));

struct packet_ack {
	uint16_t opcode;
	uint16_t seq;
	uint16_t seq_end;
	char buf[100];
} __attribute__ ((packed));

enum {
	ACK = 1,
	DATA = 2,
    END = 3
};

struct pack_header {
    uint32_t seq;
    uint32_t seq_end;
};

struct packet_gen packet;
// RX buffer
static struct pack_header *pack_array = NULL;
static uint32_t array_max_length = 1000;
static uint32_t array_length;
static uint32_t array_first;
static uint32_t array_last;
static uint32_t last_got_beg;
static uint32_t last_next_beg;

static uint32_t
pa_seq (struct pack_header* temp)
{
        return temp->seq;
}

static uint32_t
pa_seqe (struct pack_header* temp)
{
        return temp->seq_end;
}

static struct pack_header*
new_pack_array (uint32_t *length, struct pack_header* old_pack_array)
{
    struct pack_header *temp;
    if (old_pack_array == NULL) {
        return (struct pack_header*)malloc((*length)*sizeof(struct pack_header));
    } else {
        // The request is to resize the array, we shouldnot see many such requests.
        *length = 2*(*length);
        temp = (struct pack_header*)malloc((*length)*sizeof(struct pack_header));
        memcpy(temp, old_pack_array, *length);
        return temp;
    }
}

static void
update_pack_array_elem (struct pack_header* temp, uint32_t seq,
                        uint32_t seq_end)
{
    temp->seq = seq;
    temp->seq_end = seq_end;
}

static void
update_buffer_rx (void)
{

    if (array_max_length <= array_length) {
        pack_array = new_pack_array(&array_max_length,
                                         (struct pack_header *)pack_array);
    }
    array_length++;
    update_pack_array_elem(PAC(array_first),
                           last_got_beg, last_next_beg);
    array_first = (array_first + 1) % array_max_length;
    //printf("\nRX: Buffer out of order %u:%u", array_first, array_last);
}

static uint32_t
downgrade_buffer_rx (void)
{
    int temp_index = array_last;
    uint32_t next_data;

    //loop based on if the buffer is contiguous.
    //support for randomly dropped packets, and out
    //of order packets.
    next_data = last_data_req;

    while(pa_seqe(PAC(temp_index)) <= last_next_beg) {
        if (pa_seq(PAC(temp_index)) == next_data) {
            next_data = pa_seqe(PAC(temp_index));
            update_pack_array_elem(PAC(temp_index), 0, 0);
            array_length--;
            if (array_length < 0) {
               array_length++;
               //printf("\nAssert!! array length -ve at %u", temp_index);
               break;
            }
            //printf("\nRX: Cleared:target %u:%u", temp_index, array_first);
            if (temp_index != array_first) {
                temp_index = (temp_index + 1) % array_max_length;
            } else {
                // Want to stop searching when head == tail, and still not 
                // equal, we do not want a case when tail > head.
                break;
            }
        } else {
            //1. Possible if array_last has reached array first, which is
            //   NULL
            //2. Possible if there has been another packet dropped in the 
            //   sequence

            last_data_req = next_data;
            printf("\nAsk for %u now", next_data);
            break;
        }
        //printf("\nContin: %u:%u",pa_seq(PAC(temp_index)), next_data);
        //printf ("\n EOP:IEOP %u:%u",pa_seqe(PAC(temp_index)), last_next_beg);
    }
    array_last = temp_index;
    return next_data;
}

int
file_setup (void)
{
    boolean write = 1;
    int i;
    if(!(fp=fopen("file_output", "w"))) {
          printf("\nCouldn't find or create the entry file");
          write = 0;
    } else {
        fclose(fp); fp=fopen("file_output", "r+");
        if (!fp) {
           printf("\nCouldn't reopen the file");
           write = 0;
        }
    }
    return write;
}

void
write_packet (char *buf, uint16_t len, uint32_t seq)
{
    int i;
    /*
    for (i = 0; i < len; i++) {
        putchar(*((char *)buf + i));
    }*/
    seq --;
    //printf("\nPrinting LEN:%u to file at SEQ:%u", len, seq);
    if(fseek(fp, seq*sizeof(char), SEEK_SET)) {
        //printf("\n Coudnot seek the position %d, skipping", seq);
    } else {
        fwrite(buf, len, 1, fp);
    }
}


/*
 * Returns the UDP port on which send operations may be done to the
 * designated port rem_port at a HOST remote_hostname.
 */
void
tx_udp_setup (void)
{
	struct addrinfo hints, *servinfo, *p;
	int rv, numbytes, sockfd;
    char port_string[10];
    char local_hostname[100] = {0};
    memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
    snprintf(port_string, 6, "%d", rem_port);
    if (strncmp(remote_hostname, "local", 5) == 0) {
         if(gethostname(local_hostname, 100)) {
             handle_error("Could not get hostname\n");
         }
         /* Using ews hostname to subvert issues with udp rx on localhost
          * Original hostname is not changed, the local pointer is pointed to
          * ews version.
          */
         memcpy(remote_hostname, local_hostname, 100);
    }


	if ((rv = getaddrinfo(remote_hostname, port_string, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		exit(EXIT_FAILURE);
	}

	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			continue;
		}
		break;
	}

	if (p == NULL) {
		fprintf(stderr, "tx: failed to bind socket\n");
	    exit(EXIT_FAILURE);
    }

    memcpy(&rem_addr, p->ai_addr, sizeof(rem_addr));
	freeaddrinfo(servinfo);
    send_udp_port = sockfd;
    return;
}

int
tx_udp_packet (char *buf, uint16_t len)
{
    int numbytes;
	if ((numbytes = mp3_sendto(send_udp_port, buf, len, 0, &rem_addr, sizeof(rem_addr))) == -1) {
		handle_error("tx_generic_udp_send_pkt: sendto");
	}
    return numbytes;
}

void
process_data (struct packet_gen *pack, int len)
{
	struct packet_gen buf = {0};
	uint32_t numbytes = 0;

	memcpy(&buf, pack, len);
    /*
	 * Send ACK, increment the sequence number asking for the next
	 */
	buf.opcode = ACK;
	if (last_data_req != buf.seq) {
		// Write the packet anyway, but notify the sender of what you need
        /// file works as a buffer here, we are not going to flush our window.
        write_packet(buf.buf, buf.seq_off, buf.seq);
        last_got_beg = buf.seq;
        last_next_beg = buf.seq + buf.seq_off + 1;
        update_buffer_rx();       
        // Setup the DUP ACK
        buf.seq = last_data_req;
		buf.seq_off = 0;
	} else {
	    // Ingest the packet
        write_packet(buf.buf, buf.seq_off, buf.seq);
        if (!array_length) {
            buf.seq_off++;
            buf.seq += buf.seq_off;
            buf.seq_off = 0;
            last_data_req = buf.seq;
        } else {
            last_data_req = buf.seq + buf.seq_off + 1;
            //Check if this data is there in buffer, double packet drop
            //or, we need to send ACK for all the buffered packets
            last_data_req = downgrade_buffer_rx();
            buf.seq = last_data_req;
            buf.seq_off = 0;
        }
	}
	numbytes = tx_udp_packet((char *)&buf, sizeof(struct packet_gen));
}
void
process_input_udp (int file)
{
    char buf[MAXDATASIZE] = {0};
    uint8_t opcode, numbytes;
 	socklen_t addr_len;
    struct sockaddr_storage their_addr;
    int i;
	   
 	addr_len = sizeof their_addr;
	if ((numbytes = recvfrom(file, buf, MAXDATASIZE-1 , 0,
		(struct sockaddr *)&their_addr, &addr_len)) == -1) {
		handle_error("recvfrom");
	}
    if (numbytes == 0) {
        printf("\n Returning");
    }
	//printf("\nClient: packet is %d bytes long\n", numbytes);
    buf[numbytes] = '\0';

    struct packet_gen* pack = (struct packet_gen*)buf;
    //printf("\n%u:%u", pack->seq, pack->seq + pack->seq_off);
	if (pack->opcode == DATA) {
        process_data(pack, numbytes);
    } else if (pack->opcode == END) {
        if (fp) 
            fclose(fp);
        fflush(NULL);
        handle_error("Closing Check file \"file output\"");
    }
} 

void
process_input (int file)
{
    //printf("\n fdinfo 0 %d fdinfo 1 %d file %d", fdinfo[0].fd, fdinfo[1].fd, file);
	process_input_udp(file);
}

/*
 * This function opens a TCP socket on user defined port, spawns two threads, one for getting 
 *  and valioating data from the client and another for interpreting this data and pasting 
 *  the data to a file.
 */ 
int main (int argc, char *argv[])
{
    int sockfd, res;
    int count = 0; //Describing the number of Fds to be polled
    struct addrinfo hints, *servinfo, *p;
    int rv, i;
    struct sockaddr_in sin = {0};

    if (argc != 4) {
        fprintf(stderr,"usage: mp3client remote_host remote_port myudp_port\n");
        exit(1);
    }

    if (!file_setup()) {
        handle_error("write_file_open");
    }
    strncpy(remote_hostname, argv[1], 100);
    printf("\n Hostname: %s", remote_hostname);

    mp3_init();
    /*
     * Setup UDP to tx the packet to the designated port
     */
    rem_port = (short)atoi(argv[2]);
    tx_udp_setup();
    /*
     * Setup UDP to rx the packet on the designated port, which comes up 
     * on any IP address in the box.
     */
    udp_port = (short)atoi(argv[3]);
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        handle_error("socket");
    }

    sin.sin_family = AF_INET;
    sin.sin_port = htons(udp_port);
    sin.sin_addr.s_addr = INADDR_ANY;
    if (bind(sockfd, (struct sockaddr *) &sin, sizeof(sin)) == -1) {
        handle_error("bind");
    }

	packet.opcode = ACK;
	packet.seq = 1;
    packet.seq_off = 1;
	last_data_req = 1;
	snprintf(packet.buf, 3, "Hi\n");
    tx_udp_packet((char *)&packet, sizeof(struct packet_gen));

    fdinfo[0].fd = sockfd;
    fdinfo[0].events = POLLIN;
    count++;

    pack_array = new_pack_array(&array_max_length, NULL);

    i = fcntl(fdinfo[0].fd, F_SETFL, O_NONBLOCK);
    while (1) {
        res = poll(fdinfo, count, 1);
        if (res == -1) {
            printf("\n res -1, continue");
            sleep (1);
            continue;
        }
        for (i = 0; i < count && (res > 0); i++) {
            if (fdinfo[i].revents != 0) {
                res--;
                //printf ("\nFound event with fd:%d", i);
                if (fdinfo[i].revents & (POLLERR | POLLNVAL | POLLHUP)) {
                    if (fdinfo[i].revents & (POLLIN))
                        process_input(fdinfo[i].fd);
                    else {
                        printf ("\nError in fd %d", i);
                        exit(1);
                    }
                } else if (fdinfo[i].revents & (POLLIN)) {
                    process_input(fdinfo[i].fd);
                }
            }
        }
    }
    
    return 0;
}
