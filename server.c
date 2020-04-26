#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>
#include <time.h>
#include <string.h>

#include "commons.h"

#define TMP_BUFFER_SIZE 4 /* in terms of number of packets */

/**
 * Generates a new packet to be sent to the server
 * @param seq_no        THe sequence no. of the packet to be
 *                      acknowledged
 * @param channel_no	The channel through which the packet
 * 						will be sent
 * 
 * @return A new packet
 */
Packet create_packet(unsigned int seq_no, int channel_no) {
	Packet pkt;
	pkt.seq_no = seq_no;
	pkt.payload_size = 0;
	pkt.channel_no = channel_no;
	pkt.is_last = 0;
	pkt.data_or_ack = 1; /* server always sends only ack pakcets */
	return pkt;
}

/**
 * Prints the trace of a packet ot the console
 * @param pkt	The packet whose trace is to be printed
 */
void print_packet(Packet* pkt) {
	switch(pkt->data_or_ack) {
		case 0: {
			/* data packet sent */
			printf("RCVD PKT: Seq No. %d of size %ld bytes via channel %d\n", pkt->seq_no, pkt->payload_size, pkt->channel_no);
		}
		break;
		case 1: {
			/* ack received */
			printf("SENT ACK: for PKT with Seq No. %d via channel %d\n", pkt->seq_no, pkt->channel_no);
		}
		break;
	}
}

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
 * randomly generates either 0, indicating accept, or 
 * generates 1, indicating drop. The rate is determined
 * by the rate specified in the macro PACKET_DROP_RATE
 */
int accept_or_drop() {
	int rand_till_100 = rand() % 100;
	return ((rand_till_100 < PACKET_DROP_RATE) ? 1 : 0);
}

/**
 * Writes all the packets in the buffer to an output file
 * and clears the buffer
 * @param fp			Pointer to the output file
 * @param buffer		The buffer storing packets (assumed
 * 						to be in sorted order)
 * @param buf_filled	No. of packets in theh buffer
 * 
 * @return The total data written
 */
int buffer_flush(FILE* fp, Packet* buffer, int* buf_filled, int expected_seq) {
	int i;
	/* flush buffer contents */
	for(i = 0; i < *buf_filled; i++) {
		if(buffer[i].seq_no == expected_seq) {
			fwrite(buffer[i].payload, 1, buffer[i].payload_size, fp);
			expected_seq += buffer[i].payload_size;
		}
	}
	
	/* compress buffer */
	for(int j = 0; j < *buf_filled - i; j++) {
		buffer[j] = buffer[i + j];
	}
	*buf_filled = *buf_filled - i;

	return expected_seq;
}

/**
 * Inserts a packet into the buffer in sorted order (assumes that the buffer
 * has sufficient capacity for one more element)
 * @param pkt		The packet to be inserted into the buffer
 * @param buffer	The buffer into which the packet is to be inserted
 * @param buf_size	The no. of elements already in the buffer
 */
void insert_packet_to_buffer(Packet pkt, Packet* buffer, int* buf_size) {
	int i = (*buf_size) - 1;
	while(i >= 0 && buffer[i].seq_no > pkt.seq_no) {
		buffer[i+1] = buffer[i];
		i--;
	}
	buffer[i+1] = pkt;
	(*buf_size) = (*buf_size) + 1;
}

