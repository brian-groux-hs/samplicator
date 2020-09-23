#include "config.h"

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>
#ifdef HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif
#include <stdio.h>
#include <string.h>
#include <errno.h>
#if STDC_HEADERS
# define bzero(b,n) memset(b,0,n)
#else
# include <strings.h>
# ifndef HAVE_STRCHR
#  define strchr index
# endif
# ifndef HAVE_MEMCPY
#  define memcpy(d, s, n) bcopy ((s), (d), (n))
#  define memmove(d, s, n) bcopy ((s), (d), (n))
# endif
#endif
#ifdef HAVE_CTYPE_H
# include <ctype.h>
#endif
#include <pthread.h>
#include <fcntl.h>

#include "samplicator.h"
#include "read_config.h"
#include "rawsend.h"
#include "inet.h"

#if defined (SO_RCVBUFFORCE)
const int RCVBUF_FLAG = SO_RCVBUFFORCE;
#else
const int RCVBUF_FLAG = SO_RCVBUF;
#endif

static int send_pdu_to_receiver (struct receiver *, const void *, size_t,
				 struct sockaddr *);
static int init_samplicator (struct samplicator_context *);
static int start_samplicate(struct samplicator_context *);
static void* samplicate (void *);
static int make_udp_socket (long, int, int);
static int make_recv_socket (struct samplicator_context *, int *);
static int make_send_sockets (struct samplicator_context *);
static void transmit (receive_work_unit_t*, int, int);

// used to differeniate between empty high bit and \0
const unsigned char LEN_MASK = (unsigned char)(1 << 7);

// number of bytes consumed to write length of packet
const int LEN_BYTES = 2;

int
main (argc, argv)
     int argc;
     const char **argv;
{
  struct samplicator_context ctx;

  if (parse_args (argc, (const char **) argv, &ctx) == -1)
    {
      exit (1);
    }
  if (init_samplicator (&ctx) == -1)
    exit (1);
  if (start_samplicate (&ctx) != 0) /* actually, samplicate() should never return. */
    exit (1);
  exit (0);
}

static int
daemonize (void)
{
  pid_t pid;

  pid = fork();
  if (pid == -1)
    {
      fprintf (stderr, "failed to fork process\n");
      exit (1);
    }
  else if (pid > 0)
    { /* kill the parent */
      exit (0);
    }
  else
    { /* end interaction with shell */
      fclose (stdin);
      fclose (stdout);
      fclose (stderr);
    }
  return 0;
}

static int
write_pid_file (const char *filename)
{
  FILE *fp;

  unlink (filename);	/* Ignore results - the old file may not exist. */
  if ((fp = fopen (filename, "w")) == 0)
    {
      fprintf (stderr, "Failed to create PID file %s: %s\n",
	       filename, strerror (errno));
      return -1;
    }
  if (fprintf (fp, "%ld\n", (long) getpid ()) <= 0)
    {
      fprintf (stderr, "Failed to write PID to PID file %s: %s\n",
	       filename, strerror (errno));
      return -1;
    }
  if (fclose (fp) == EOF)
    {
      fprintf (stderr, "Error closing PID file %s: %s\n",
	       filename, strerror (errno));
      return -1;
    }
  return 0;
}

/*
 make_recv_socket(ctx)

 Create the socket on which samplicator receives its packets.

 There can only be one.  This will be either a wildcard socket
 listening on a specific port on all interfaces, or a socket bound to
 a specific address (and, thus, interface).

 The creation of this socket is affected by the preferences in CTX:

 CTX->faddr_spec
   This is either a null pointer, meaning that a wildcard socket
   should be created, or a hostname or address literal specifying
   which address to listen on.  If this maps to multiple addresses,
   the socket will be bound to the first of those addresses that it
   can be bound to, in the order returned by getaddrinfo().

 CTX->fport_spec
   This must be a string, and specifies the port number or service
   name on which the socket will listen.

 CTX->ipv4_only
   If this is non-zero, the socket will be an IPv4 socket.  An error
   will be signaled if faddr_spec doesn't map to an IPv4 address.

 CTX->ipv6_only
   If non zero, only IPv6 addresses will be considered.

 If ipv4_only and ipv6_only are both zero, and faddr_spec is also
 null, then the receive socket will be an IPv6 socket bound to a
 specific port on all interfaces.  This socket will be able to receive
 packets over both IPv6 and IPv4.

 CTX->sockbuflen
   If this is non-zero, the function will try to set the socket's
   receiver buffer size to this many bytes.  If setting the socket
   buffer fails, a warning will be printed, but the socket will still
   be created.  The idea here is that a socket with an incorrect
   buffer size is more useful than no socket at all, although some
   people may differ.

 RETURN VALUE

 If a socket could be created and bound, this function will return
 zero.  If this was not possible, the function will produce an error
 message and return -1.
 */
