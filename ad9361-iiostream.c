#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <iio.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <pthread.h>

#define ECHOMAX 255     /* Longest string to echo */
/* helper macros */
#define MHZ(x) ((long long)(x*1000000.0 + .5))
#define GHZ(x) ((long long)(x*1000000000.0 + .5))
#define BUFF_SIZE 1024*1024

void DieWithError(char *errorMessage)
{
    perror(errorMessage);
    exit(1);
}
int verbose=0;
sig_atomic_t abtomic=0;
long long int sendcount=0;
long long int recvcount=0;
	struct sockaddr_in fromAddr;     	/* Source address of echo */
	unsigned int fromSize;           	/* In-out of address size for recvfrom() */
int sock,sock2 , tcpsock;			/* Socket descriptor */
int rsplen ,sndlen,sndlen2;               	/* Length of received response */
//	Buffer
int buff_from_a[BUFF_SIZE]={0};
int buff_from_n[BUFF_SIZE]={0};
	struct timeval tv;


void *send_msg1(void *vargp){
	int i;
	for(i=0;i<32;i++){
        	sndlen = send(sock,buff_from_a+(1024*32*i), (1024*32), 0);
		sendcount=sendcount+sndlen;
        }
       	//printf("\rSend %d",sndlen);
	pthread_exit ((void*)0);
return NULL;
}


void *send_msg2(void *vargp){
	int i; 
	for(i=16;i<32;i++){
        	sndlen2 = send(sock2,buff_from_a+(1024*32*17), (1024*32), 0);
		sendcount=sendcount+sndlen2;
        }
       	//printf("\r\t\tSend2 %d",sndlen2);
	pthread_exit ((void*)0);
return NULL;
}

void *recv_msg(void *vargp){
	int i;
    	fromSize = sizeof(fromAddr);
	/*Set waiting limit*/
	tv.tv_sec = 2;   
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) DieWithError("Error");
	/* Connecting to the server */
	if (connect(sock,(struct sockaddr *)&fromAddr, sizeof(fromAddr)) < 0) DieWithError("socket() failed");
	for(i=0;i<32;i++){
		rsplen = recv(sock,buff_from_n,1024*32, 0);
		if(errno==EAGAIN) { setsockopt(sock, SOL_SOCKET, 0,&tv,sizeof(tv));
	        	printf("(Timeout is reached)");  pthread_exit ((void*)1); }

		if(rsplen< 0)  DieWithError("recvfrom() failed");
		recvcount=recvcount+rsplen;
		}
//        printf("\n\t\t\t\t\tRecv %lld kB\n",recvcount/1024/1024);
	pthread_exit ((void*)0);
}

/*struct required for server*/
long long int count=0,count2=0;
time_t sec_begin, sec_end, sec_elapsed;
/* RX is input, TX is output */
enum iodev { RX, TX };

/* common RX and TX streaming params */
struct stream_cfg {
	long long bw_hz; // Analog banwidth in Hz
	long long fs_hz; // Baseband sample rate in Hz
	long long lo_hz; // Local oscillator frequency in Hz
	const char* rfport; // Port name
};
/* static scratch mem for strings */
static char tmpstr[64];

/* IIO structs required for streaming */
static struct iio_context *ctx   = NULL;
static struct iio_channel *rx0_i = NULL;
static struct iio_channel *rx0_q = NULL;
static struct iio_channel *tx0_i = NULL;
static struct iio_channel *tx0_q = NULL;
static struct iio_buffer  *rxbuf = NULL;
static struct iio_buffer  *txbuf = NULL;


/* cleanup and exit */
void shutdowniio(int s)
{
	sec_end=time(NULL);
	sec_elapsed=sec_end-sec_begin;	
	printf("\n\nTime elapsed: %lus \tBytes recived %llu \tDebit %llu MB/sec \n ",
			sec_elapsed,	count/1024/1024,	count/sec_elapsed/1024/1024);
	printf("\nSocket sended %lld",sendcount);
	printf("* Destroying buffers\n");
	if (rxbuf) { iio_buffer_destroy(rxbuf); }
	if (txbuf) { iio_buffer_destroy(txbuf); }

	printf("* Disabling streaming channels\n");
	if (rx0_i) { iio_channel_disable(rx0_i); }
	if (rx0_q) { iio_channel_disable(rx0_q); }
	if (tx0_i) { iio_channel_disable(tx0_i); }
	if (tx0_q) { iio_channel_disable(tx0_q); }


	printf("* Destroying context\n");
	if (ctx) { iio_context_destroy(ctx); }
	exit(0);
}