int main() {
	/* set seed for random number generation */
	srand(time(0));

	/* create a socket */
	int listen_sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(listen_sock < 0) {
		report_error("Failed to create socket");
	}

	/* Create address structure */
	struct sockaddr_in server_addr;
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(SERVER_PORT);
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	/* Allow socket descriptor to be usable */
	int i = 1;
	if(setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*) &i, sizeof(i)) < 0) {
		report_error("Failed to make socket reusable");
	}

	/* binding server socket */
	if(bind(listen_sock, (struct sockaddr*) &server_addr, sizeof(server_addr)) < 0) {
		report_error("Failed to bind the socket");
	}

	int fds[2]; /* stores two channels */

	/* listen for two connections (mimics two channels) */
	for(i = 0; i < 2; i++) {
		if(listen(listen_sock, MAX_PENDING) < 0) {
			report_error("Failed to setup listening mode of socket");
		}
		if((fds[i] = accept(listen_sock, NULL, NULL)) < 0) {
			report_error("Failed to accept incoming connection");
		}
	}

	/* buffer for managing out-of-order packets */
	Packet buffer[TMP_BUFFER_SIZE];
	int expected_seq = 0;
	int buf_filled = 0;

	FILE* fptr = fopen("output.txt", "w");

	int is_last_ackd = 0;
	int last_seq = __INT_MAX__;

	while(is_last_ackd == 0) {
		fd_set read_fds;
		FD_ZERO(&read_fds);
		FD_SET(fds[0], &read_fds);
		FD_SET(fds[1], &read_fds);

		if(select(fds[1] + 1, &read_fds, NULL, NULL, NULL) <= 0) {
			report_error("Error occurred in select()");
		}
		/* select returned a positive value, some fd is set */
		if(FD_ISSET(fds[0], &read_fds)) {
			Packet pkt;
			memset(&pkt, 0, sizeof(Packet));
			int status;
			if((status = recv(fds[0], &pkt, sizeof(Packet), 0)) < 0) {
				report_error("Failed to receive packet");
			} else if(status == 0) {
				printf("Connection closed by client.\nTerminating program\n");
				exit(0);
			}

			if(accept_or_drop() != 1) {
				/* packet should not be dropped randomly */
				if(pkt.seq_no < expected_seq) {
					/* must be retransmitted packet due to timeout, but was ack'd. Drop silently */
				} else if(pkt.seq_no == expected_seq) {
					/* print trace of received packet */
					print_packet(&pkt);

					/* send acknowledgement */
					Packet ack = create_packet(pkt.seq_no, pkt.channel_no);
					
					if(pkt.is_last) {
						/* last packet received */
						last_seq = pkt.seq_no;
					}

					if(expected_seq >= last_seq) {
						/* all packets have been received by server */
						ack.is_last = 1;
						is_last_ackd = 1;
					}

					if(send(fds[0], &ack, sizeof(Packet), 0) < 0) {
						report_error("Failed to send acknowledgement");
					}

					/* print trace of sent acknowledgement */
					print_packet(&ack);

					/* write in-order packet to file */
					fwrite(pkt.payload, 1, pkt.payload_size, fptr);

					/* update expected sequence number for in-order packet */
					expected_seq += pkt.payload_size;

					/* write any out-of-order packets to file */
					expected_seq = buffer_flush(fptr, buffer, &buf_filled, expected_seq);
				} else if(buf_filled < TMP_BUFFER_SIZE) {
					/* out-of-order packet to be accepted */
					/* print trace of received packet */
					print_packet(&pkt);

					/* send acknowledgement */
					Packet ack = create_packet(pkt.seq_no, pkt.channel_no);

					if(pkt.is_last) {
						/* last packet received */
						ack.is_last = 1;
						is_last_ackd = 1;
					}

					if(send(fds[0], &ack, sizeof(Packet), 0) < 0) {
						report_error("Failed to send acknowledgement");
					}

					/* print trace of sent acknowledgement */
					print_packet(&ack);

					/* insert packet into buffer */
					insert_packet_to_buffer(pkt, buffer, &buf_filled);
				} /* otherwise drop packet due to filled buffer */
			} /* packet dropped randomly */
		}
		if(FD_ISSET(fds[1], &read_fds)) {
			Packet pkt;
			memset(&pkt, 0, sizeof(Packet));
			int status;
			if((status = recv(fds[1], &pkt, sizeof(Packet), 0)) < 0) {
				report_error("Failed to receive packet");
			} else if(status == 0) {
				printf("Connection closed by client.\nTerminating program\n");
				exit(0);
			}

			if(accept_or_drop() != 1) {
				/* packet should not be dropped randomly */
				if(pkt.seq_no < expected_seq) {
					/* must be retransmitted packet due to timeout, but was ack'd. Drop silently */
				} else if(pkt.seq_no == expected_seq) {
					/* print trace of received packet */
					print_packet(&pkt);

					/* send acknowledgement */
					Packet ack = create_packet(pkt.seq_no, pkt.channel_no);

					if(pkt.is_last) {
						/* last packet received */
						last_seq = pkt.seq_no;
					}

					if(expected_seq >= last_seq) {
						/* all packets have been received by server */
						ack.is_last = 1;
						is_last_ackd = 1;
					}

					if(send(fds[1], &ack, sizeof(Packet), 0) < 0) {
						report_error("Failed to send acknowledgement");
					}

					/* print trace of sent acknowledgement */
					print_packet(&ack);

					/* write in-order packet to file */
					fwrite(pkt.payload, 1, pkt.payload_size, fptr);

					/* update expected sequence number for in-order packet */
					expected_seq += pkt.payload_size;

					/* write any out-of-order packets to file */
					expected_seq = buffer_flush(fptr, buffer, &buf_filled, expected_seq);
				} else if(buf_filled < TMP_BUFFER_SIZE) {
					/* out-of-order packet to be accepted */
					/* print trace of received packet */
					print_packet(&pkt);

					/* send acknowledgement */
					Packet ack = create_packet(pkt.seq_no, pkt.channel_no);

					if(pkt.is_last) {
						/* last packet received */
						ack.is_last = 1;
						is_last_ackd = 1;
					}

					if(send(fds[1], &ack, sizeof(Packet), 0) < 0) {
						report_error("Failed to send acknowledgement");
					}

					/* print trace of sent acknowledgement */
					print_packet(&ack);

					/* insert packet into buffer */
					insert_packet_to_buffer(pkt, buffer, &buf_filled);
				} /* otherwise drop packet due to filled buffer */
			} /* packet dropped randomly */
		}
	}

	fclose(fptr);
	printf("\nFile received successfully, stored as output.txt\n");
	return 0;
}