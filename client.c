#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include "commons.h"

/**
 * Function to report an error and terminate the program
 * @param str	The error message
 */
void report_error(char* str) {
	perror(str);
	printf("Terminating program\n");
	exit(0);
}

/**
 * Returns the channel on which the timeout is smallest
 * @param c0_sec	The total seconds left for channel 0 to timeout
 * @param c0_usec	The total micro-seconds left for channel 0 to timeout
 * @param c1_sec	The total seconds left for channel 1 to timeout
 * @param c1_usec	The total micro-seconds left for channel 1 to timeout
 * 
 * @return	The channel number (0 or 1) which has smallest timeout left
 */
int min_time_channel(size_t c0_sec, size_t c0_usec, size_t c1_sec, size_t c1_usec) {
	size_t c0_time = c0_sec * CLOCKS_PER_SEC + c0_usec;
	size_t c1_time = c1_sec * CLOCKS_PER_SEC + c1_usec;
	if(c0_time < c1_time) {
		return 0;
	} else if(c0_time > c1_time) {
		return 1;
	} else {
		return rand()%2; /* since both are same, return any one */
	}
}

/**
 * Creates a new connection with the server
 * @param port_num	The port number 
 */
int create_connection() {
	/* Create a socket */
	int sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(sock < 0) {
		report_error("Failed to create socket");
		
	}

	/* Construct server address structure */
	struct sockaddr_in serv_addr;
	memset(&serv_addr, 0, sizeof(struct sockaddr_in));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
	serv_addr.sin_port = htons(SERVER_PORT);

	/* Make socket non-blocking */
	// if(fcntl(sock, F_SETFL, (fcntl(sock, F_GETFL, 0) | O_NONBLOCK)) < 0) {
	// 	report_error("Could not make socket nonblocking");
	// }

	/* Establish connection */
	if(connect(sock, (struct sockaddr*) &serv_addr, sizeof(struct sockaddr)) < 0) {
		report_error("Could not establish connection with server");
		
	}

	return sock;
}

/**
 * Generates a new packet to be sent to the server
 * @param fptr			The input file pointer
 * @param channel_no	The channel through which the packet
 * 						will be sent
 * 
 * @return A new packet
 */
Packet create_packet(FILE* fptr, int channel_no) {
	Packet pkt;
	pkt.seq_no = ftell(fptr);
	pkt.payload_size = fread(pkt.payload, 1, PACKET_SIZE, fptr);
	pkt.channel_no = channel_no;
	pkt.is_last = ((pkt.payload_size < PACKET_SIZE) ? 1 : 0); /* Last pakcet if less than required no. of bytes read */
	pkt.data_or_ack = 0; /* client always sends only data pakcets */
	return pkt;
}

/**
 * Computes the time left for a channel to timeout. Value-result type implementation,
 * thus values of input parameters will be updated to reflect the results
 * @param sec	The total seconds that were initially left
 * @param usec	The total micro-seconds that were initially left
 */
void time_left(size_t* sec, size_t* usec, double time_taken) {
	double remaining_time = *sec + (double) *usec / CLOCKS_PER_SEC;
	remaining_time -= time_taken;
	long remaining_sec = (long) remaining_time;
	long remaining_usec = (long) ((remaining_time - (double) remaining_sec) * CLOCKS_PER_SEC);
	if(remaining_sec < 0 || remaining_usec < 0) {
		*sec = 0;
		*usec = 1; /* force instant timeout at select */
	} else {
		*sec = (size_t) remaining_sec;
		*usec = (size_t) remaining_usec;
	}
}

/**
 * Prints the trace of a packet ot the console
 * @param pkt	The packet whose trace is to be printed
 */
void print_packet(Packet* pkt) {
	switch(pkt->data_or_ack) {
		case 0: {
			/* data packet sent */
			printf("SENT PKT: Seq No. %d of size %ld bytes via channel %d\n", pkt->seq_no, pkt->payload_size, pkt->channel_no);
		}
		break;
		case 1: {
			/* ack received */
			printf("RCVD ACK: for PKT with Seq No. %d via channel %d\n", pkt->seq_no, pkt->channel_no);
		}
		break;
	}
}

