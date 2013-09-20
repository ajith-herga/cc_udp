/* udpserver.c
 */
#include <fcntl.h>
#include <time.h>
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
#include <sys/timerfd.h>
#include <sys/time.h>
#include <math.h>
#include "mp3.hh"

#define LEN_DATA 36
typedef int boolean;
uint16_t udp_port = 0;
uint16_t rem_port = 0;
uint16_t send_udp_port = 0;
struct sockaddr rem_addr = {0};
char remote_hostname[100] = {0};
struct pollfd fdinfo[2] = {0};
uint64_t tot_exp = 0;
FILE* fp;
int ok_to_end = 0, retransmit = 0;
#define PAC(X) (struct pack_header*)&tcp->pack_array[X]
#define handle_error(msg) \
        do { perror(msg); exit(EXIT_FAILURE); } while (0)

char* phase_string[] = {"NONE", "SLOW_START", "CONG_AVOID", "FAST_RECOV"};
#define MAXDATASIZE 100
#define MAXSTHRESH 65535

struct packet_gen {
	uint16_t opcode;
	uint16_t seq_off;
	uint32_t seq;
	char buf[LEN_DATA];
} __attribute__ ((packed));

struct pack_header {
	uint64_t timestamp;
	uint32_t seq;
	uint32_t seq_end;
};

struct tcp_conn {
	uint32_t socket;
	uint32_t timer_fd;
	uint32_t cwnd;  // will keep track of the current conj window
	uint32_t unack; //will keep track of the currently unack packets out there.
	uint32_t ssthresh; // will keep track of the ssthresh
	uint32_t next_ack;
	uint32_t last_sent_beg; // will keep track of the beg number of the last pkt out.
	uint32_t last_sent_end; //will keep track of the last sequence sent out.
	uint32_t last_ack; // will keep track of the last ack got.
	uint16_t array_first; // ;; timestamp of the latest element of the pkt sent.
	uint16_t array_last; // timestamp of the oldest element of pkt sent.
	uint16_t array_length; // length of the array of pack timestamps
	uint16_t array_max_length; // max length of the array
	struct pack_header *pack_array; //pack_array store timestamps of packets.
    uint8_t phase; //Phase of the tcp connection
    uint8_t duplicate_ack; // Number of duplicate acks for the connection
    uint64_t rtt; // RTT in usecs
};

struct tcp_conn *one_tcp;

enum {
	ACK = 1,
	DATA = 2,
    END = 3
};

enum {
	SLOWS = 1,
    CONG_AVOID = 2,
	FAST_RECOVERY = 3
};

static struct pack_header*
new_pack_array (uint16_t *length, struct pack_header *old_pack_array)
{
    struct pack_header  *temp;
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
update_pack_array_elem (struct pack_header* temp, uint64_t timestamp,
                        uint32_t seq, uint32_t seq_end)
{
    temp->timestamp = timestamp;
    temp->seq = seq;
    temp->seq_end = seq_end;
}

static uint64_t
pa_time (struct pack_header* temp)
{
    return temp->timestamp; 
}

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

static struct tcp_conn*
new_tcp_conn (uint32_t socket, uint32_t timer_fd, struct tcp_conn *tcp)
{
    if (tcp == NULL) {
        tcp = (struct tcp_conn *)malloc(sizeof(struct tcp_conn));
    } else {
        free(tcp->pack_array);
    }
	tcp->socket = socket;
	tcp->timer_fd = timer_fd;
	tcp->cwnd = 0;
	tcp->unack = 0;
    tcp->ssthresh = 65535;
	tcp->next_ack = 0;
	tcp->last_ack = 0;
	tcp->last_sent_end = 0;
	tcp->last_sent_beg = 0;
	tcp->array_first = 0;
	tcp->array_last = 0;
	tcp->array_max_length = 1000;
	tcp->array_length = 0;
	tcp->pack_array = new_pack_array((uint16_t *)&tcp->array_max_length, NULL);
    tcp->phase = SLOWS;
    tcp->duplicate_ack = 0;
    tcp->rtt = 1000000;
	return tcp;
}

static uint64_t
get_elapsed_time (boolean print)
{
    static struct timeval start_val;
    struct timeval curr_val;
    static int first_call = 1;
    uint64_t secs;
	int64_t usecs;

    if (first_call) {
         first_call = 0;
         if (gettimeofday(&start_val, NULL) == -1)
             handle_error("gettimeofday");
    }

    if (gettimeofday(&curr_val, NULL) == -1)
         handle_error("gettimeofday");

	secs = curr_val.tv_sec - start_val.tv_sec;
    usecs = curr_val.tv_usec - start_val.tv_usec;
    if (usecs < 0) {
        secs--;
        usecs += 1000000;
    }

	return (secs*1000000)+usecs;
}

int
init_timer_setup (void)
{
    struct itimerspec new_value;
    int fd;
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) == -1)
        handle_error("clock_gettime");

    new_value.it_value.tv_sec = now.tv_sec + 1;
    new_value.it_value.tv_nsec = now.tv_nsec;
    
    new_value.it_interval.tv_sec = 1;
    new_value.it_interval.tv_nsec = 0;

    fd = timerfd_create(CLOCK_REALTIME, 0);
    if (fd == -1)
        handle_error("timerfd_create");

   if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &new_value, NULL) == -1)
        handle_error("timerfd_settime");

   get_elapsed_time(0);

   return fd;
}