/* check return value of attr_write function */
static void errchk(int v, const char* what) {
	 if (v < 0) { fprintf(stderr, "Error %d writing to channel \"%s\"\nvalue may not be supported.\n", v, what); shutdowniio(0); }
}

/* write attribute: long long int */
static void wr_ch_lli(struct iio_channel *chn, const char* what, long long val)
{
	errchk(iio_channel_attr_write_longlong(chn, what, val), what);
}

/* write attribute: string */
static void wr_ch_str(struct iio_channel *chn, const char* what, const char* str)
{
	errchk(iio_channel_attr_write(chn, what, str), what);
}

/* helper function generating channel names */
static char* get_ch_name(const char* type, int id)
{
	snprintf(tmpstr, sizeof(tmpstr), "%s%d", type, id);
	return tmpstr;
}

/* returns ad9361 phy device */
static struct iio_device* get_ad9361_phy(struct iio_context *ctx)
{
	struct iio_device *dev =  iio_context_find_device(ctx, "ad9361-phy");
	assert(dev && "No ad9361-phy found");
	return dev;
}

/* finds AD9361 streaming IIO devices */
static bool get_ad9361_stream_dev(struct iio_context *ctx, enum iodev d, struct iio_device **dev)
{
	switch (d) {
	case TX: *dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc"); return *dev != NULL;
	case RX: *dev = iio_context_find_device(ctx, "cf-ad9361-lpc");  return *dev != NULL;
	default: assert(0); return false;
	}
}

/* finds AD9361 streaming IIO channels */
static bool get_ad9361_stream_ch(struct iio_context *ctx, enum iodev d, struct iio_device *dev, int chid, struct iio_channel **chn)
{
	*chn = iio_device_find_channel(dev, get_ch_name("voltage", chid), d == TX);
	if (!*chn)
		*chn = iio_device_find_channel(dev, get_ch_name("altvoltage", chid), d == TX);
	return *chn != NULL;
}

/* finds AD9361 phy IIO configuration channel with id chid */
static bool get_phy_chan(struct iio_context *ctx, enum iodev d, int chid, struct iio_channel **chn)
{
	switch (d) {
	case RX: *chn = iio_device_find_channel(get_ad9361_phy(ctx), get_ch_name("voltage", chid), false); return *chn != NULL;
	case TX: *chn = iio_device_find_channel(get_ad9361_phy(ctx), get_ch_name("voltage", chid), true);  return *chn != NULL;
	default: assert(0); return false;
	}
}

/* finds AD9361 local oscillator IIO configuration channels */
static bool get_lo_chan(struct iio_context *ctx, enum iodev d, struct iio_channel **chn)
{
	switch (d) {
	 // LO chan is always output, i.e. true
	case RX: *chn = iio_device_find_channel(get_ad9361_phy(ctx), get_ch_name("altvoltage", 0), true); return *chn != NULL;
	case TX: *chn = iio_device_find_channel(get_ad9361_phy(ctx), get_ch_name("altvoltage", 1), true); return *chn != NULL;
	default: assert(0); return false;
	}
}

/* applies streaming configuration through IIO */
bool cfg_ad9361_streaming_ch(struct iio_context *ctx, struct stream_cfg *cfg, enum iodev type, int chid)
{
	struct iio_channel *chn = NULL;

	// Configure phy and lo channels
//	printf("* Acquiring AD9361 phy channel %d\n", chid);
	if (!get_phy_chan(ctx, type, chid, &chn)) {	return false; }
	wr_ch_str(chn, "rf_port_select",     cfg->rfport);
	wr_ch_lli(chn, "rf_bandwidth",       cfg->bw_hz);
	wr_ch_lli(chn, "sampling_frequency", cfg->fs_hz);

	// Configure LO channel
//	printf("* Acquiring AD9361 %s lo channel\n", type == TX ? "TX" : "RX");
	if (!get_lo_chan(ctx, type, &chn)) { return false; }
	wr_ch_lli(chn, "frequency", cfg->lo_hz);
	return true;
}