static int
make_recv_socket (ctx, psock)
     struct samplicator_context *ctx;
	 int *psock;
{
  struct addrinfo hints, *res;
  int result;

  init_hints_from_preferences (&hints, ctx);
  if ((result = getaddrinfo (ctx->faddr_spec, ctx->fport_spec, &hints, &res)) != 0)
    {
      fprintf (stderr, "Failed to resolve IP address/port (%s:%s): %s\n",
	       ctx->faddr_spec, ctx->fport_spec, gai_strerror (result));
      return -1;
    }
  for (; res; res = res->ai_next)
    {
      if ((*psock = socket (res->ai_family, SOCK_DGRAM, 0)) < 0)
	{
	  fprintf (stderr, "socket(): %s\n", strerror (errno));
	  break;
	}
      if (setsockopt (*psock, SOL_SOCKET, RCVBUF_FLAG,
		      (char *) &ctx->sockbuflen, sizeof ctx->sockbuflen) == -1)
	{
	  fprintf (stderr, "Warning: setsockopt(SO_RCVBUF(FORCE),%ld) failed: %s\n",
		   ctx->sockbuflen, strerror (errno));
	}
      int enable = 1;
	  if (setsockopt (*psock, SOL_SOCKET, SO_REUSEPORT | SO_REUSEADDR, &enable, sizeof(int)) == -1)
	{
	  fprintf (stderr, "Warning: setsockopt(SO_REUSEPORT | SO_REUSEADDR) failed: %s\n",
		    strerror (errno));
	}
      struct timeval tv;
	  tv.tv_sec = 2;
	  if (setsockopt (*psock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(struct timeval)) == -1)
	{
	  fprintf (stderr, "Warning: setsockopt(SO_RCVTIMEO) failed: %s\n",
		    strerror (errno));
	}
      if (bind (*psock,
		(struct sockaddr*)res->ai_addr, res->ai_addrlen) < 0)
	{
	  fprintf (stderr, "bind(): %s\n", strerror (errno));
	  break;
	}
      ctx->fsockaddrlen = res->ai_addrlen;
      return 0;
    }
  return -1;
}

/* init_samplicator: prepares receiving socket */
static int
init_samplicator (ctx)
     struct samplicator_context *ctx;
{
  struct source_context *sctx;
  int i;

  /* check is there actually at least one configured data receiver */
  for (i = 0, sctx = ctx->sources; sctx != NULL; sctx = sctx->next)
    {
      i += sctx->nreceivers;
    }
  if (i == 0)
    {
      fprintf(stderr, "You have to specify at least one receiver, exiting\n");
      return -1;
    }

  if (make_send_sockets (ctx) != 0)
    {
      return -1;
    }

  if (ctx->fork == 1)
    daemonize ();
  if (ctx->pid_file != 0)
    {
      if (write_pid_file (ctx->pid_file) != 0)
	{
	  return -1;
	}
    }
  return 0;
}

static int
match_addr_p (struct sockaddr *input_generic,
	      struct sockaddr *addr_generic,
	      struct sockaddr *mask_generic)
{
#define SPECIALIZE(VAR, STRUCT) \
  struct STRUCT *VAR = (struct STRUCT *) VAR ## _generic

