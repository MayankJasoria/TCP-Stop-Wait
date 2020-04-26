#define SERVER_IP "127.0.0.1" /* Using loopback address for simplicity */

#define PACKET_SIZE 100 /* in bytes */
#define RETRANSMISSION_TIMEOUT 2 /* seconds */
#define MAX_RETRIES 10 /* if exceeded, assume channel has been broken */

#define SERVER_PORT 12500

#define PACKET_DROP_RATE 10

#define MAX_PENDING 5

typedef struct packet {
	size_t payload_size;
	unsigned int seq_no;
	unsigned int data_or_ack : 1; /* 0 -> data, 1-> ack */
	unsigned int channel_no : 1; /* 0 or 1 according to the channel used */
	unsigned int is_last : 1; /* 0 -> not last, 1 -> last */
	char payload[PACKET_SIZE]; /* actual data payload */
} Packet;