/* simple configuration and streaming */
int main (int argc, char **argv){

int child=fork();
if (child==-1) {DieWithError("socket() failed");}
//Father code
else if (child==0){
	pause();
//	sleep(5);	//Child run first
	// Streaming devices
	struct iio_device *rx;
	// Stream configurations
	struct stream_cfg rxcfg;

	// Listen to ctrl+c and assert
	signal(SIGINT, shutdowniio);

	// RX stream config
	rxcfg.bw_hz = MHZ(28);   // 2 MHz rf bandwidth		//200KHz-56MHz Channel bandwidths 
	rxcfg.fs_hz = MHZ(61.44);   // 2 MS/s rx sample rate	//	61.44
	rxcfg.lo_hz = GHZ(2.5); // 2.5 GHz rf frequency		//70MHz -6 GHz Local-oscilator
	rxcfg.rfport = "A_BALANCED"; // port A (select for rf freq.)

	if(verbose==1)	printf("* Acquiring IIO context\n");
	assert((ctx = iio_create_default_context()) && "No context");
	assert(iio_context_get_devices_count(ctx) > 0 && "No devices");
	if(verbose==1) printf("* Acquiring AD9361 streaming devices\n");
	assert(get_ad9361_stream_dev(ctx, RX, &rx) && "No rx dev found");
	if(verbose==1) printf("* Configuring AD9361 for streaming\n");
	assert(cfg_ad9361_streaming_ch(ctx, &rxcfg, RX, 0) && "RX port 0 not found");
	if(verbose==1) printf("* Init AD9361 IIO streaming channels\n");
	assert(get_ad9361_stream_ch(ctx, RX, rx, 0, &rx0_i) && "RX chan i not found");
	assert(get_ad9361_stream_ch(ctx, RX, rx, 1, &rx0_q) && "RX chan q not found");
	if(verbose==1) printf("* Enabling IIO streaming channels\n");
	iio_channel_enable(rx0_i);
	iio_channel_enable(rx0_q);
	if(verbose==1) printf("* Creating non-cyclic buff with 1 MiS\n");
	rxbuf = iio_device_create_buffer(rx, BUFF_SIZE/4, false);

	struct sockaddr_in serv,serv2; 				/* Echo server address */
	printf("* Server conection(192.168.0.229:50705)*");
	/* Construct the server address structure */
	memset(&serv, 0, sizeof(serv));    			/* Zero out structure */
	serv.sin_family = AF_INET;                 		/* Internet addr family */
	serv.sin_addr.s_addr = inet_addr("192.168.0.229");  	/* Server IP address */
	serv.sin_port   = htons(50705);     			/* Server port */

	memset(&serv2, 0, sizeof(serv2));    			/* Zero out structure */
	serv2.sin_family = AF_INET;                 		/* Internet addr family */
	serv2.sin_addr.s_addr = inet_addr("192.168.0.229");  	/* Server IP address */
	serv2.sin_port   = htons(50705);     			/* Server port */
    	/* Create a datagram/UDP socket */
	if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
	DieWithError("socket() failed");
	if ((sock2 = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
	DieWithError("socket2() failed");

	/* Connecting to the server */
	if (connect(sock,(struct sockaddr *)&serv, sizeof(serv)) < 0) DieWithError("socket() failed");
	if (connect(sock2,(struct sockaddr *)&serv2, sizeof(serv2)) < 0) DieWithError("socket() failed"); 
	
	/* Sincronization */
	printf("\n* Sended   AK *");   
	if (sendto(sock, "AK", 2, 0, (struct sockaddr *)&serv, sizeof(serv)) != 2)
		DieWithError("sendto() sent a different number of bytes than expected");
	fromSize = sizeof(fromAddr);
	rsplen = recvfrom(sock, buff_from_n, 1024*32, 0,(struct sockaddr *) &fromAddr, &fromSize);
	if(rsplen!= 2){ DieWithError("recvfrom() failed");}

	if (serv.sin_addr.s_addr != fromAddr.sin_addr.s_addr) {printf("Unknown client.\n"); exit(1);}
	buff_from_n[rsplen] = '\0';	
	printf("\n* Received %s *\n", (char*)buff_from_n);   
	if(verbose==1) printf("* Starting IO streaming ");

	int status;
	ssize_t nbytes_rx=0;
	void *p_dat ,*p_end;
	pthread_t tid,tid2;
	//Fill buffer from antena
	nbytes_rx = iio_buffer_refill(rxbuf); 
	if (nbytes_rx < 0) { printf("Error refilling buf %d\n",(int) nbytes_rx); shutdowniio(0); }
	p_end = iio_buffer_end(rxbuf);
	p_dat = iio_buffer_first(rxbuf, rx0_i);
	memcpy (buff_from_a, p_dat, (&p_end-&p_dat));
	printf("\n* Ready for fast transfer *");

	sec_begin= time(NULL);
	while(sec_elapsed<10)
	{	
		sec_end=time(NULL);
		sec_elapsed=sec_end-sec_begin;	
		
		//Send data to network
		memcpy (buff_from_a, p_dat, (&p_end-&p_dat));
		
		if(pthread_create(&tid, NULL, send_msg1,(int*)sock)!=0){
			printf ("\nPthread_create");exit(1);}
	//	if(pthread_create(&tid2, NULL, send_msg2,(int*)sock)!=0){
	//		printf ("\nPthread_create");exit(1);}

		//Fill buffer from antena
		nbytes_rx = iio_buffer_refill(rxbuf); 
			if (nbytes_rx < 0) { printf("Error refilling buf %d\n",(int) nbytes_rx); shutdowniio(0); }
		p_end = iio_buffer_end(rxbuf);
		p_dat = iio_buffer_first(rxbuf, rx0_i);
		count=count+nbytes_rx;

		//Syncronizing
		if(pthread_join(tid ,(void**)&status)!=0){printf ("Pthread_join");exit(1);}
	//	if(pthread_join(tid2,(void**)&status)!=0){printf ("Pthread_join");exit(1);}
	}
printf("\n\tTime elapsed: %lus \n\tAntena recived  %llu MB\tDebit %llu Mb/sec ",
		sec_elapsed,	count/1024/1024,	count/sec_elapsed/1024/1024);
printf("\n\tSocket sended %lld Mb \n",sendcount/1024/1024);
//printf("\nBytes sendeded %llu ",summ);
	
close(sock);
close(tcpsock);
return 0;
}//End Parent




//Child programm
else if (child!=0){
	//pause();
	// Listen to ctrl+c and assert
	signal(SIGINT, shutdowniio);
	struct iio_device *tx;		// Streaming devices
	struct stream_cfg txcfg;	// Stream configurations
	// TX stream config
	txcfg.bw_hz = MHZ(28); 		// 1.5 MHz rf bandwidth
	txcfg.fs_hz = MHZ(61.44);  	// 2 MS/s tx sample rate
	txcfg.lo_hz = GHZ(2.5); 	// 2.5 GHz rf frequency
	txcfg.rfport = "A"; 		// port A (select for rf freq.)
	
	if(verbose==1) printf("\t\t\t\t\t* Acquiring IIO context\n");
	assert((ctx = iio_create_default_context()) && "No context");
	assert(iio_context_get_devices_count(ctx) > 0 && "No devices");
	if(verbose==1) printf("\t\t\t\t\t* Acquiring AD9361 streaming devices\n");
	assert(get_ad9361_stream_dev(ctx, TX, &tx) && "No tx dev found");
	if(verbose==1) printf("\t\t\t\t\t* Configuring AD9361 for streaming\n");
	assert(cfg_ad9361_streaming_ch(ctx, &txcfg, TX, 0) && "TX port 0 not found");
	if(verbose==1) printf("\t\t\t\t\t* Initializing AD9361 IIO streaming channels\n");
	assert(get_ad9361_stream_ch(ctx, TX, tx, 0, &tx0_i) && "TX chan i not found");
	assert(get_ad9361_stream_ch(ctx, TX, tx, 1, &tx0_q) && "TX chan q not found");
	if(verbose==1) printf("\t\t\t\t\t* Enabling IIO streaming channels\n");
	iio_channel_enable(tx0_i);
	iio_channel_enable(tx0_q);
	if(verbose==1) printf("\t\t\t\t\t* Creating non-cyclic IIO buffers with 1 MiS\n");
	txbuf = iio_device_create_buffer(tx, BUFF_SIZE/4, false);

	printf("\t\t\t\t\t* Server conection*\n");
	struct sockaddr_in serv; 				/* Echo server address */
	/* Construct the server address structure */
	memset(&serv, 0, sizeof(serv));    			/* Zero out structure */
	serv.sin_family = AF_INET;                 		/* Internet addr family */
	serv.sin_addr.s_addr = inet_addr("192.168.0.229");  	/* Server IP address */
	serv.sin_port   = htons(50707);     			/* Server port */
    	/* Create a datagram/UDP socket */
	if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)	DieWithError("socket() failed");
	/* Connecting to the server */
	//if (connect(sock,(struct sockaddr *)&serv, sizeof(serv)) < 0) 	DieWithError("socket() failed");
	//pause();
	/* Sincronization */
	printf("\t\t\t\t\t* Sended   AK *\n");   
	if (sendto(sock, "AK", 2, 0, (struct sockaddr *)&serv, sizeof(serv)) != 2)
		DieWithError("sendto() sent a different number of bytes than expected");
	fromSize = sizeof(fromAddr);
	rsplen = recvfrom(sock, buff_from_n, 1024*32, 0,(struct sockaddr *) &fromAddr, &fromSize);
	if(rsplen!= 2){ DieWithError("recvfrom() failed");}

	if (serv.sin_addr.s_addr != fromAddr.sin_addr.s_addr) {printf("Unknown client.\n"); exit(1);	}
	buff_from_n[rsplen] = '\0';	
	printf("\t\t\t\t\t* Received %s *", (char*)buff_from_n);   

	int status;
	ssize_t nbytes_tx=0;
	void  *t_dat,*t_end;
	pthread_t tid3;
	setpriority(PRIO_PROCESS, 0, -20);
	//Fill buff_from_n from network
	if(pthread_create(&tid3, NULL, recv_msg,(int*)sock)!=0){
		printf ("\nPthread_create\n");exit(1);}
	if(pthread_join(tid3,(void**)&status)!=0){printf ("Pthread_join");exit(1);}
	//Bug push after in the third try	
	nbytes_tx = iio_buffer_push(txbuf);
	nbytes_tx = iio_buffer_push(txbuf);
	printf("\n\t\t\t\t\tReciving........  ");
	while(1)
      	{
		//Fill TXbuffer 
		t_end = iio_buffer_end(txbuf);
		t_dat = iio_buffer_first(txbuf, tx0_i);
		memcpy(t_dat,buff_from_n, (1024*1024));
		nbytes_tx = iio_buffer_push(txbuf);
		//Fill buff_from_n from network
		if(pthread_create(&tid3, NULL, recv_msg,(int*)sock)!=0){printf ("\nPthread_create\n");exit(1);}
      		// Send buffer to antena
		if (nbytes_tx < 0) { printf("Error pushing buf %d\n", (int) nbytes_tx); shutdowniio(0); }
		count2=count2+nbytes_tx;
		if(pthread_join(tid3,(void**)&status)!=0){printf ("Pthread_join");exit(1);}
		if(status==1){ break;	}
	}

	t_end = iio_buffer_end(txbuf);
	t_dat = iio_buffer_first(txbuf, tx0_i);
	memcpy(t_dat,buff_from_n, (&t_end-&t_dat));
	nbytes_tx = iio_buffer_push(txbuf);
	if (nbytes_tx < 0) { printf("Error pushing buf %d\n", (int) nbytes_tx); shutdowniio(0); }
	count2=count2+nbytes_tx;

	printf("\n\t\t\t\t\tRecived from net %llu MB",recvcount/1024/1024);
	printf("\tSended to antena %lld Mb \n",count2/1024/1024);

}

return 0;
}//End of the life
