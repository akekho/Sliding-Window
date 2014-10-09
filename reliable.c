#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <stdbool.h>
#include "rlib.h"



/* 
 * DEFINED CONSTANTS 
 * =============================== */
#define READ_SIZE 500
#define DATA_HEADER_SIZE 12
#define ACK_PKT_SIZE 8
#define MAX_NUM_PKT_IN_BUFFER 1
#define MAX_NUM_PKT_IN_FLIGHT 1
#define MILLI 1000
/* =============================== */





/* STRUCT dataPktNode
 * ==========================================================================
 * This struct is for packets that are in flight or those which are
 * waiting to be printed out.
 * dataPkt - refers to the packet in question.
 * next - is a pointer to the next packet on the list.
 * pktIsEmpty - informs whether or not there's a packet on a particular spot
 * timeSent - this is only useful for packets in flight, is the time stamp
 *            that tells us when the packet was sent incase of timeouts.
 * ===========================================================================
 */
struct dataPktNode{
  packet_t dataPkt;
  bool pktIsEmpty;
  struct timespec timeSent; 
  struct dataPktNode *next;
};
typedef struct dataPktNode dataPktNode;



/* STRUCT rel_t
 * ==========================================================================================
 * seqnoForLastSentPkt - Sequence number for the last sent packet
 * seqnoForLastReceivedPkt - Sequence number for the last received packet
 * numPktsToOutput - Number of packets in the received packet buffer
 * numPktsInFlight - Number of packets in flight
 * lastAcknoReceived - The number of the last acknowledgement received
 * packetsInFlight - A pointer to the linked list of packets in flight
 * packetsToOutput - A pointer to the linked list of packets to output
 * inputBuffer - A buffer that reads input data.
 * partialPacketInFlight -  A value that keeps track of whether or not
 *                          there is a partial packet in flight
 * readEOFOrErrorFromInput - A flag that is set to true if we read an EOF or Error from input
 * readEOFFromSender -  A flag that is set to true if we read EOF from
 *                      sender
 * EOFreadBuffNotEmpty - A flag that is set to true if we read an EOF
 *                       while we have a partial packet in flight
 * stateConfig - A structure for the state configuration.
 * socket - A pointer to the socket address storage of the state.
 * 
 * ==========================================================================================
 */
struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;
  conn_t *c;			/* This is the connection object */


  uint32_t seqnoForLastSentPkt; 
  uint32_t seqnoForLastReceivedPkt; 
  uint32_t numPktsToOutput; 
  uint32_t numPktsInFlight;
  uint32_t lastAcknoReceived;
  dataPktNode *packetsInFlight; 
  dataPktNode *packetsToOutput; 
  char inputBuffer[READ_SIZE];
  bool partialPacketInFlight;
  bool readEOFOrErrorFromInput; 
  bool readEOFFromSender; 
  bool EOFreadBuffNotEmpty;
  uint32_t amount;
  struct config_common stateConfig;
  struct sockaddr_storage *socket;

};
rel_t *rel_list;






/*
 * FUNCTION PROTOTYPES
 * ==================================================================== */
void initDataPacket(packet_t *dataPkt);
void initAckPacket(struct ack_packet *ackPkt);
bool ackWithinRange(packet_t *ackPkt, rel_t *r);
void convertToHostByteOrder(packet_t *packet);
void convertToNetworkByteOrder(packet_t *pkt);
void handleAckPacket(rel_t *r,packet_t *ackPkt); 
void handleDataPacket(rel_t *r,packet_t *ackPkt);
void sendAckPacket(rel_t *r);
void sendDataPacekt(rel_t *s, char *buff, size_t dataSize, bool isEOF);
void checkTermination(rel_t *state);
bool isCorrupted(packet_t *pkt);
bool isInvalidLength(uint16_t pktLen, size_t n);
void checkRetransmission(rel_t *state, struct timespec currTime);
void updatePktsInFlightList(rel_t *r, uint32_t ackno);
void retransmit(rel_t *s, packet_t *pkt);
void initPacketFlightList(rel_t *s);
void initPacketOutputList(rel_t *r);
void bufferReceivedPacket(rel_t *r, packet_t *pkt);
void freePacketsInFlightList(rel_t *s);
void freePacketsToOutputList(rel_t *r);
void emptyBuffer(rel_t *r);
void freeMemory(dataPktNode *start);
void initLinkedList(rel_t *r, bool flightList);
void attemptToPrintEOF(rel_t *r, packet_t *dataPkt);
bool dataPktWithinRange(rel_t *r, packet_t *dataPkt);
void nagleBufferInput(rel_t *s);
/* ==================================================================== */








