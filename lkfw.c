#include 	<signal.h>
#include 	<stdio.h>
#include 	<string.h>
#include 	<errno.h>
#include 	<fcntl.h> 
#include 	<stdlib.h>
#include 	<termios.h>
#include 	<unistd.h>
#include 	<stdint.h>
#include 	<sys/time.h>
#include 	<mosquitto.h>
#include 	<time.h>

/* CU16 related macros */
#define 	CMD_LEN 	5
#define 	RES_LEN 	9
#define 	OPEN_CMD 	0x31
#define		QUERY_CMD	0x32
#define		OPEN_ALL	0x33
#define 	LK_NAME		"LKAAS_LOCKERS"

/* MQTT related stuff */

#define		BROKER_URL	"95efa85031e142e69db9bbb994342b28.s1.eu.hivemq.cloud"
#define		BROKER_PORT	8883
#define		BROKER_USER	"devopstrends"
#define		BROKER_PASS	"C2g2d0s2012"
#define		CA_CERT		"isrgrootx1.pem"
#define		CA_PATH		"/home/user/lkfw/"
#define		KEEP_ALIVE	10

/* Sequence to open the lockers based on address */
uint8_t cmd_one [CMD_LEN] =  { 
		0x02, /* Fixed value 0x02 */ 
		0x00, /* Locker address 0x00 == #1 */
		0x31, /* Command to open or query */
		0x03, /* Fixed value 0x03 */
		0x00  /* Checksum (low byte) for the values above */
	};

/* Sequence to query all lockers status */
uint8_t query [CMD_LEN] = { 0x02, 0x00, 0x32, 0x03, 0x37};

/* Sequence to open all lockers */
uint8_t oall [CMD_LEN] = {0x02, 0x00, 0x33, 0x03, 0x38};

/* Response buffer for lockers status query */
uint8_t response [RES_LEN] = {};

/* Flag to finalize an clear all */
uint8_t finalize = 0;

/* Configure serial port */
int set_serial(int fd, int speed) 
{
   	struct termios tty;	
	

    	cfsetospeed(&tty, (speed_t)speed);
    	cfsetispeed(&tty, (speed_t)speed);

    	tty.c_cflag |= (CLOCAL | CREAD);    /* ignore modem controls */
    	tty.c_cflag &= ~CSIZE;
    	tty.c_cflag |= CS8;         /* 8-bit characters */
    	tty.c_cflag &= ~PARENB;     /* no parity bit */
    	tty.c_cflag &= ~CSTOPB;     /* only need 1 stop bit */
    	tty.c_cflag &= ~CRTSCTS;    /* no hardware flowcontrol */

    	/* setup for non-canonical mode */
    	tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR 
					| IGNCR | ICRNL | IXON);
    	tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    	tty.c_oflag &= ~OPOST;

    	/* fetch bytes as they become available */
    	tty.c_cc[VMIN] = 1;
    	tty.c_cc[VTIME] = 1;

    	if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        	printf("Error from tcsetattr: %s\n", strerror(errno));
        	return -1;
    	}	
    
    	return 0;
}

/* Open all lockers sequentially */
void open_all(int fd) 
{
	int wlen;
	
	wlen = write(fd, oall, CMD_LEN);
    	if (wlen != CMD_LEN) {
        	printf("Error from open_all: %d, %d\n", CMD_LEN, errno);
    	}
}

/* Open one locker by his number */
void open_one(int fd, uint8_t number)
{
	int wlen;
	
	uint8_t cmd_base = 0x36;
	uint16_t checksum;
	
	cmd_one[1] = number - 1;
	checksum = cmd_base + cmd_one[1];       // Calculate checksum
	cmd_one[4] = (uint8_t) checksum & 0xFF; // Get the lower byte
	
	wlen = write(fd, cmd_one, CMD_LEN);
    	if (wlen != CMD_LEN) {
        	printf("Error from open_one: %d, %d\n", CMD_LEN, errno);
    	}
}

/* 
Returns all lockers and infrared status
- Bit order (reverse) corresponds to locker number
- First two bytes are for lock status
- Second two bytes are for infrared
*/
uint32_t query_all(int fd)
{
	int wlen, rdlen;
	uint32_t status = 0;
	
	wlen = write(fd, query, CMD_LEN);
    	if (wlen != CMD_LEN) {
        	printf("Error from query_all: %d, %d\n", CMD_LEN, errno);
    	}	
    
	rdlen = read(fd, response, RES_LEN);
	if (rdlen <= 0) { 
		printf("Error from query_read: %d: %s\n", rdlen, strerror(errno));
	} else {
		status = response[6] | 
			(response[5] << 8) | 
			(response[4] << 16) |
			(response[3] << 24);
	}
		
	return status;
}