int
setup_timer (void)
{
    struct itimerspec new_value;
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) == -1)
        handle_error("clock_gettime");

    new_value.it_value.tv_sec = now.tv_sec + 1;
    new_value.it_value.tv_nsec = now.tv_nsec;
    
    new_value.it_interval.tv_sec = 0;
    new_value.it_interval.tv_nsec = (one_tcp->rtt*1000);

    if (timerfd_settime(fdinfo[1].fd, TFD_TIMER_ABSTIME, &new_value, NULL)
                         == -1) {
        handle_error("timerfd_settime");
    }

    printf("\n\t\tRTT:%6lu usec: Timer Set", one_tcp->rtt);
}

static void
update_timestamp_tx (struct tcp_conn *tcp)
{
   uint64_t timestamp_local = get_elapsed_time(0);
   int i;
   if (tcp->array_max_length <= tcp->array_length) {
       tcp->pack_array = new_pack_array((uint16_t *)&tcp->array_max_length,
                                        tcp->pack_array);
   }
   update_pack_array_elem(PAC(tcp->array_first), timestamp_local,
                          tcp->last_sent_beg,
                          tcp->last_sent_end);
   tcp->array_length++;
   /*printf("\nTX: Timestamp %lu at  %u, Seq: %u",
          pa_time(PAC(tcp->array_first)),
          tcp->array_first, pa_seq(PAC(tcp->array_first)));
   */
   tcp->array_first = (tcp->array_first + 1) % tcp->array_max_length;

}

static void
downgrade_timestamp_tx (struct tcp_conn *tcp)
{
    int temp_index = tcp->array_last, absent = 0;
    while(pa_seq(PAC(temp_index)) <= one_tcp->last_ack) {
        update_pack_array_elem(PAC(temp_index), 0, 0, 0);
        tcp->array_length--;
        //printf("\nRX: Cleared waiting at location %u", temp_index);
        if (temp_index != tcp->array_first) {
            temp_index = (temp_index + 1) % tcp->array_max_length;
        } else {
            // Want to stop searching when head == tail, and still not 
            // equal, we do not want a case when tail > head.
            break;
        }
        tcp->array_last = temp_index;
    }
}

uint16_t
get_file_data (char *buf, uint32_t seq, uint16_t len)
{
    uint16_t len_read = 0;
    
    if (seq == 1) seq--;
    else seq -= 2;

    if(fseek(fp, seq*sizeof(char), SEEK_SET)) {
        printf("\n Coudnot seek the position %d, skipping", seq);
    } else {
        if((len_read = fread(buf, 1, len, fp)) == -1) {
            printf("\n Coudnot read, Skipping");
            return 0;
        } else return (len_read);
    }
    return 0;
}