/*
 * Function: initAckPacket
 * Usage: initAckPacket(&(ackPkt))
 * --------------------------------------------------------------------
 * This method is called for the initialization of acknowledge packets.
 * It zero's out all the fields ready to have data put in.
 */
void initAckPacket(struct ack_packet *ackPkt){
  ackPkt->cksum = 0;
  ackPkt->len = 0;
  ackPkt->ackno = 0;
}

/*
 * Function: initDataPacket
 * Usage: initDataPcket(&(state->pktInFlight))
 * -------------------------------------------------------------------
 * This method is called for the initialization of data packets.
 * It zero's out all the fields so thata they can later be filled in
 * with appropriate information.
 */
void initDataPacket(packet_t *dataPkt){
  dataPkt->cksum = 0;
  dataPkt->len = 0;
  dataPkt->ackno = 0;
  dataPkt->seqno = 0;
  memset(dataPkt->data,'\0',READ_SIZE);
}


/*
 * Function: convertToNetworkByteOrder
 * Usage: convertToNetworkByteOrder(pkt)
 * -----------------------------------------------------------------
 * This function converts the fields of a packet into network
 * byte order ready for transmission to be done across the wire.
 * In cases where the packet is an ack packet, the function returns
 * without converting a the sequence number.
 */
void convertToNetworkByteOrder(packet_t *pkt){
  pkt->len = htons(pkt->len);
  pkt->ackno = htonl(pkt->ackno);
  if(ntohs(pkt->len) == ACK_PKT_SIZE) return;
  pkt->seqno = htonl(pkt->seqno);
}


/*
 * Function: convertToHostByteOrder
 * Usage: convertToHostByteOrder(pkt)
 * -----------------------------------------------------------------
 * This function converts the fields of a packet into host byte
 * order ready for reading to be done by the receiver.
 * In cases where the packet is an ack packet, the function
 * returns without converting the sequence number.
 */
void convertToHostByteOrder(packet_t *pkt){
  pkt->len = ntohs(pkt->len);
  pkt->ackno = ntohl(pkt->ackno);
  if(pkt->len == ACK_PKT_SIZE) return;
  pkt->seqno = ntohl(pkt->seqno);

}


/*
 * Function: isInvalidLength
 * Usage: if(isInvalidLength(pktLen, n)) return;
 * --------------------------------------------------------------------
 * This function checks to see if the length of the packet is valid.
 *
 * If the length reported by the system is less than the length of an
 * acknoweldgement packet or less than the packet length then we know
 * that this is invalid and we return true.
 * 
 * If the length reported by the system is greater than the
 * acknowledgement size which means that the packet then has 
 * to be a data packet and if also at the same time the 
 * length is length than the data packet header size then 
 * we know this is invalid and we return true. The same is true
 * for the packet length.
 *
 * If the length reported by the system or the packet length is greater
 * than the maximum size of a data packet we also return true.
 * 
 * If all these conditions don't pass then we return false.
 */
bool isInvalidLength(uint16_t pktLen, size_t n){
  if((n < ACK_PKT_SIZE) || (n < pktLen)) return true;
  if((pktLen > ACK_PKT_SIZE) && (pktLen < DATA_HEADER_SIZE)) return true;
  if((pktLen > DATA_HEADER_SIZE + READ_SIZE)) return true;
  
  return false;
}


/*
 * Function: isCorrupted
 * Usage: if(isCorrupted(pkt)) return;
 * --------------------------------------------------------------------
 * This function checks to see if the packet's stored checksum
 * correspond to it's calculated checksum in which case we know
 * that the received packet is not corrupted otherwise we
 * report that the packet is corrupted.
 */
bool isCorrupted(packet_t *pkt){
  uint16_t cksumOfPkt = pkt->cksum;
  pkt->cksum = 0;
  uint16_t newCksum =  cksum((const void*)pkt, ntohs(pkt->len));
  pkt->cksum = cksumOfPkt;
  if(cksumOfPkt != newCksum)return true;
  return false;
}


/*
 * Function: updatePktsInFlightList
 * Usage: updatePktsInFlightList(r, ackPkt->ackno)
 * --------------------------------------------------------------------
 * Upon receiving an acknowledgement this function goes to the flight
 * list and updates all the packets that have sequence numbers less than
 * the acknowledge number by setting their fields of being empty to true.
 * 
 * Also if any of the packet was a partial packet we note this fact by
 * setting the flag to false.
 *
 */
