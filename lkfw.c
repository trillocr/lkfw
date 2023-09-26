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
#include	<confuse.h>

/* CU16 related macros */
#define 	CMD_LEN 	5
#define 	RES_LEN 	9
#define 	OPEN_CMD 	0x31
#define		QUERY_CMD	0x32
#define		OPEN_ALL	0x33

static char	*locker_name = NULL;
static int	broker_port = 0;
static char 	*broker_user = NULL;
static char 	*broker_pass = NULL;
static int	keep_alive = 0;
static int	post_seconds = 0;
static char	*broker_url = NULL;
static char	*ca_cert = NULL;
static char	*ca_path = NULL;
static char	*serial_port = NULL;
static char	*pid_file = NULL;

cfg_opt_t opts[] = {
	CFG_SIMPLE_STR("locker_name", &locker_name),
	CFG_SIMPLE_INT("broker_port", (long int *) &broker_port),
	CFG_SIMPLE_STR("broker_user", &broker_user),
	CFG_SIMPLE_STR("broker_pass", &broker_pass),
	CFG_SIMPLE_INT("keep_alive", (long int *) &keep_alive),
	CFG_SIMPLE_INT("post_seconds", (long int *) &post_seconds),
	CFG_SIMPLE_STR("broker_url", &broker_url),
	CFG_SIMPLE_STR("ca_cert", &ca_cert),
	CFG_SIMPLE_STR("ca_path", &ca_path),
	CFG_SIMPLE_STR("serial_port", &serial_port),
	CFG_SIMPLE_STR("pid_file", &pid_file),
	CFG_END()
};

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

/* MQTT global variables */
char 	cmd[10] = " ";
struct 	mosquitto *mosqg;
int	gfd;

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
 * This function write the result in a global string 
 * called "ocp";
 */
char ocp[128] = " ";
char st_topic[32] = "";
char ev_topic[32] = ""; 
char cmd_topic[32] = "";

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
	
	sprintf(ocp, "%s", asctime(localtime(&ltime)));
    	ocp[strlen(ocp) - 1] = ',';
    	strcat(ocp, id);
    
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
//	ocp[strlen(ocp)] = '\0';
}

/* Connect and subscribe */
void on_connect(struct mosquitto *mosq, void *obj, int rc) 
{
	if(rc) {
		printf("Error with result code: %d\n", rc);
		exit(-1);
	}
	mosquitto_subscribe(mosq, NULL, cmd_topic, 2);
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

	mosquitto_publish(mosqg, NULL, ev_topic, sizeof(cmd_event), cmd_event, 2, false);
}

/* 
 Periodic task to check all locker status and publish to MQTT Topic
 called via main while cycle 
*/
void check_status(int fd)
{
	uint32_t status;
	
	status = query_all(fd);
	oc_payload(locker_name, status);
     
   	mosquitto_publish(mosqg, NULL, st_topic, sizeof(ocp), ocp, 2, false);	
}

void ctrl_c(int sh) {
    	finalize = 1;
}

int main()
{ 
	int rc, id=12;
	
	cfg_t *cfg;
	
#ifdef LC_MESSAGES
	setlocale(LC_MESSAGES, "");
	setlocale(LC_CTYPE, "");
#endif

	cfg = cfg_init(opts, 0);
	cfg_parse(cfg, "lkfw.conf");

	sprintf(st_topic, "%s/status", locker_name);
	sprintf(ev_topic, "%s/events", locker_name);
	sprintf(cmd_topic, "%s/cmd", locker_name);

	signal(SIGINT, ctrl_c);


	gfd = open(serial_port, O_RDWR | O_NOCTTY | O_SYNC);
    	if (gfd < 0) {
        	printf("Error opening %s: %s\n", serial_port, strerror(errno));
        	exit(-1);
    	}
    	
    	set_serial(gfd, B19200);
		
	mosquitto_lib_init();

	mosqg = mosquitto_new("locker-status", true, &id);
	
	mosquitto_username_pw_set(mosqg, broker_user, broker_pass);	
	mosquitto_tls_set(mosqg, ca_cert, ca_path, NULL, NULL, NULL);	

	mosquitto_connect_callback_set(mosqg, on_connect);
	mosquitto_message_callback_set(mosqg, on_message);

	rc = mosquitto_connect(mosqg, broker_url, broker_port, keep_alive);
	if(rc) {
		printf("Could not connect to Broker with return code %d\n", rc);
		return -1;
	}
	
		
	mosquitto_loop_start(mosqg);
    	printf("Press Ctrl+C to quit...\n");
   
        FILE* pid;
       
	pid = fopen(pid_file, "w");

	if (pid == NULL) {
		printf("Can not create pid file");
		return -5;
	}

	fprintf(pid, "%d\n", (int) getpid());
	fclose(pid);

	while (!finalize) {
		check_status(gfd);
		sleep(post_seconds);
    	}

	mosquitto_disconnect(mosqg);
	mosquitto_destroy(mosqg);
	mosquitto_lib_cleanup();
	
	close(gfd);	

	free(locker_name);
	free(broker_user);
	free(broker_pass);
	free(broker_url);
	free(ca_cert);
	free(ca_path);
	free(serial_port);
	free(pid_file);
}