int
tx_udp_packet (char *buf, uint16_t len)
{
    int numbytes;
    struct packet_gen *pack = (struct packet_gen *)buf;
    /*if (pack->seq > 6200 && pack->seq < 6700 && !retransmit) {
        retransmit = 0;
        return len;
    }*/
	if ((numbytes = mp3_sendto(send_udp_port, buf, len, 0, &rem_addr, sizeof(rem_addr))) == -1) {
		handle_error("tx_generic_udp_send_pkt: sendto");
	}
    return numbytes;
}

static void
retransmit_general (struct tcp_conn *tcp)
{
    struct pack_header* temp_hdr = NULL;
    uint32_t seq;
    uint16_t len;
    struct packet_gen buf = {0};
    int numbytes;

    temp_hdr = (struct pack_header*)&tcp->pack_array[(tcp->array_last)];
    seq = temp_hdr->seq;
    len = temp_hdr->seq_end;
    buf.opcode = DATA;
    //updating the pack seq to last sent + 1
    buf.seq = one_tcp->last_ack;
    buf.seq_off = get_file_data(buf.buf, buf.seq, LEN_DATA);
    retransmit = 1;
    numbytes = tx_udp_packet((char *)&buf, sizeof(struct packet_gen));
    printf("\nOUT:%5u    DATA:%5u:%5u  %s    CWND:%5u    SSTHRESH:%5u",
           one_tcp->unack, buf.seq, (buf.seq_off + buf.seq + 1),
           phase_string[one_tcp->phase], one_tcp->cwnd, one_tcp->ssthresh);
}


void
handle_slows_conversion (void)
{
    one_tcp->phase = SLOWS;
    one_tcp->ssthresh = (((one_tcp->cwnd)/2) > (2*MAXDATASIZE) ?
                        (one_tcp->cwnd/2) : (2*MAXDATASIZE));
    one_tcp->ssthresh -= one_tcp->ssthresh % MAXDATASIZE;
    one_tcp->cwnd = MAXDATASIZE;
    retransmit_general(one_tcp);
    one_tcp->duplicate_ack = 0;
}

static void
periodic_timestamp_check (void)
{
    struct tcp_conn *tcp = one_tcp;
    int temp_index = tcp->array_last;
    uint64_t time_now;
    uint32_t time_diff;
    if (tcp->array_first == temp_index) {
        if (pa_seq(PAC(tcp->array_last)) == 0)
            return;
    }

    time_now = get_elapsed_time(0);
    while (temp_index <= tcp->array_first) {
        //printf("\nRX: Checked timestamp at %u", temp_index);
        time_diff = time_now - pa_time(PAC(tcp->array_last));
        if (time_diff > 6*one_tcp->rtt) {
            printf("\nTIMEOUT      DATA:%5u    RTT%lu",
                   pa_seq(PAC(tcp->array_last)), one_tcp->rtt);
            handle_slows_conversion();
            return;
        }
        temp_index = (temp_index + 1) % tcp->array_max_length;
    }
}