void updatePktsInFlightList(rel_t *r, uint32_t ackno){
  dataPktNode *tempPtr;
  tempPtr = r->packetsInFlight;
  
  while(tempPtr){
    if(!(tempPtr->pktIsEmpty)){
      if(tempPtr->dataPkt.seqno < ackno) tempPtr->pktIsEmpty = true;
      if((tempPtr->dataPkt.len - DATA_HEADER_SIZE) < READ_SIZE) r->partialPacketInFlight = false;
    }
    tempPtr = tempPtr->next;
  }
  

}



/*
 * Function: freeMemory
 * Usage: freeMemory(s->packetsInFlight) OR freeMemory(s->packetsToOutput)
 * ----------------------------------------------------------------------
 * This function takes in the pointer to either the beginning of the
 * packets in flight list or packet to output list and frees all the
 * memory allocated by them.
 */
void freeMemory(dataPktNode *start){
  dataPktNode *tempPtr = start;
  while(tempPtr){
    dataPktNode *next;
    next = tempPtr->next;
    free(tempPtr);
    tempPtr = next;
  }
}


/* Function: freePacketsInFlight
 * Usage: freePacketsInFlight(s)
 * --------------------------------------------------------
 * Calls the helper function freeMemory with the beginning
 * of the pointer to the packet in flight linked list.
 */
void freePacketsInFlightList(rel_t *s){
  freeMemory(s->packetsInFlight);
}



/* Function: freePacketsInFlight
 * Usage: freePacketsInFlight(s)
 * --------------------------------------------------------
 * Calls the helper function freeMemory with the beginning
 * of the pointer to the packet in flight linked list.
 */
void freePacketsToOutputList(rel_t *r){
  freeMemory(r->packetsToOutput);
}



/*
 * Function: ackWithinRange
 * Usage: if(ackWithinRange(ackPkt,r)
 * -------------------------------------------------------------------
 * This function checks to see if the ack is within the expected range
 * For example if the sequence for the last sent packet was 6 and there
 * are 3 packets in flight (which means we have 4, 5 and 6) then we expect 
 * the ackno to be between 5 [that is: 6 - 3 + 2], acknowledging the receiving of
 * packet 4 (hence we have ackno >= r->seqnoForLastSentPkt -
 * r->numPktsInFlight + 2)...AND the acknowledgmenet should go as far
 * as 7 [that is: 6 + 1], acknowledging the receiving of the last packet, packet 6
 * (hence we have ackno <= r->seqnoForLastSentPkt + 1)
 *
 */
bool ackWithinRange(packet_t *ackPkt, rel_t *r){
  return ((ackPkt->ackno >= r->seqnoForLastSentPkt - r->numPktsInFlight + 2) 
	  &&(ackPkt->ackno <= r->seqnoForLastSentPkt + 1));
}




/*
 *
 * Function: handleAckPacket
 * Usage: handleAckPacket(state,ackPkt)
 * -----------------------------------------------------------------------
 * If an acknowledgement is within the range of packets in flight then we
 * we update the packets in flight by setting their fields to empty.
 * Afterwards we update the number of packets in flight and the last
 * acknoweldgement we have received.
 * We then check for termination. If we don't need to terminate then we 
 * call rel_read to read in more packets.
 */
void handleAckPacket(rel_t *r, packet_t *ackPkt){
  if(ackWithinRange(ackPkt,r)){
    updatePktsInFlightList(r, ackPkt->ackno);
    r->numPktsInFlight = r->seqnoForLastSentPkt - (ackPkt->ackno - 1);
    r->lastAcknoReceived = ackPkt->ackno;
    checkTermination(r);
    rel_read(r);
  }
 
}


/*
 *
 * Function: sendAckPacket
 * Usage: sendAckPacket(state,dataPkt)
 * -----------------------------------------------------------------------
 * This function sends an acknowledgement based on the received data
 * packet, dataPkt. It creates an ack_packet struct and initializes
 * all the fields and afterwards it fills in the appropriate fields in,
 * converts the fields into Network Byte Order and then afterwards
 * sends the packet.
 */