int main() {
	int fds[2];
	int max_fd;

	/* Creating the channels for communicating with server */
	fds[0] = create_connection();
	fds[1] = create_connection();
	max_fd = fds[1] + 1;

	/* opening the file to be read */
	FILE* fptr = fopen("input.txt", "r");
	if(fptr == NULL) {
		report_error("The requested file could not be opened");
	}
	fseek(fptr, 0, SEEK_SET);

	clock_t start, end;

	/* used to maintain timer of each packet (one for each channel) */
	size_t time_left_0_sec = RETRANSMISSION_TIMEOUT;
	size_t time_left_0_usec = RETRANSMISSION_TIMEOUT;
	size_t time_left_1_sec = 0;
	size_t time_left_1_usec = 0;
	int min_time_sock = 0;

	/* used to maintain no. of retransmissions of each packet (-1 = channel closed) */
	int pkt0_trans_count = 0;
	int pkt1_trans_count = 0;

	/*
	 * used to maintain state of a packet:
	 * 0 -> send new packet
	 * 1 -> wiat for ACK or Timeout, act accordingly
	 * 2 -> no more packets to transmit on current channel
	 */
	int state_ch0 = 0;
	int state_ch1 = 0;

	/* generate first packets of each channel */
	Packet ch0_pkt = create_packet(fptr, 0);
	Packet ch1_pkt;

	/* send first packet of each channel */
	if(send(fds[0], &ch0_pkt, sizeof(Packet), 0) < 0) {
		report_error("Failed to perform send()");
	}
	pkt0_trans_count++;
	state_ch0 = 1;
	print_packet(&ch0_pkt);

	if(ch0_pkt.is_last) {
		state_ch0 = 2;
		fclose(fptr);
	}

	if(state_ch0 != 2) {
		ch1_pkt = create_packet(fptr, 1);
		/* File requires more than 1 packet */
		if(send(fds[1], &ch1_pkt, sizeof(Packet), 0) < 0) {
			report_error("Failed to perform send()");
		}
		pkt1_trans_count++;
		state_ch1 = 1;
		print_packet(&ch1_pkt);
	} else {
		/* last packet already sent */
		time_left_1_sec = __LONG_LONG_MAX__;
		state_ch1 = 2;
	}

	while(state_ch0 + state_ch1 < 4) {
		/* preparing FD_SET for select() */
		fd_set read_fds;
		FD_ZERO(&read_fds);
		int i;
		for(i = 0; i < 2; i++) {
			FD_SET(fds[i], &read_fds);
		}

		struct timeval timeout;
		min_time_sock = min_time_channel(time_left_0_sec, time_left_0_usec, time_left_1_sec, time_left_1_usec);

		if(min_time_sock == 0) {
			timeout.tv_sec = time_left_0_sec;
			timeout.tv_usec = time_left_0_usec;
		} else {
			timeout.tv_sec = time_left_1_sec;
			timeout.tv_usec - time_left_1_usec;
		}

		/* noting time before select call */
		clock_t start = clock();

		/* select call */
		int num_ready = select(max_fd, &read_fds, NULL, NULL, &timeout);

		/* noting time after select call (ignoring time taken for computation for simplicity) */
		clock_t end = clock();

		double time_taken = (end - start) / (double) CLOCKS_PER_SEC;

		int is_ch0_updated = 0;
		int is_ch1_updated = 0;

		if(num_ready == 0) {
			/* timeout occurred */
			if(min_time_sock == 0) {
				/* channel 0 timeout */
				if(pkt0_trans_count >= MAX_RETRIES) {
					/* assume channel broken */
					fprintf(stderr, "Failed to transmit file due to exceeded max retries. Terminating Program\n");
					close(fds[0]);
					close(fds[1]);
					exit(0);
				} else {
					/* retransmit packet */
					if(send(fds[0], &ch0_pkt, sizeof(Packet), 0) < 0) {
						report_error("Failed to perform send()");
					}
					pkt0_trans_count++;
					state_ch0 = 1;
					/* print trace of packet */
					print_packet(&ch0_pkt);

					/* recompute timers */
					time_left_0_sec = RETRANSMISSION_TIMEOUT;
					time_left_0_usec = 0;
					is_ch0_updated = 1;
				}
			} else {
				/* channel 1 timeout */
				if(pkt1_trans_count >= MAX_RETRIES) {
					/* assume channel broken */
					fprintf(stderr, "Failed to transmit file due to exceeded max retries. Terminating Program\n");
					close(fds[0]);
					close(fds[1]);
					exit(0);
				} else {
					/* retransmit packet */
					if(send(fds[1], &ch1_pkt, sizeof(Packet), 0) < 0) {
						report_error("Failed to perform send()");
					}
					pkt1_trans_count++;
					state_ch1 = 1;

					/* print trace of packet */
					print_packet(&ch1_pkt);

					/* recompute timers */
					time_left_1_sec = RETRANSMISSION_TIMEOUT;
					time_left_1_usec = 0;
					is_ch1_updated = 1;
				}
			}
		} else {
			/* FD_ISSET check to identify which fd has received ack */
			if(FD_ISSET(fds[0], &read_fds)) {
				/* receive the ack */
				if(recv(fds[0], &ch0_pkt, sizeof(Packet), 0) < 0) {
					report_error("Failed to receive");
				}

				/* print the acknowledgement trace */
				print_packet(&ch0_pkt);

				if(state_ch0 != 2 && state_ch1 != 2) {
					/* last packet not yet sent */
					state_ch0 = 0;
					pkt0_trans_count = 0;

					/* generate new packet for channel 0 */
					ch0_pkt = create_packet(fptr, 0);

					/* send the new packet */
					if(send(fds[0], &ch0_pkt, sizeof(Packet), 0) < 0) {
						report_error("Failed to send packet");
					}
					state_ch0 = 1;
					pkt0_trans_count++;

					/* print the newly transmitted packet */
					print_packet(&ch0_pkt);

					/* update timer */
					time_left_0_sec = RETRANSMISSION_TIMEOUT;
					time_left_0_usec = 0;
					is_ch0_updated = 1;

					if(ch0_pkt.is_last) {
						/* last packet, perform cleanup */
						state_ch0 = 2;
						fclose(fptr);
					}
				} else {
					time_left_0_sec = __LONG_LONG_MAX__;
					state_ch0 = 2;
				}
			}

			if(FD_ISSET(fds[1], &read_fds)) {
				/* receive the ack */
				if(recv(fds[1], &ch1_pkt, sizeof(Packet), 0) < 0) {
					report_error("Failed to receive");
				}

				/* print the acknowledgement trace */
				print_packet(&ch1_pkt);

				if(state_ch0 != 2 && state_ch1 != 2) {
					/* last packet not yet sent */
					state_ch1 = 0;
					pkt1_trans_count = 0;

					/* generate new packet for channel 0 */
					ch1_pkt = create_packet(fptr, 1);

					/* send the new packet */
					if(send(fds[1], &ch1_pkt, sizeof(Packet), 0) < 0) {
						report_error("Failed to send packet");
					}
					state_ch1 = 1;
					pkt1_trans_count++;

					/* print the newly transmitted packet */
					print_packet(&ch1_pkt);

					/* update timer */
					time_left_1_sec = RETRANSMISSION_TIMEOUT;
					time_left_1_usec = 0;
					is_ch1_updated = 1;

					if(ch1_pkt.is_last) {
						/* last packet, perform cleanup */
						state_ch1 = 2;
						fclose(fptr);
					}
				} else {
					time_left_1_sec = __LONG_LONG_MAX__;
					state_ch1 = 2;
				}
			}
		}

		/* recompute timers */
		if(is_ch0_updated == 0) {
			time_left(&time_left_0_sec, &time_left_0_usec, time_taken);
		}

		if(is_ch1_updated == 0) {
			time_left(&time_left_1_sec, &time_left_1_usec, time_taken);
		}

		// printf("Time remaining\nPacket 0: %ld + %ld * 10^-6\nPacket 1: %ld + %ld * 10^-6\n", time_left_0_sec, time_left_0_usec, time_left_1_sec, time_left_1_usec);
	}

	close(fds[0]);
	close(fds[1]);

	printf("\nFile transfer completed successfully\n");

	return 0;
}