void
rtt_check_tcp (void)
{
    static uint64_t time_first = 0;

    if (one_tcp->rtt == 1000000) {
        if (time_first == 0) {
            time_first = get_elapsed_time(0);
        } else {
            one_tcp->rtt = get_elapsed_time(0) - time_first;
            setup_timer();
        }
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
             handle_error("Could not get hostname");
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
file_setup (char *buf) {
    if(!(fp=fopen(buf, "r"))) {
        printf("\nCouldn't find entry file");
        return 0;
    } else {
        return 1;
    }
}

void
send_data_upto_cwnd (struct packet_gen *buf)
{
    int numbytes, i;

    while (one_tcp->unack < one_tcp->cwnd) {
		buf->opcode = DATA;
		//updating the pack seq to last sent + 1
		buf->seq = one_tcp->last_sent_end + 1;
		//Updating the last sent beg now to the new packet
		one_tcp->last_sent_beg = buf->seq;
        buf->seq_off = get_file_data(buf->buf, buf->seq, LEN_DATA);
        if (buf->seq_off == 0) {
           ok_to_end = 1;
           break;
        }

        /*
        for (i = 0; i < LEN_DATA; i++) {
            putchar(buf->buf[i]);
        }
        */
        
		numbytes = tx_udp_packet((char *)buf, sizeof(struct packet_gen));
		one_tcp->last_sent_end = buf->seq_off + buf->seq;
		one_tcp->unack = (one_tcp->last_sent_end - one_tcp->last_ack + 1);
	
    	// Update the timestamp when sent
        update_timestamp_tx(one_tcp);
		
        printf("\nOUT:%5u    DATA:%5u:%5u  %s    CWND:%5u    SSTHRESH:%5u",
               one_tcp->unack, one_tcp->last_sent_beg, one_tcp->last_sent_end,
               phase_string[one_tcp->phase], one_tcp->cwnd, one_tcp->ssthresh);
	}
}

void
close_if_done (void)
{    
    struct packet_gen buf = {0};
    int i = 0;
    if (ok_to_end && (!one_tcp || !one_tcp->unack)) {
        buf.opcode = END;
        snprintf(buf.buf, 4, "BYE\n");
        while(i<4) {
            //Forcing close of remote, no ACKs expected, so trying to game
            //the channel
            sleep(1);
            tx_udp_packet((char *)&buf, sizeof(struct packet_gen));
            i++;
        }
        if (fp)
            fclose(fp);
        fflush(NULL);
        handle_error("\nClosing Done File Transfer, Acks received");
    }
}

void
handle_fasrec_conversion (void)
{
    one_tcp->phase = FAST_RECOVERY;
    one_tcp->ssthresh = (((one_tcp->cwnd)/2) > (2*MAXDATASIZE) ?
                        (one_tcp->cwnd/2) : (2*MAXDATASIZE));
    one_tcp->ssthresh -= one_tcp->ssthresh % MAXDATASIZE;
    one_tcp->cwnd = one_tcp->ssthresh + 3*MAXDATASIZE;
    retransmit_general(one_tcp);
    one_tcp->duplicate_ack = 0;
}

void
handle_ca_conversion (void)
{
    one_tcp->phase = CONG_AVOID;
}

void
process_cong_avoid(struct packet_gen *pack)
{
    struct packet_gen buf = {0};
    uint32_t numbytes = 0;
    int i;

    printf("\n\t ACK:%5u    %s", pack->seq, phase_string[one_tcp->phase]);

    if (pack->seq != one_tcp->last_ack) {
        //Reduce outstanding
        one_tcp->unack -= pack->seq  - one_tcp->last_ack;
        downgrade_timestamp_tx(one_tcp);
        close_if_done();
        one_tcp->last_ack = pack->seq;
    } else {
        one_tcp->duplicate_ack++;
        if (one_tcp->duplicate_ack == 3) {
            handle_fasrec_conversion();
            return;
        }
    } 

   // Update congestion window in a linear fashion
   one_tcp->cwnd += (MAXDATASIZE*MAXDATASIZE)/one_tcp->cwnd;
   //Send more data if possible
   send_data_upto_cwnd(&buf);
   return;
}


void
process_fast_recovery(struct packet_gen *pack)
{
 	struct packet_gen buf = {0};
	uint32_t numbytes = 0;
    int i;

    printf("\n\t ACK:%5u    PHASE:%s", pack->seq, phase_string[one_tcp->phase]);
	
    if (pack->seq != one_tcp->last_ack) {
        //Go to congestion avoidance, note when there is a dup ack seen on slow
        //start, here we come to fast rec, and on getting out of it, we go to CA mode.
        //This is signalled by ssthresh = 65535
        
        /*
         * Adjust the outstanding packet count, as now we can.
         */
        one_tcp->unack -= pack->seq  - one_tcp->last_ack;
        downgrade_timestamp_tx(one_tcp);
        close_if_done();

        one_tcp->last_ack = pack->seq;
        handle_ca_conversion();
        if (one_tcp->ssthresh == 65535) {
            one_tcp->ssthresh = one_tcp->cwnd;
        }
	} else {
        // Increase cwnd by 1MSS on receipt of every dup ack
        one_tcp->cwnd += sizeof(struct packet_gen);
    }
   //Send more data if window isnt full
   send_data_upto_cwnd(&buf);
   return;

}

void
process_slows_ack (struct packet_gen *pack)
{
	struct packet_gen buf = {0};
	uint32_t numbytes = 0;
    int i;

    if (one_tcp) {
    printf("\n\t ACK:%5u    PHASE:%s", pack->seq, phase_string[one_tcp->phase]);
    }

    if (pack->seq == 1) {
        /* 
         * Provision made for only one tcp connection, reuse if you already have
         * one.
         */
        one_tcp = new_tcp_conn(fdinfo[0].fd, fdinfo[1].fd, one_tcp);
        rtt_check_tcp();
    } else if (pack->seq == one_tcp->last_ack) {
        one_tcp->duplicate_ack++;
        if (one_tcp->duplicate_ack == 3) {
            handle_fasrec_conversion();
            return;
        }
    } else {
        rtt_check_tcp();
       //Reduce outstanding
		one_tcp->unack -= pack->seq  - one_tcp->last_ack;
        downgrade_timestamp_tx(one_tcp);

        close_if_done();
	}

	//printf("\nUnack packets reduced to %u", one_tcp->unack);
	one_tcp->last_ack = pack->seq;

	//update the cwnd whenever ACK is rx for slow start
    one_tcp->cwnd += sizeof(struct packet_gen);
    //Send data if window isnt full
    send_data_upto_cwnd(&buf);
    if (one_tcp->cwnd >= one_tcp->ssthresh) {
        handle_ca_conversion();
    }
    return;
}
 
void
process_ack (struct packet_gen *pack, int len)
{
    uint16_t element = SLOWS;
    if (one_tcp) {
        element = one_tcp->phase;
    }

    switch (element) {
        case SLOWS:
            process_slows_ack(pack);
        break;
        case CONG_AVOID:
            process_cong_avoid(pack);
        break;
        case FAST_RECOVERY:
            process_fast_recovery(pack);
        break;
        default:
        break;
    }
}

void
process_input_udp (int file)
{
    char buf[MAXDATASIZE] = {0};
    uint8_t opcode, numbytes;
 	socklen_t addr_len;
    struct sockaddr_storage their_addr;
	struct packet_gen *pack = NULL;
    int i;
	   
 	addr_len = sizeof their_addr;
	if ((numbytes = recvfrom(file, buf, MAXDATASIZE-1 , 0,
		(struct sockaddr *)&their_addr, &addr_len)) == -1) {
		handle_error("recvfrom");
	}
    if (numbytes == 0) {
        printf("\n Returning");
    }
	//printf("\nServer: packet is %d bytes long\n", numbytes);
    buf[numbytes] = '\0';

    pack = (struct packet_gen*)buf;

	if (pack->opcode == ACK) {
        process_ack(pack, numbytes);
    }
} 

void
process_timer (int file)
{
    uint64_t exp = 0;
    uint16_t s = 0;

    s = read(file, &exp, sizeof(uint64_t));
    if (s != sizeof(uint64_t))
        handle_error("read");

    if (one_tcp) {
        periodic_timestamp_check();
    }
}
  
void
process_input (int file)
{
    //printf("\n fdinfo 0 %d fdinfo 1 %d file %d", fdinfo[0].fd, fdinfo[1].fd, file);
    if (fdinfo[0].fd == file) {
        process_input_udp(file);
    } else {
        process_timer(file);
    }
}

/*
 * The function sets up files, sockets, timers and polls on them
 */
int main (int argc, char *argv[])
{
    int sockfd, res;
    int count = 0; //Describing the number of Fds to be polled
    struct addrinfo hints, *servinfo, *p;
    int rv, i;
    struct sockaddr_in sin = {0};
    if (argc != 5) {
        fprintf(stderr,"usage: mp3server remote_host remote_port myudp_port filename\n");
        exit(1);
    }

    if (!file_setup(argv[4])) {
        handle_error("Read file open");
    }

    strncpy(remote_hostname, argv[1], 100);
    //printf("\n Hostname: %s", remote_hostname);
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
    
    fdinfo[0].fd = sockfd;
    fdinfo[0].events = POLLIN;
    count++;
    
    fdinfo[1].fd = init_timer_setup();
    fdinfo[1].events = POLLIN;
    count++;

    i = fcntl(fdinfo[0].fd, F_SETFL, O_NONBLOCK);
    i = fcntl(fdinfo[1].fd, F_SETFL, O_NONBLOCK);
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