void sendAckPacket(rel_t *r){
  struct ack_packet ackPkt;
  initAckPacket(&(ackPkt));
  ackPkt.len = ACK_PKT_SIZE;
  ackPkt.ackno = r->seqnoForLastReceivedPkt + 1;
  convertToNetworkByteOrder((packet_t*)&(ackPkt));
  ackPkt.cksum = cksum((const void*)&(ackPkt), ntohs(ackPkt.len));
  conn_sendpkt(r->c, (packet_t*)&(ackPkt), ntohs(ackPkt.len));
}




/*
 * Function: checkTermination
 * Usage: checkTermination(state)
 * --------------------------------------------------
 * This function checks if all the four conditions necessary
 * for terminating a connection are met. The conditions are:
 *
 * We have read EOF from sender.
 * We have read EOF or -1 from our input.
 * All the data that we have sent has been acknowledged
 * All the packets that we have received have been printed out.
 *
 * If they are all met then  we tear down the connection.
 * Otherwise we just return.
 */
void checkTermination(rel_t *r){
  if(r->readEOFFromSender){
    if(r->readEOFOrErrorFromInput){
      if(r->lastAcknoReceived == r->seqnoForLastSentPkt + 1){
	if(r->numPktsToOutput == 0){
	  rel_destroy(r);
	}
      } 
    }
  }
  return;
}



/*
 * Function: bufferReceivedPacket(rel_t *r, packet_t *pkt)
 * Usage: bufferReceivedPacket(r,dataPkt);
 * -------------------------------------------------------------------
 * This functions buffer a received data packet if it cannot currently
 * be printed out either because it is out of order or if it's because
 * the output buffer is occupied.
 * 
 * Since we have to make sure these packets are printed in order once 
 * space is available and we are not guaranteed if our sender will send
 * them in order we have to count and find the correct place to insert
 * them so that they will be in order during printing. Because of this
 * fact we have a count that counts from the lowest packet we are
 * expecting and it is increased by one until we find the appropriate
 * spot. Upon finding the spot, if it is not empty then this packet is 
 * a duplicate and we do nothing, otherwise we copy the contents
 * note that the place is occupied and increase the number of packets
 * that are in queue for output.
 */
void bufferReceivedPacket(rel_t *r, packet_t *pkt){
  dataPktNode *tempPtr;
  tempPtr = r->packetsToOutput;
  uint32_t count = r->seqnoForLastReceivedPkt + 1;
  while(tempPtr){
    if(pkt->seqno == count){
      if(tempPtr->pktIsEmpty){
	memcpy(&(tempPtr->dataPkt),pkt,DATA_HEADER_SIZE + READ_SIZE);
	tempPtr->pktIsEmpty = false;
	r->numPktsToOutput++;
      }
      return;
    }
    count++;
    tempPtr = tempPtr->next;

  }
 
}


/*
 * Function: attemptToPrintEOF
 * Usage: attemptToPrintEOF(r,pkt)
 * -------------------------------------------------
 * This functions check if we can print out an EOF after we
 * have received an EOF from the sender. The only time this
 * will be possible is if there's no packet buffered. After
 * we print out the EOF we check if the four conditions
 * for terminating have been met.
 *
 */
void attemptToPrintEOF(rel_t *r, packet_t *dataPkt){
  if((r->numPktsToOutput == 0)){
    r->seqnoForLastReceivedPkt = dataPkt->seqno;
    r->readEOFFromSender = true;
    sendAckPacket(r);
    conn_output(r->c, NULL, 0);
    checkTermination(r);
  }

}

/*
 * Function: dataPktWithinRange
 * Usage: if dataPktWithinRange(r,dataPkt)
 * ----------------------------------------------------------------------
 * This function returns whether or not the received data packet is within
 * the range of our expected data according to the window size.
 * For example if our window size is 5 and the last data packet we
 * received was 2, then we expect to get packets that range from
 * 3 [that is: 2 + 1], hence we have (seqno >= seqnoForLastReceivedPkt +
 * 1) up to 7 [that is: 2 + 5], hence we have (seqno >=
 * seqnoForLastReceivedPkt + r->stateConfig.window)
 */
bool dataPktWithinRange(rel_t *r, packet_t *dataPkt){
  return ((dataPkt->seqno >= r->seqnoForLastReceivedPkt + 1) && 
	  (dataPkt->seqno <= r->seqnoForLastReceivedPkt+r->stateConfig.window));
}