  if (addr_generic->sa_family == AF_INET)
    {
      SPECIALIZE (addr, sockaddr_in);
      SPECIALIZE (mask, sockaddr_in);
      if (addr->sin_addr.s_addr == 0)
	return 1;
      if (input_generic->sa_family == AF_INET)
	{
	  SPECIALIZE (input, sockaddr_in);
	  if ((input->sin_addr.s_addr & mask->sin_addr.s_addr) == addr->sin_addr.s_addr)
	    return 1;
	  return 0;
	}
      else if (input_generic->sa_family == AF_INET6)
	{
	  SPECIALIZE (input, sockaddr_in6);
	  if (IN6_IS_ADDR_V4MAPPED (&input->sin6_addr))
	    {
	      abort ();		/* TODO */
	    }
	  else
	    {
	      return 0;
	    }
	}
      else
	abort ();		/* Unexpected address family */
    }
  else
    {
      SPECIALIZE (addr, sockaddr_in6);
      SPECIALIZE (mask, sockaddr_in6);

      if (IN6_IS_ADDR_UNSPECIFIED (&mask->sin6_addr))
	{
	  return 1;
	}
      else if (input_generic->sa_family == AF_INET)
	{
	  abort ();
	}
      else if (input_generic->sa_family == AF_INET6)
	{
	  SPECIALIZE (input, sockaddr_in6);
	  unsigned k;
	  for (k = 0; k < 16; ++k)
	    {
	      if ((input->sin6_addr.s6_addr[k] & mask->sin6_addr.s6_addr[k])
		  != addr->sin6_addr.s6_addr[k])
		{
		  return 0;
		}
	      return 1;
	    }
	  abort ();
	}
      else
	abort ();		/* Unexpected address family */
    }
#undef SPECIALIZE
}

static void
handle_receive (unit_wrapper)
	void * unit_wrapper;
{
	receive_work_unit_t *unit = (receive_work_unit_t*)unit_wrapper;

	char next = unit->buffer[0];
	int numtx = 0;
	int len = 0;
	int i = 0;

	while (unit->buffer[i] != '\0')
	{
		// read length
		len = ((unit->buffer[i] & ~LEN_MASK) << 8) | (unit->buffer[i+1]);
		i += 2;

		// process buffer
		transmit(unit, i, len);

		// move to next boundary
		i += len;
		numtx++;
	}

	if (unit->ctx->async == 1)
	{
	  free(unit->buffer);
	  free(unit);
	  pthread_exit(NULL);
	}
}

static void
transmit (unit, buff_ptr, length)
	receive_work_unit_t *unit;
	int buff_ptr;
	int length;
{
	unsigned i;
	int freq_count = 0;
	struct source_context *sctx;

	// used to retireve names for logging
	char host[INET6_ADDRSTRLEN];
	char serv[6];

	  for (sctx = unit->ctx->sources; sctx != NULL; sctx = sctx->next)
	{
	  if (match_addr_p ((struct sockaddr *) &unit->remote_address,
			    (struct sockaddr *) &sctx->source,
			    (struct sockaddr *) &sctx->mask))
	    {
	      sctx->matched_packets += 1;
	      sctx->matched_octets += length;

	      for (i = 0; i < sctx->nreceivers; ++i)
		{
		  struct receiver *receiver = &(sctx->receivers[i]);

		  if (receiver->freqcount == 0)
		    {
		      if (send_pdu_to_receiver (receiver, unit->buffer + buff_ptr, length, (struct sockaddr *) &unit->remote_address)
			  == -1)
			{
			  receiver->out_errors += 1;
			  if (getnameinfo ((struct sockaddr *) &receiver->addr,
					   receiver->addrlen,
					   host, INET6_ADDRSTRLEN,
					   serv, 6,
					   NI_NUMERICHOST|NI_NUMERICSERV)
			      == -1)
			    {
			      strcpy (host, "???");
			      strcpy (serv, "?????");
			    }
			  fprintf (stderr, "sending datagram to %s:%s failed: %s\n",
				   host, serv, strerror (errno));
			}
		      else
			{
			  receiver->out_packets += 1;
			  receiver->out_octets += length;

			  if (unit->ctx->debug)
			    {
			      if (getnameinfo ((struct sockaddr *) &receiver->addr,
					       receiver->addrlen,
					       host, INET6_ADDRSTRLEN,
					       serv, 6,
					       NI_NUMERICHOST|NI_NUMERICSERV)
				  == -1)
				{
				  strcpy (host, "???");
				  strcpy (serv, "?????");
				}
			      fprintf (stderr, "  sent to %s:%s\n", host, serv);
			    }
			}
		      receiver->freqcount = receiver->freq-1;
		    }
		  else
		    {
		      receiver->freqcount -= 1;
		    }
		  if (sctx->tx_delay)
		    usleep (sctx->tx_delay);
		}
	    }
	  else
	    {
	      if (unit->ctx->debug)
		{
		  if (getnameinfo ((struct sockaddr *) &sctx->source,
				   sctx->addrlen,
				   host, INET6_ADDRSTRLEN,
				   0, 0,
				   NI_NUMERICHOST|NI_NUMERICSERV)
		      == -1)
		    {
		      strcpy (host, "???");
		    }
		  fprintf (stderr, "Not matching %s/", host);
		  if (getnameinfo ((struct sockaddr *) &sctx->mask,
				   sctx->addrlen,
				   host, INET6_ADDRSTRLEN,
				   0, 0,
				   NI_NUMERICHOST|NI_NUMERICSERV)
		      == -1)
		    {
		      strcpy (host, "???");
		    }
		  fprintf (stderr, "%s\n", host);
		}
	    }
	}
}

