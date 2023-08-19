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
#define 	LK_NAME		"DUAL SERVICIOS"

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
 * ID1234;001:Status;...;NNN:Status,Timestamp
 * 
 * Where NNN is the max number of lockers
 * 
 * This function write the result in a global array
 * called "ocp";
 */
char 	ocp[100] = "";
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

/* MQTT related stuff */

char 	cmd[10] = " ";
struct 	mosquitto *mosqg;
struct 	mosquitto *mosqc;

void on_connect(struct mosquitto *mosq, void *obj, int rc) 
{
	if(rc) {
		printf("Error with result code: %d\n", rc);
		exit(-1);
	}
	mosquitto_subscribe(mosq, NULL, "lkaas/cmd", 0);
}

void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {

	time_t ltime;
        ltime = time(NULL);

	printf("COMMAND:OPEN - LOCKER: %s - %s", (char *) msg->payload, asctime(localtime(&ltime)));
	strcpy(cmd, (char *) msg->payload);
}

/* 
 Periodic task to check locker status and publish to MQTT Topic
 also check for available commands to execute in the CMD topic
*/
void check_status(int sn)
{
	int fd;
    	uint32_t status;
    	char *portname = "/dev/ttyUSB0";
	int open_to = 0;
	
    	fd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);
    	if (fd < 0) {
        	printf("Error opening %s: %s\n", portname, strerror(errno));
        	exit(-1);
    	}
    	
    	set_serial(fd, B19200);
    	status = query_all(fd);
    	oc_payload(LK_NAME, status);
    
     
    	mosquitto_publish(mosqg, NULL, "lkaas/status", sizeof(ocp), ocp, 0, false);
	
	if (!strcmp(cmd, "ALL")) 
		open_all(fd);
	else {
		open_to = atoi((char *) cmd);
		open_one(fd, open_to);
	}

	strcpy(cmd, (char *) " ");
    	open_to = 0;
		
    	close(fd);
}

/* Prepare a timer for status check with "interval" as periodic time */
void set_timer(uint8_t interval)
{
	struct sigaction sa;
	struct itimerval timer;

	/* Install periodic_task  as the signal handler for SIGVTALRM. */
	memset (&sa, 0, sizeof (sa));
	sa.sa_handler = &check_status;
	sigaction (SIGVTALRM, &sa, NULL);

	/* Configure the timer to expire after one second */
	timer.it_value.tv_sec = interval;
	timer.it_value.tv_usec = 0;

	/* And every one second after that. */
	timer.it_interval.tv_sec = interval;
	timer.it_interval.tv_usec = 0;

	/* Start a virtual timer. */
	setitimer (ITIMER_VIRTUAL, &timer, NULL);
}

void ctrl_c(int sh) {
    	finalize = 1;
}

int main()
{ 
	int rc, rc1, id=12, id1=11;

	set_timer(1);
	signal(SIGINT, ctrl_c);
	
	mosquitto_lib_init();

	mosqg = mosquitto_new("lkaas-status", true, &id);
	rc = mosquitto_connect(mosqg, "localhost", 1883, 10);
	if(rc) {
		printf("Could not connect to Broker with return code %d\n", rc);
		return -1;
	}
	
	mosqc = mosquitto_new("lkaas-cmd", true, &id1);
	
	mosquitto_connect_callback_set(mosqc, on_connect);
	mosquitto_message_callback_set(mosqc, on_message);
	rc1 = mosquitto_connect(mosqc, "localhost", 1883, 10);
	if(rc1) {
		printf("Could not connect to Broker with return code %d\n", rc);
		return -1;
	}    
    
	mosquitto_loop_start(mosqc);
    	printf("Press Ctrl+C to quit...\n");
    
    	while (!finalize) {
		__asm__("NOP");
    	}
    
	mosquitto_loop_stop(mosqc, true);
	mosquitto_disconnect(mosqc);
	mosquitto_destroy(mosqc);
	
	mosquitto_disconnect(mosqg);
	mosquitto_destroy(mosqg);
	mosquitto_lib_cleanup();	
}