/*
 * Function: handleDataPackets
 * Usage: handleDataPackets(state, pkt)
 * ----------------------------------------------------------------
 * This functions handles received data packets.
 * 
 * If the output buffer has packets equivalent to the maximum
 * number of packets that our output window allows we just drop the
 * packet.
 *
 * If the data packet's sequence number is within the range that 
 * we are currently expecting based on our window then the following
 * happens:
 * If the data packet is EXACTLY one sequence number above the last
 * one we printed then we need to examine it. If it is an EOF packet
 * then we attempt to print it out by calling the function
 * attemptToPrintEOF which will check if we have printed everything
 * otherwise it will just drop the EOF packet.
 * If what we have is a data packet with a payload then we check
 * if there's enough space to print it. If there isn't any then
 * we buffer it. If there's a space we print it and go attempt to
 * empty the buffer, had this packet been the one we were waiting
 * in order to be able to print the packets that are currently buffered.
 * 
 *
 * If the data packet is not EXACTLY a sequence number above the last
 * packet we printed then we just buffer it and send an acknowledgement
 * requesting for the packet that we expect to receive.
 *
 *
 * If the data is outside the range we just drop it and
 * send an acknowledgement requesting for the packet we are
 * currently expecting
 */
void handleDataPacket(rel_t *r, packet_t *dataPkt){

  if(r->numPktsToOutput == r->stateConfig.window) return;

  
  if(dataPktWithinRange(r,dataPkt)){
    size_t available_space = conn_bufspace(r->c);
    if(dataPkt->seqno == r->seqnoForLastReceivedPkt + 1){
      if(dataPkt->len == DATA_HEADER_SIZE){
	attemptToPrintEOF(r,dataPkt);	
	return;
      }
       
      if(available_space < dataPkt->len - DATA_HEADER_SIZE){
	bufferReceivedPacket(r,dataPkt);
	return;
      }
     
      r->seqnoForLastReceivedPkt = dataPkt->seqno;
      conn_output(r->c,dataPkt->data,dataPkt->len - DATA_HEADER_SIZE);
      emptyBuffer(r);
    }
    else{
       bufferReceivedPacket(r,dataPkt);
       sendAckPacket(r);
       return;
    } 
  }
  sendAckPacket(r);
 
}

//./tester (-s) -v valgrind --leak-check=full --show-reachable=yes ./reliable




/*
 * Function: initLinkedList
 * Usage: initLinkedList(r,true) OR initLinkedList(r,false)
 * ----------------------------------------------------------------
 * This function attempts to re-use the code to initialize a linked 
 * list for both a packetToFlight list and a packetToOutput list
 * Every packet is set to empty and the necessary pointer re-wiring
 * is done.
 * The function receives a flag that tells it whether we
 * are initializing a flightList or not.
 */
void initLinkedList(rel_t *r, bool flightList){
  
  int count = 0;
  dataPktNode *tempPtr;
  if(flightList){
    r->packetsInFlight = malloc(sizeof(dataPktNode));
    tempPtr = r->packetsInFlight;
  }
  else{
    r->packetsToOutput = malloc(sizeof(dataPktNode));
    tempPtr = r->packetsToOutput;
  }

  
  while(true){
    tempPtr->pktIsEmpty = true;
    if(count == r->stateConfig.window - 1){
      tempPtr->next = NULL;
      break;
    }
    tempPtr->next = malloc(sizeof(dataPktNode));
    tempPtr = tempPtr->next;
    count++;
  }
}



/*
 * Function: initPacketFlightList
 * Usage: initPacketFlightList(s)
 * ---------------------------------------------------
 * Uses a helper function initLinkedList to initialize
 * the linked list of the packet in flight.
 */
void initPacketFlightList(rel_t *s){
  initLinkedList(s,true);
}


/*
 * Function: initPacketOutputList
 * Usage: initPacketOutputList(s)
 * ---------------------------------------------------
 * Uses a helper function initLinkedList to initialize
 * the linked list of the packet to be outputed.
 *
 */
void initPacketOutputList(rel_t *r){
  initLinkedList(r,false);

}