static int
start_samplicate(ctx)
	struct samplicator_context *ctx;
{
	int i;
	pthread_t *pthread_ids = malloc(ctx->workers * sizeof(pthread_t));;

	for (i=0; i < ctx->workers; i++)
		pthread_create(&pthread_ids[i], NULL, samplicate, ctx);

    for (i = 0; i < ctx->workers; i++)
    	pthread_join(pthread_ids[i], NULL);

	return 0;
}

void*
samplicate (obj_param)
     void *obj_param;
{
  struct samplicator_context *ctx = (struct samplicator_context *)obj_param;
  int fsockfd;

  if (make_recv_socket (ctx, &fsockfd) != 0)
	return -1;

  int n;
  char host[INET6_ADDRSTRLEN];
  char serv[6];
  int mtu = ctx->pdulen * sizeof(unsigned char);
  int buff_ptr = 0;
  long buffer_size = ctx->flush_threshold;

  // used in this func only
  socklen_t addrlen;

  // create a unit of work for the recieve function
  receive_work_unit_t *unit = NULL;

  // tmp pointer
  void* tmp;
  int force_flush = 0;

  // used when we create new threads
  pthread_t thread_id;

  while (1)
    {
      if (ctx->timeout)
      {
          struct pollfd fds[1];
          fds[0].fd=fsockfd;
          fds[0].events=POLLIN;
          int timeout_rc=poll(fds, 1, ctx->timeout);
          if (!timeout_rc)
          {
              fprintf (stderr, "Timeout, no data received in %d milliseconds.\n",
                  ctx->timeout);
              exit (5);
          }
      }

	   if (buff_ptr == 0)
	 {
	   unit = malloc(sizeof(receive_work_unit_t));
	   unit->buffer = malloc(buffer_size * sizeof(unsigned char*));
	   unit->ctx = ctx;
	 }

      addrlen = sizeof(unit->remote_address);

      // recv and don't wait if there is data in the buffer waiting to be tx'd,
	  // this drains the socket buffer until empty then waits for another pdu
	  if ((n = recvfrom (fsockfd, unit->buffer + buff_ptr + 2,
			 mtu, MSG_TRUNC,
			 (struct sockaddr *) &unit->remote_address, &addrlen)) == -1)
	{
		// if socket was nonblocking errno is set
		//  to EAGAIN or EWOULDBLOCK when recived == 0 bytes
	    if (errno != EAGAIN && errno != EWOULDBLOCK )
	  {
		fprintf (stderr, "recvfrom(): %s\n", strerror(errno));
		exit (1);
	  }
	  	else
	  {
		  force_flush = 1;
	  }
	}
      if (n > ctx->pdulen)
	{
	  fprintf (stderr, "Warning: %ld excess bytes discarded\n",
		n-ctx->pdulen);
	  n = ctx->pdulen;
	}
      if (n != -1 && addrlen != ctx->fsockaddrlen)
	{
	  fprintf (stderr, "recvfrom() return address length %lu - expected %lu\n",
		   (unsigned long) addrlen, (unsigned long) ctx->fsockaddrlen);
	  exit (1);
	}
      if (ctx->debug)
	{
	  if (getnameinfo ((struct sockaddr *) &unit->remote_address, addrlen,
			   host, INET6_ADDRSTRLEN,
			   serv, 6,
			   NI_NUMERICHOST|NI_NUMERICSERV) == -1)
	    {
	      strcpy (host, "???");
	      strcpy (serv, "?????");
	    }
	  fprintf (stderr, "received %d bytes from %s:%s\n", n, host, serv);
	}
		// write length to buffer, if we actually recieved data
		  if (n > 0)
		{
			unit->buffer[buff_ptr] = LEN_MASK | (n >> 8);
			unit->buffer[buff_ptr + 1] = n & 0x000000FF;
			buff_ptr += (n + LEN_BYTES);
		}

		  if (ctx->async == 0)
		{
			if (n > 0) {
				// terminate the bufffer and tx each pdu when running synchronously
				unit->buffer[buff_ptr]= '\0';
				handle_receive(unit);
				buff_ptr = 0;
			}
		}
		  else if (buff_ptr + LEN_BYTES + mtu >= buffer_size || force_flush)
		{
			// terminate the buffer and tx on a transient thread when;
			// buffer cannot hold another pdu OR the socket has run dry
			unit->buffer[buff_ptr]= '\0';
			pthread_create(&thread_id, NULL, handle_receive, unit);
			pthread_detach(thread_id);
			buff_ptr = 0;
			force_flush = 0;
		}
    }
}