/* Create payload in ASCII to publish on MQTT topic 
 * Statuses are:
 * 
 * OE - Empty and Opened
 * CE - Empty and Closed
 * OL - Loaded and Opened
 * CL - Loaded and Closed
 * 
 * The frame format is:
 * 
 * Timestamp,ID1234,Status001,...,StatusNNN
 * 
 * Where: 
 *
 * - Status001 to StatusNNN is the locker's number
 * - ID1234 is the locker wall id
 *
 * This function write the result in a global array
 * called "ocp";
 */
char 	ocp[86] = "";
void oc_payload(char *id, uint32_t status)
{
	
	uint16_t oc = status >> 16;
	uint16_t ir = status & 0xFFFF;
	
	uint8_t ocb1 = oc >> 8;
	uint8_t ocb2 = oc & 0xFF;
	
	uint8_t irb1 = ir >> 8;
	uint8_t irb2 = ir & 0xFF;

	time_t ltime;
	ltime = time(NULL);
	
	strcpy(ocp,asctime(localtime(&ltime)));
    	ocp[strlen(ocp)-1] = ',';
    	strcat(ocp,id);
    
    	int i;
	
	for (i = 1; i < 0x100; i <<= 1) {
		strcat(ocp,",");
		(ocb1 & i) ? strcat(ocp, "C") : strcat(ocp, "O");
		(irb1 & i) ? strcat(ocp, "L") : strcat(ocp, "E");
	}
	
	for (i = 1; i < 0x100; i <<= 1) {
		strcat(ocp,",");
		(ocb2 & i) ? strcat(ocp, "C") : strcat(ocp, "O");
		(irb2 & i) ? strcat(ocp, "L") : strcat(ocp, "E");
	}
}

/* MQTT lambdas and serial port variables */

char 	cmd[10] = " ";
struct 	mosquitto *mosqg;
int	gfd;

/* Connect and subscribe */
void on_connect(struct mosquitto *mosq, void *obj, int rc) 
{
	if(rc) {
		printf("Error with result code: %d\n", rc);
		exit(-1);
	}
	mosquitto_subscribe(mosq, NULL, "lkaas/cmd", 2);
}

/* Reacts to a message, execute the command and publish the event */
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {

	time_t ltime;
        ltime = time(NULL);
	char cmd_event[48] = "";
	int open_to = 0;

	sprintf(cmd_event, "COMMAND:OPEN,LOCKER:%s,%s", (char *) msg->payload, asctime(localtime(&ltime)));
	cmd_event[strlen(cmd_event) - 1] = '\0';

	strcpy(cmd, (char *) msg->payload);
	
	if (!strcmp(cmd, "ALL")) 
		open_all(gfd);
	else {
		open_to = atoi((char *) cmd);
		open_one(gfd, open_to);
	}

	mosquitto_publish(mosqg, NULL, "lkaas/events", sizeof(cmd_event), cmd_event, 2, false);
}

/* 
 Periodic task to check all locker status and publish to MQTT Topic
 called via main while cycle 
*/
void check_status(int fd)
{
    	uint32_t status;
	
	status = query_all(fd);
    	oc_payload(LK_NAME, status);
     
    	mosquitto_publish(mosqg, NULL, "lkaas/status", sizeof(ocp), ocp, 2, false);	
}

void ctrl_c(int sh) {
    	finalize = 1;
}

int main()
{ 
	int rc, id=12;
	char *portname = "/dev/ttyUSB0";
	
	signal(SIGINT, ctrl_c);

	gfd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);
    	if (gfd < 0) {
        	printf("Error opening %s: %s\n", portname, strerror(errno));
        	exit(-1);
    	}
    	
    	set_serial(gfd, B19200);
		
	mosquitto_lib_init();

	mosqg = mosquitto_new("lkaas-status", true, &id);
	
	mosquitto_username_pw_set(mosqg, BROKER_USER, BROKER_PASS);	
	mosquitto_tls_set(mosqg, CA_CERT, CA_PATH, NULL, NULL, NULL);	

	mosquitto_connect_callback_set(mosqg, on_connect);
	mosquitto_message_callback_set(mosqg, on_message);

	rc = mosquitto_connect(mosqg, BROKER_URL, BROKER_PORT, KEEP_ALIVE);
	if(rc) {
		printf("Could not connect to Broker with return code %d\n", rc);
		return -1;
	}
	
		
	mosquitto_loop_start(mosqg);
    	printf("Press Ctrl+C to quit...\n");
   
        FILE* pid;
       
	pid = fopen("/tmp/lkfw.pid", "w");

	if (pid == NULL) {
		printf("Can not create pid file");
		return -5;
	}

	fprintf(pid, "%d\n", (int) getpid());
	fclose(pid);
	
	
    	while (!finalize) {
		check_status(gfd);
		sleep(1);
    	}

	mosquitto_disconnect(mosqg);
	mosquitto_destroy(mosqg);
	mosquitto_lib_cleanup();
	
	close(gfd);	
}