/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
	    const struct config_common *cc)
{
  
  rel_t *r;

  r = xmalloc (sizeof (*r));
  memset (r, 0, sizeof (*r));

  if (!c) {
    c = conn_create (r, ss);
    if (!c) {
      free (r);
      return NULL;
    }
  }

  r->c = c;
  r->next = rel_list;
  r->prev = &rel_list;
  if (rel_list)
    rel_list->prev = &r->next;
  rel_list = r;

  /* Do any other initialization you need here */
  r->seqnoForLastSentPkt = 0;
  r->seqnoForLastReceivedPkt = 0;
  r->numPktsToOutput = 0;
  r->numPktsInFlight = 0;
  r->lastAcknoReceived = 1;
  r->amount = 0;
  r->inputBuffer[0] = 0;
  r->readEOFOrErrorFromInput = false;
  r->readEOFFromSender = false;
  r->partialPacketInFlight = false;
  r->EOFreadBuffNotEmpty = false;

 
  //Initializing the config_common of our state
  r->stateConfig.window = cc->window;
  r->stateConfig.timer = cc->timer;
  r->stateConfig.timeout = cc->timeout;
  r->stateConfig.single_connection = cc->single_connection;


  //Initializing the packet flight lists
  initPacketFlightList(r);
  initPacketOutputList(r);

  //Initializing the sockaddr_storage
  if(ss){
    r->socket = malloc(sizeof(struct sockaddr_storage)); 
    memcpy(r->socket,ss,sizeof(struct sockaddr_storage));
  }
  else{
    r->socket = NULL;
  }

  return r;
}


/*
 * Function rel_destroy
 * --------------------
 * This function frees dynamically allocated 
 * objects. In our case two linked lists, a
 * sockaddr_storage struct IF not NULL and
 * and rel_t.
 */
void
rel_destroy (rel_t *r)
{
 
  if (r->next)
    r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy (r->c);

  /* Free any other allocated memory here */
  freePacketsInFlightList(r);
  freePacketsToOutputList(r);

  if(r->socket) free(r->socket);
  free(r);
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 * If this is a new connection but the packet doesn't have the first
 * sequence number we just drop it.
 */
void
rel_demux (const struct config_common *cc,
	   const struct sockaddr_storage *ss,
	   packet_t *pkt, size_t len)
{

  if(isInvalidLength(ntohs(pkt->len), len)) return;
  if(isCorrupted(pkt)) return; 
  convertToHostByteOrder(pkt);
  
  rel_t *r = rel_list;
  while(r){
    if(addreq(r->socket,ss)){
      convertToNetworkByteOrder(pkt);
      rel_recvpkt(r,pkt,len);
      return;
    }
    r=r->next;
  }
 
  if(pkt->seqno == 1){
    rel_create(NULL,ss,cc);  
    rel_recvpkt(r,pkt,len);
  }
  
}





/*
 * Function: rel_recvpkt
 * ----------------------------------------------------------------
 * This function is called when a packet is received. First a check
 * is done to see if the length is valid if it isn't we return.
 *
 * Then a check is done to see if the packet is corrupted in which
 * case we also return.
 *
 * If all the checks fail in which case our packet is valid, we convert it
 * into host order and then determines whether it is an acknowledge
 * or data packet and deal with each case accordingly.
 */
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
  if(isInvalidLength(ntohs(pkt->len), n)) return;
  if(isCorrupted(pkt)) return; 
  convertToHostByteOrder(pkt);
  if(pkt->len == ACK_PKT_SIZE ) {
    handleAckPacket(r,pkt);
    return;
  }
  handleDataPacket(r,pkt);

}


/*
 * Function: setPacketFields
 * usage: setPacketFields(s,&(tempPtr->dataPkt),dataSize);
 * --------------------------------------------------------
 * This function sets the packet fields and then converts
 * them to network byte order and compute the checksum afterwards
 * ready to send the packet.
 */
void setPacketFields(rel_t* s,packet_t *pkt,size_t dataSize){
  s->seqnoForLastSentPkt++;
  pkt->seqno = s->seqnoForLastSentPkt;
  pkt->len = dataSize + DATA_HEADER_SIZE;
  pkt->ackno = 0;
  pkt->cksum = 0;
  convertToNetworkByteOrder(pkt);
  pkt->cksum = cksum((const void*)pkt,ntohs(pkt->len));
}

/*
 * Function: sendDataPacket
 * Usage: sendDataPacket(s,pkt,true) OR sendDataPacket(s,pkt,false)
 * -----------------------------------------------------------------
 * This is a helper function called whenever we want to send a data
 * packet. We first find a point on the linked list of packets in
 * flight that is empty. 
 * 
 * Upon finding this spot, we first check to see if the packet is not
 * an EOF in this case we will copy the contents of the buffer to the data
 * part of our packet, otherwise if the packet is an EOF the payload has
 * nothing so we skip this.
 *
 * We then set other fields of the packets appropriately and increase the
 * number of packets that are currently in flight.
 *
 * After that we send the packet, time stamps it set the spot that the
 * packet occupies to be false and also we convert the packet to host byte
 * order for later when we want to do operations on it such as checking
 * the sequence number once acknowledgements arrive and we want to update
 * the spot to be empty. In addition to that we check if the sent packet
 * was a partial packet. If it is we not this.
 * We also update the amount of data read to the input buffer to be 0
 * since we just copied all of it's contents.
 *
 *
 */