static int
send_pdu_to_receiver (receiver, fpdu, length, source_addr)
     struct receiver * receiver;
     const void * fpdu;
     size_t length;
     struct sockaddr * source_addr;
{
  if (receiver->flags & pf_SPOOF)
    {
      int rawsend_flags
	= ((receiver->flags & pf_CHECKSUM) ? RAWSEND_COMPUTE_UDP_CHECKSUM : 0);
      return raw_send_from_to (receiver->fd, fpdu, length,
			       (struct sockaddr *) source_addr,
			       (struct sockaddr *) &receiver->addr,
			       receiver->ttl, rawsend_flags);
    }
  else
    {
      return sendto (receiver->fd, (char*) fpdu, length, 0,
		     (struct sockaddr*) &receiver->addr, receiver->addrlen);
    }
}

static int
make_cooked_udp_socket (long sockbuflen, int af)
{
  int s;
  if ((s = socket (af == AF_INET ? PF_INET : PF_INET6, SOCK_DGRAM, 0)) == -1)
    return s;
  if (sockbuflen != -1)
    {
      if (setsockopt (s, SOL_SOCKET, SO_SNDBUF,
		      (char *) &sockbuflen, sizeof sockbuflen) == -1)
	{
	  fprintf (stderr, "setsockopt(SO_SNDBUF,%ld): %s\n",
		   sockbuflen, strerror (errno));
	}
    }
  return s;
}

static int
make_udp_socket (long sockbuflen, int raw, int af)
{
  return raw
    ? make_raw_udp_socket (sockbuflen, af)
    : make_cooked_udp_socket (sockbuflen, af);
}

static int
make_send_sockets (struct samplicator_context *ctx)
{
  /* Array of four sockets:

     First index: cooked(0)/raw(1)
     Second index: IPv4(0)/IPv6(1)

     At a maximum, we need one socket of each kind.  These sockets can
     be used by multiple receivers of the same type.
   */
  int socks[2][2] = { { -1, -1 }, { -1, -1 } };

  struct source_context *sctx;
  unsigned i;

  for (sctx = ctx->sources; sctx != 0; sctx = sctx->next)
    {
      for (i = 0; i < sctx->nreceivers; ++i)
	{
	  struct receiver *receiver = &sctx->receivers[i];
	  int af = receiver->addr.ss_family;
	  int af_index = af == AF_INET ? 0 : 1;
	  int spoof_p = receiver->flags & pf_SPOOF;

	  if (socks[spoof_p][af_index] == -1)
	    {
	      if ((socks[spoof_p][af_index] = make_udp_socket (ctx->sockbuflen, spoof_p, af)) < 0)
		{
		  if (spoof_p && errno == EPERM)
		    {
		      fprintf (stderr, "Not enough privilege for -S option---try again as root.\n");
		      return -1;
		    }
		  else
		    {
		      fprintf (stderr, "Error creating%s socket: %s\n",
			       spoof_p ? " raw" : "", strerror (errno));
		    }
		  return -1;
		}
	    }
	  receiver->fd = socks[spoof_p][af_index];
	}
    }
  return 0;
}