void sendDataPacket(rel_t *s, char *buff, size_t dataSize, bool isEOF){
  dataPktNode *tempPtr;
  tempPtr = s->packetsInFlight;


  while(!(tempPtr->pktIsEmpty)){
    tempPtr = tempPtr->next;
  }
 
  if(!isEOF) memcpy(tempPtr->dataPkt.data,buff,dataSize+1);
  setPacketFields(s,&(tempPtr->dataPkt),dataSize);
  s->numPktsInFlight++;

 
  conn_sendpkt(s->c, &(tempPtr->dataPkt), ntohs(tempPtr->dataPkt.len));
  clock_gettime(CLOCK_MONOTONIC,&(tempPtr->timeSent));
  tempPtr->pktIsEmpty = false;
  convertToHostByteOrder(&(tempPtr->dataPkt));
  if((tempPtr->dataPkt.len -DATA_HEADER_SIZE) < (READ_SIZE-1)) s->partialPacketInFlight = true;
  s->amount = 0;
  rel_read(s);


}



/*
 * Function: nagleBufferInput
 * Usage: nagleBufferInput(s);
 * ------------------------------------------------------
 * This function buffers up a packet until we have a full sized
 * packet since at the moment we have a partial packet in flight.
 * In case we read an EOF while still buffering then we set a flag
 * that we had read an EOF while having an outstading partial packet
 * in flight. Incase we read nothing we return.
 * Otherwise we copy the read data to our input buffer.
 */
void nagleBufferInput(rel_t *s){
  if(s->EOFreadBuffNotEmpty) return;
  char buff[READ_SIZE - s->amount - 1];
  int actualDataSize = conn_input(s->c,buff,READ_SIZE - s->amount - 1);
  if(actualDataSize == 0) return;
  if(actualDataSize == -1) {
    s->EOFreadBuffNotEmpty = true;
    return;
  }
  memcpy(&(s->inputBuffer[s->amount]),buff,actualDataSize);
  s->amount += actualDataSize;
}

/*
 * Function: processData
 * Usage processData(s,s->amount);
 * ---------------------------------
 * This function determines what to data to send to the receiver
 * based on the input received by conn_input. If 0 is read we
 * just return. If -1 is read then we send an EOF to the receiver.
 * otherwise we send a normal data packet with a payload.
 */
void processData(rel_t *s, size_t actualDataSize){
  if(actualDataSize == 0) return;

  if(actualDataSize == -1){
    s->readEOFOrErrorFromInput = true;
    sendDataPacket(s,NULL,0,true);
    return;
  }

  s->inputBuffer[actualDataSize] = 0;
  sendDataPacket(s,s->inputBuffer,actualDataSize,false);
 
}





/*
 * Function: rel_read
 * -----------------------------------------------------------------
 * This function can be called in by the system when there's input to
 * read or it can also called by my program when an acknowledgment is
 * received.
 *
 *
 * The function first checks to see if we are in a state of 
 * where the buffer for packets in flight is maximum or we had 
 * read an EOF or error from input already in which case we 
 * just ignore the input.
 *
 * Then the function considers two states that the program can be in
 * 1. Partial packet in flight
 * If there's a partial packet in flight and we find out that our
 * buffer is a full packet then we send it, otherwise we buffer up the
 * input by calling the function nagleBufferInput.
 *
 * 2. No Partial packet in flight
 * If there's no partial packet in flight then:
 * If the buffer is empty then we check if had previously read an
 * EOF while still buffering up our  nagle buffer in which case 
 * we don't read any more input otherwise we read in more input. 
 * If the buffer is not empty then we just send out whatever data
 * is contained in there as there are no more partial packets.
 *
 */
void
rel_read (rel_t *s)
{
  //PACKETS IN FLIGHT MAXIMUM OR READ EOF STATE
  if(s->numPktsInFlight == s->stateConfig.window || s->readEOFOrErrorFromInput) return;
  

  //PARTIAL PACKET IN FLIGHT STATE
  if(s->partialPacketInFlight){
    if(s->amount == READ_SIZE-1){
      return processData(s,s->amount);
    }
    else{
      return nagleBufferInput(s);
    } 
    
  }
  
  //NO PARTIAL PACKET IN FLIGHT STATE
  if(!s->partialPacketInFlight){
    int actualDataSize;
    if(s->amount == 0){
      if(s->EOFreadBuffNotEmpty == true) return processData(s,-1);
      actualDataSize = conn_input(s->c,s->inputBuffer,READ_SIZE-1);
      return processData(s,actualDataSize);
    }
    else{
      return processData(s,s->amount);
    }
    
  }
  
}






/*
 * Function: emptyBuffer
 * Usage: emptyBuffer(r)
 * --------------------------------------------------------------------
 * This function attempts to see if we can empty the buffer of
 * outstanding packets uponn receiving the packet that we were waiting
 * print or having been notified that there's available space by rel_output.
 *
 * After printing out every packet to the output we update the place
 * occupied by it as empty and decrease the number of packets to output.
 */
void emptyBuffer(rel_t *r){

  dataPktNode *tempPtr = r->packetsToOutput;
  while(tempPtr){
    if((!tempPtr->pktIsEmpty)){
      if(tempPtr->dataPkt.seqno == (r-> seqnoForLastReceivedPkt + 1)){ 
	size_t available_space = conn_bufspace(r->c);
	if(available_space > tempPtr->dataPkt.len - DATA_HEADER_SIZE){
	  conn_output(r->c,tempPtr->dataPkt.data,tempPtr->dataPkt.len - DATA_HEADER_SIZE);
	  r->seqnoForLastReceivedPkt = tempPtr->dataPkt.seqno;
	  tempPtr->pktIsEmpty = true;
	  r->numPktsToOutput--;
	}
	else return;
      }
    }
    tempPtr = tempPtr->next;
  }  
  
}


/*
 * Function rel_output(state)
 * ---------------------------------------------------------
 * This function is called whenever there's available space
 * at the output. 
 * We first check to see if our output buffer has packets to
 * print if it doesn't we just return. 
 * 
 * Otherwise we attempt to empty the buffer and send an acknowledgement
 * afterwards.
 */
void
rel_output (rel_t *r)
{
  if(r->numPktsToOutput == 0) return;
  emptyBuffer(r);
  sendAckPacket(r);
}


/*
 * Function: retransmit
 * Usage: retransmit(s, &(tempPtr->dataPkt));
 * -------------------------------------------
 * This function retransmits any packet that has timed out.
 */
void retransmit(rel_t *s, packet_t *pkt){
  convertToNetworkByteOrder(pkt);
  conn_sendpkt(s->c, pkt, ntohs(pkt->len));
  convertToHostByteOrder(pkt);

}



/*
 * Function: hasTimedOut
 * Usage: if(hasTimedOut(s,currTime))...
 * --------------------------------------------------------
 * This function checks if the time elapsed since we sent our
 * packet to the current time is long enough for us to retransmit
 * the packet, if it is the function returns true, otherwise
 * it returns false.
 */
void checkRetransmission(rel_t *s, struct timespec currTime){
  dataPktNode *tempPtr;
  tempPtr = s->packetsInFlight; 
  while(tempPtr){
    if((!tempPtr->pktIsEmpty)){
      if(tempPtr->dataPkt.seqno == s->lastAcknoReceived){
	if(currTime.tv_sec - tempPtr->timeSent.tv_sec > (s->stateConfig.timeout/MILLI)){
	  retransmit(s, &(tempPtr->dataPkt));
	  clock_gettime(CLOCK_MONOTONIC,&(tempPtr->timeSent));
	  tempPtr->pktIsEmpty = false;
	}
      }
    }
    tempPtr = tempPtr->next;
  } 
  
}


/*
 * Function: rel_timer
 * ----------------------------------------------------------
 * This function is called after 1/5th of the timeout. When it
 * is called we loop through all states within rel_list and
 * check if a state is waiting for an ack. If it is waiting
 * for an ack then we get the current time, check if the packet
 * had timed out in which case we retransmit the packet by calling
 * the sendpacket with a boolean value true indicating that we
 * are retransmitting the packet.
 */
void
rel_timer ()
{
  
  struct timespec currTime;
  rel_t *s = rel_list;
  while(s){
    clock_gettime(CLOCK_MONOTONIC,&(currTime));
    checkRetransmission(s,currTime);
    s = s->next;
  }
  
 
}

