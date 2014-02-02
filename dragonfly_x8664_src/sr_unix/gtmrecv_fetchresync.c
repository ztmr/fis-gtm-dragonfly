/****************************************************************
 *								*
 *	Copyright 2001, 2012 Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "gtm_ipc.h"
#include "gtm_socket.h"
#include "gtm_string.h"
#include "gtm_inet.h"
#include "gtm_stdio.h"

#include <sys/un.h>
#include "gtm_time.h" /* needed for difftime() definition; if this file is not included, difftime returns bad values on AIX */
#include <sys/time.h>
#include <errno.h>
#include "gtm_fcntl.h"
#include "gtm_unistd.h"
#include <sys/shm.h>
#include <sys/wait.h>

#include "gdsroot.h"
#include "gdsblk.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsbt.h"
#include "gdsfhead.h"
#include "filestruct.h"
#include "jnl.h"
#include "buddy_list.h"		/* needed for muprec.h */
#include "hashtab_mname.h"	/* needed for muprec.h */
#include "hashtab_int4.h"	/* needed for muprec.h */
#include "hashtab_int8.h"	/* needed for muprec.h */
#include "repl_sem.h"
#include "muprec.h"
#include "error.h"
#include "iosp.h"
#include "gtmrecv.h"
#include "repl_comm.h"
#include "repl_msg.h"
#include "repl_errno.h"
#include "repl_dbg.h"
#include "gtm_logicals.h"
#include "eintr_wrappers.h"
#include "repl_sp.h"
#include "repl_log.h"
#include "io.h"
#include "trans_log_name.h"
#include "util.h"
#include "gtmsource.h"
#include "repl_instance.h"
#include "iotcpdef.h"
#include "gtmio.h"
#include "replgbl.h"

#define MAX_ATTEMPTS_FOR_FETCH_RESYNC	60 /* max-wait in seconds for source server response after connection is established */
#define MAX_WAIT_FOR_FETCHRESYNC_CONN	60 /* max-wait in seconds to establish connection with the source server */
#define FETCHRESYNC_PRIMARY_POLL	(MICROSEC_IN_SEC - 1) /* micro seconds, almost 1 second */

GBLREF	uint4			process_id;
GBLREF	int			recvpool_shmid;
GBLREF	int			gtmrecv_listen_sock_fd, gtmrecv_sock_fd;
GBLREF	struct sockaddr_in	primary_addr;
GBLREF	seq_num			seq_num_zero;
GBLREF	jnl_gbls_t		jgbl;
GBLREF	int			repl_max_send_buffsize, repl_max_recv_buffsize;
GBLREF	jnlpool_addrs		jnlpool;
GBLREF	boolean_t		repl_connection_reset;
GBLREF 	mur_gbls_t		murgbl;
GBLREF	boolean_t		holds_sem[NUM_SEM_SETS][NUM_SRC_SEMS];
GBLREF	repl_conn_info_t	*remote_side;
GBLREF	int4			strm_index;

error_def(ERR_PRIMARYNOTROOT);
error_def(ERR_RECVPOOLSETUP);
error_def(ERR_REPL2OLD);
error_def(ERR_REPLCOMM);
error_def(ERR_REPLINSTNOHIST);
error_def(ERR_TEXT);

CONDITION_HANDLER(gtmrecv_fetchresync_ch)
{
	int	rc;

	START_CH;
	if (FD_INVALID != gtmrecv_listen_sock_fd)
		CLOSEFILE_RESET(gtmrecv_listen_sock_fd, rc);	/* resets "gtmrecv_listen_sock_fd" to FD_INVALID */
	if (FD_INVALID != gtmrecv_sock_fd)
		CLOSEFILE_RESET(gtmrecv_sock_fd, rc);	/* resets "gtmrecv_sock_fd" to FD_INVALID */
	PRN_ERROR;
	NEXTCH;
}

int gtmrecv_fetchresync(int port, seq_num *resync_seqno, seq_num max_reg_seqno)
{
	GTM_SOCKLEN_TYPE		primary_addr_len;
	repl_resync_msg_t		resync_msg;
	repl_msg_t			msg;
	uchar_ptr_t			msgp;
	unsigned char			*msg_ptr;				/* needed for REPL_{SEND,RECV}_LOOP */
	int				tosend_len, sent_len, sent_this_iter;	/* needed for REPL_SEND_LOOP */
	int				torecv_len, recvd_len, recvd_this_iter;	/* needed for REPL_RECV_LOOP */
	int				status;					/* needed for REPL_{SEND,RECV}_LOOP */
	fd_set				input_fds;
	int				wait_count;
	char				seq_num_str[32], *seq_num_ptr;
	pid_t				rollback_pid;
	int				rollback_status;
	int				wait_status;
	time_t				t1, t2;
	struct timeval			gtmrecv_fetchresync_max_wait, gtmrecv_fetchresync_poll, sel_timeout_val;
	repl_old_instinfo_msg_t		old_instinfo_msg;
	repl_old_needinst_msg_ptr_t	old_need_instinfo_msg;
	repl_needinst_msg_t		need_instinfo_msg;
	repl_needhistinfo_msg_ptr_t	need_histinfo_msg;
	repl_histinfo			histinfo;
	seq_num				histinfo_seqno;
	short				retry_num;
	repl_inst_hdr_ptr_t		inst_hdr;
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	repl_log(stdout, TRUE, TRUE, "Assuming primary supports multisite functionality. Connecting "
		"using multisite communication protocol.\n");
	ESTABLISH_RET(gtmrecv_fetchresync_ch, (!SS_NORMAL));
	QWASSIGN(*resync_seqno, seq_num_zero);
	gtmrecv_fetchresync_max_wait.tv_sec = MAX_WAIT_FOR_FETCHRESYNC_CONN;
	gtmrecv_fetchresync_max_wait.tv_usec = 0;
	gtmrecv_fetchresync_poll.tv_sec = 0;
	gtmrecv_fetchresync_poll.tv_usec = FETCHRESYNC_PRIMARY_POLL;
	gtmrecv_comm_init(port);

	primary_addr_len = SIZEOF(primary_addr);
	remote_side->proto_ver = REPL_PROTO_VER_UNINITIALIZED;
	repl_log(stdout, TRUE, TRUE, "Waiting for a connection...\n");
	FD_ZERO(&input_fds);
	FD_SET(gtmrecv_listen_sock_fd, &input_fds);
	/* Note - the following call to select checks for EINTR. The SELECT macro is not used because
	 * the code also checks for EAGAIN and takes action before retrying the select.
	 */
	t1 = time(NULL);
	while ((status = select(gtmrecv_listen_sock_fd + 1, &input_fds, NULL, NULL, &gtmrecv_fetchresync_max_wait)) < 0)
	{
		if ((EINTR == errno)  || (EAGAIN == errno))
		{
			t2 = time(NULL);
			if (0 >= (int)(gtmrecv_fetchresync_max_wait.tv_sec =
					(MAX_WAIT_FOR_FETCHRESYNC_CONN - (int)difftime(t2, t1))))
			{
				status = 0;
				break;
			}
			gtmrecv_fetchresync_max_wait.tv_usec = 0;
			FD_SET(gtmrecv_listen_sock_fd, &input_fds);
			continue;
		} else
			rts_error(VARLSTCNT(7) ERR_REPLCOMM, 0, ERR_TEXT, 2,
					RTS_ERROR_LITERAL("Error in select on listen socket"), errno);
	}
	if (status == 0)
	{
		repl_log(stdout, TRUE, TRUE, "Waited about %d seconds for connection from primary source server\n",
			MAX_WAIT_FOR_FETCHRESYNC_CONN);
		rts_error(VARLSTCNT(6) ERR_REPLCOMM, 0, ERR_TEXT, 2,
			RTS_ERROR_LITERAL("Waited too long to get a connection request. Check if primary is alive."));
	}
	ACCEPT_SOCKET(gtmrecv_listen_sock_fd, (struct sockaddr *)&primary_addr,
								(GTM_SOCKLEN_TYPE *)&primary_addr_len, gtmrecv_sock_fd);
	if (0 > gtmrecv_sock_fd)
	{
#		ifdef __hpux
		/* ENOBUFS in HP-UX is either because of a memory problem or when we have received a RST just
		after a SYN before an accept call. Normally this is not fatal and is just a transient state.Hence
		exiting just after a single error of this kind should not be done. So retry in case of HP-UX and ENOBUFS error*/
		if (ENOBUFS == errno)
		{
			retry_num = 0;
			/*In case of succeeding with select in first go, accept will still get 5ms time difference*/
			while (HPUX_MAX_RETRIES > retry_num)
			{
				SHORT_SLEEP(5);
				FD_ZERO(&input_fds);
				FD_SET(gtmrecv_listen_sock_fd, &input_fds);
				/* Since we use Blocking socket, check before re-trying if there is a connection to be accepted.
				 * Timeout of HPUX_SEL_TIMEOUT.  In case the earlier connection is not available there can be
				 * some time gap between the time the error occured and the new client requests coming in.
				 */
				for ( ; HPUX_MAX_RETRIES > retry_num; retry_num++)
				{
					  FD_ZERO(&input_fds);
					  FD_SET(gtmrecv_listen_sock_fd, &input_fds);
					  sel_timeout_val.tv_sec = 0;
					  sel_timeout_val.tv_usec = HPUX_SEL_TIMEOUT;
					  status = select(gtmrecv_listen_sock_fd + 1, &input_fds, NULL,
							NULL, &sel_timeout_val);
					  if (0 < status)
						  break;
					  else
						 SHORT_SLEEP(5);
				}
				if (0 > status)
					 rts_error(VARLSTCNT(7) ERR_REPLCOMM, 0, ERR_TEXT, 2,
				      RTS_ERROR_LITERAL("Error in select on listen socket after ENOBUFS error"), errno);
				else
				{
					 ACCEPT_SOCKET(gtmrecv_listen_sock_fd, (struct sockaddr *)&primary_addr,
							       (GTM_SOCKLEN_TYPE *)&primary_addr_len, gtmrecv_sock_fd);
					if ((0 > gtmrecv_sock_fd) && (errno == ENOBUFS))
						retry_num++;
					else
						break;
				}
			  }
		}
		if (0 > gtmrecv_sock_fd)
#		endif
		{
			rts_error(VARLSTCNT(7) ERR_REPLCOMM, 0, ERR_TEXT, 2,
			RTS_ERROR_LITERAL("Error accepting connection from Source Server"), errno);
		}
	}
	repl_close(&gtmrecv_listen_sock_fd); /* Close the listener socket */
	repl_connection_reset = FALSE;
	if (0 != (status = get_send_sock_buff_size(gtmrecv_sock_fd, &repl_max_send_buffsize))
		|| 0 != (status = get_recv_sock_buff_size(gtmrecv_sock_fd, &repl_max_recv_buffsize)))
	{
		rts_error(VARLSTCNT(7) ERR_REPLCOMM, 0, ERR_TEXT, 2,
			LEN_AND_LIT("Error getting socket send/recv buffsizes"), status);
		return ERR_REPLCOMM;
	}
	repl_log(stdout, TRUE, TRUE, "Connection established, using TCP send buffer size %d receive buffer size %d\n",
			repl_max_send_buffsize, repl_max_recv_buffsize);
	repl_log_conn_info(gtmrecv_sock_fd, stdout);

	/* Send REPL_FETCH_RESYNC message */
	memset(&resync_msg, 0, SIZEOF(resync_msg));
	resync_msg.resync_seqno = max_reg_seqno;
	assert(resync_msg.resync_seqno);
	resync_msg.proto_ver = REPL_PROTO_VER_THIS;
	resync_msg.node_endianness = NODE_ENDIANNESS;
	resync_msg.is_supplementary = jnlpool.repl_inst_filehdr->is_supplementary;
	remote_side->endianness_known = FALSE;
	remote_side->cross_endian = FALSE;
	gtmrecv_repl_send((repl_msg_ptr_t)&resync_msg, REPL_FETCH_RESYNC, MIN_REPL_MSGLEN,
				"REPL_FETCH_RESYNC", resync_msg.resync_seqno);
	if (repl_connection_reset)
	{	/* Connection got reset during the above send */
		rts_error(VARLSTCNT(1) ERR_REPLCOMM);
		return ERR_REPLCOMM;
	}
	/* Wait for REPL_RESYNC_SEQNO (if dual-site primary) or REPL_OLD_NEED_INSTANCE_INFO (if multi-site primary)
	 * or REPL_NEED_INSTINFO (if multi-site primary with supplementary instance support) message */
	do
	{
		wait_count = MAX_ATTEMPTS_FOR_FETCH_RESYNC;
		assert(SIZEOF(msg) == MIN_REPL_MSGLEN);
		REPL_RECV_LOOP(gtmrecv_sock_fd, &msg, MIN_REPL_MSGLEN, FALSE, &gtmrecv_fetchresync_poll)
		{
			if (0 >= wait_count)
				break;
			repl_log(stdout, TRUE, TRUE, "Waiting for REPL_RESYNC_SEQNO or REPL_OLD_NEED_INSTANCE_INFO "
				" or REPL_NEED_INSTINFO or REPL_NEED_HISTINFO\n");
			wait_count--;
		}
		if (status != SS_NORMAL)
		{
			if (EREPL_RECV == repl_errno)
				rts_error(VARLSTCNT(7) ERR_REPLCOMM, 0, ERR_TEXT, 2,
					RTS_ERROR_LITERAL("Error receiving RESYNC JNLSEQNO. Error in recv"), status);
			if (EREPL_SELECT == repl_errno)
				rts_error(VARLSTCNT(7) ERR_REPLCOMM, 0, ERR_TEXT, 2,
					RTS_ERROR_LITERAL("Error receiving RESYNC JNLSEQNO. Error in select"), status);
		}
		if (wait_count <= 0)
			rts_error(VARLSTCNT(6) ERR_REPLCOMM, 0, ERR_TEXT, 2,
				LEN_AND_LIT("Waited too long to get message from primary. Check if primary is alive."));
		if (!remote_side->endianness_known)
		{
			remote_side->endianness_known = TRUE;
			if ((REPL_MSGTYPE_LAST < msg.type) && (REPL_MSGTYPE_LAST > GTM_BYTESWAP_32(msg.type)))
			{
				remote_side->cross_endian = TRUE;
				repl_log(stdout, TRUE, TRUE, "Source and Receiver sides have opposite endianness\n");
			} else
			{
				remote_side->cross_endian = FALSE;
				repl_log(stdout, TRUE, TRUE, "Source and Receiver sides have same endianness\n");
			}
		}
		if (remote_side->cross_endian)
		{
			msg.type = GTM_BYTESWAP_32(msg.type);
			msg.len = GTM_BYTESWAP_32(msg.len);
		}
		switch(msg.type)
		{
			case REPL_OLD_NEED_INSTANCE_INFO:
				assert(NULL != jnlpool.repl_inst_filehdr);
				old_need_instinfo_msg = (repl_old_needinst_msg_ptr_t)&msg;
				repl_log(stdout, TRUE, TRUE, "Received REPL_OLD_NEED_INSTANCE_INFO message from primary "
					"instance [%s]\n", old_need_instinfo_msg->instname);
				if (jnlpool.repl_inst_filehdr->is_supplementary)
				{	/* Issue REPL2OLD error because this is a supplementary instance and remote side runs
					 * on a GT.M version that does not understand the supplementary protocol */
					rts_error(VARLSTCNT(6) ERR_REPL2OLD, 4, LEN_AND_STR(old_need_instinfo_msg->instname),
						LEN_AND_STR(jnlpool.repl_inst_filehdr->inst_info.this_instname));
				}
				remote_side->proto_ver = old_need_instinfo_msg->proto_ver;
				assert(REPL_PROTO_VER_MULTISITE <= remote_side->proto_ver);
				assert(REPL_PROTO_VER_SUPPLEMENTARY > remote_side->proto_ver);
				memset(&old_instinfo_msg, 0, SIZEOF(old_instinfo_msg));
				memcpy(old_instinfo_msg.instname, jnlpool.repl_inst_filehdr->inst_info.this_instname,
					MAX_INSTNAME_LEN - 1);
				old_instinfo_msg.was_rootprimary = (unsigned char)repl_inst_was_rootprimary();
				murgbl.was_rootprimary = old_instinfo_msg.was_rootprimary;
				assert(MIN_REPL_MSGLEN == SIZEOF(old_instinfo_msg));
				gtmrecv_repl_send((repl_msg_ptr_t)&old_instinfo_msg, REPL_OLD_INSTANCE_INFO,
							MIN_REPL_MSGLEN, "REPL_OLD_INSTANCE_INFO", MAX_SEQNO);
				if (old_instinfo_msg.was_rootprimary && !old_need_instinfo_msg->is_rootprimary)
					rts_error(VARLSTCNT(4) ERR_PRIMARYNOTROOT, 2,
						LEN_AND_STR((char *)old_need_instinfo_msg->instname));
				break;

			case REPL_NEED_INSTINFO:
				/* We got only a part of this message. Get the remaining part as well */
				assert(SIZEOF(need_instinfo_msg) > MIN_REPL_MSGLEN);
				memcpy(&need_instinfo_msg, &msg, MIN_REPL_MSGLEN);
				msgp = (uchar_ptr_t)&need_instinfo_msg + MIN_REPL_MSGLEN;
				REPL_RECV_LOOP(gtmrecv_sock_fd, msgp,
					SIZEOF(need_instinfo_msg) - MIN_REPL_MSGLEN, FALSE, &gtmrecv_fetchresync_poll)
				{
					if (0 >= wait_count)
						break;
					repl_log(stdout, TRUE, TRUE, "Waiting for REPL_NEED_INSTINFO\n");
					wait_count--;
				}
				if (status != SS_NORMAL)
				{
					if (EREPL_RECV == repl_errno)
						rts_error(VARLSTCNT(7) ERR_REPLCOMM, 0, ERR_TEXT, 2,
							RTS_ERROR_LITERAL("Error in recv REPL_NEED_INSTINFO"), status);
					if (EREPL_SELECT == repl_errno)
						rts_error(VARLSTCNT(7) ERR_REPLCOMM, 0, ERR_TEXT, 2,
							RTS_ERROR_LITERAL("Error in select REPL_NEED_INSTINFO"), status);
				}
				if (wait_count <= 0)
					rts_error(VARLSTCNT(6) ERR_REPLCOMM, 0, ERR_TEXT, 2,
						LEN_AND_LIT("Waited too long to get message from primary."
							" Check if primary is alive."));
				gtmrecv_check_and_send_instinfo(&need_instinfo_msg, IS_RCVR_SRVR_FALSE);
				break;

			case REPL_NEED_HISTINFO:
			/* case REPL_NEED_TRIPLE_INFO: too but that message has been renamed to REPL_NEED_HISTINFO */
				need_histinfo_msg = RECAST(repl_needhistinfo_msg_ptr_t)&msg;
				/* In case of fetchresync rollback, the REPL_NEED_HISTINFO message asks for a seqno to be found.
				 * Not a specific histinfo number. The latter is valid only in the receiver. Assert this.
				 * Versions older than V55000 dont set this histinfo number so relax the assert accordingly.
				 */
				assert((INVALID_HISTINFO_NUM == need_histinfo_msg->histinfo_num)
					|| ((0 == need_histinfo_msg->histinfo_num)
						&& (REPL_PROTO_VER_SUPPLEMENTARY > remote_side->proto_ver)));
				assert(remote_side->endianness_known);	/* only then is remote_side->cross_endian reliable */
				if (!remote_side->cross_endian)
					histinfo_seqno = need_histinfo_msg->seqno;
				else
					histinfo_seqno = GTM_BYTESWAP_64(need_histinfo_msg->seqno);
				assert((INVALID_SUPPL_STRM == strm_index) || ((0 <= strm_index) && (MAX_SUPPL_STRMS > strm_index)));
				if (0 < strm_index)
				{	/* Source side is a non-supplementary instance. */
					repl_log(stdout, TRUE, TRUE, "Received REPL_NEED_HISTINFO message for "
						"Stream %d : Seqno %llu [0x%llx]\n", strm_index, histinfo_seqno, histinfo_seqno);
				} else
					repl_log(stdout, TRUE, TRUE, "Received REPL_NEED_HISTINFO message for "
						"Seqno %llu [0x%llx]\n", histinfo_seqno, histinfo_seqno);
				assert(REPL_PROTO_VER_UNINITIALIZED != remote_side->proto_ver);
				assert(NULL != jnlpool.jnlpool_dummy_reg);
				/* repl_inst_wrapper_triple_find_seqno needs the ftok lock on the replication instance file. But,
				 * But, ROLLBACK already holds JNLPOOL and RECVPOOL access semaphores and so asking for ftok lock
				 * can result in potential deadlocks. Since we hold the JNLPOOL and RECVPOOL semaphores, no one
				 * else can startup until we are done. So, assert that we hold the access control semaphores and
				 * proceed.
				 */
				ASSERT_HOLD_REPLPOOL_SEMS;
				status = repl_inst_wrapper_histinfo_find_seqno(histinfo_seqno, strm_index, &histinfo);
				if (0 != status)
				{	/* Close the connection. The function call above would have issued the error. */
					assert(ERR_REPLINSTNOHIST == status);
					repl_log(stdout, TRUE, TRUE, "Connection reset due to REPLINSTNOHIST error\n");
					repl_connection_reset = TRUE;
					repl_close(&gtmrecv_sock_fd);
					return status;
				}
				if (0 < strm_index)
				{	/* About to send to a non-supplementary instance. It does not understand strm_seqnos.
					 * So convert it back to a format it understands.
					 */
					CONVERT_SUPPL2NONSUPPL_HISTINFO(histinfo);
				}
				assert(histinfo.start_seqno < histinfo_seqno);
				gtmrecv_send_histinfo(&histinfo);
				break;

			case REPL_INST_NOHIST:
				repl_log(stdout, TRUE, TRUE, "Originating instance encountered a REPLINSTNOHIST error."
					" JNL_SEQNO of this replicating instance precedes the current history in the "
					"originating instance file. Rollback exiting.\n");
				status = ERR_REPLINSTNOHIST;
				repl_log(stdout, TRUE, TRUE, "Connection reset due to REPLINSTNOHIST error on primary\n");
				repl_connection_reset = TRUE;
				repl_close(&gtmrecv_sock_fd);
				return status;
				break;

			case REPL_RESYNC_SEQNO:
				repl_log(stdout, TRUE, TRUE, "Received REPL_RESYNC_SEQNO message\n");
				if (REPL_PROTO_VER_UNINITIALIZED == remote_side->proto_ver)
				{	/*  Issue REPL2OLD error because primary is dual-site */
					assert(NULL != jnlpool.repl_inst_filehdr);
					rts_error(VARLSTCNT(6) ERR_REPL2OLD, 4, LEN_AND_STR(UNKNOWN_INSTNAME),
						LEN_AND_STR(jnlpool.repl_inst_filehdr->inst_info.this_instname));
				}
				assert(REPL_PROTO_VER_MULTISITE <= remote_side->proto_ver);
				/* The following fields dont need to be initialized since they are needed (for internal filter
				 * transformations) only if we send journal records across. Besides we are going to close
				 * the connection now and proceed with the rollback.
				 *	remote_side->jnl_ver = ...
				 *	remote_side->is_std_null_coll = ...
				 *	remote_side->trigger_supported = ...
				 *	remote_side->null_subs_xform = ...
				 */
				break;

			default:
				repl_log(stdout, TRUE, TRUE, "Message of unknown type (%d) received\n", msg.type);
				assert(FALSE);
				rts_error(VARLSTCNT(1) ERR_REPLCOMM);
				break;
		}
	} while (!repl_connection_reset && (REPL_RESYNC_SEQNO != msg.type));
	if (repl_connection_reset)
	{	/* Connection got reset during the above send */
		rts_error(VARLSTCNT(1) ERR_REPLCOMM);
		return ERR_REPLCOMM;
	}
	assert(remote_side->endianness_known);	/* only then is remote_side->cross_endian reliable */
	if (!remote_side->cross_endian)
		QWASSIGN(*resync_seqno, *(seq_num *)&msg.msg[0]);
	else
		QWASSIGN(*resync_seqno, GTM_BYTESWAP_64(*(seq_num *)&msg.msg[0]));
	/* Wait till connection is broken or REPL_CONN_CLOSE is received */
	REPL_RECV_LOOP(gtmrecv_sock_fd, &msg, MIN_REPL_MSGLEN, FALSE, &gtmrecv_fetchresync_poll)
	{
		REPL_DPRINT1("FETCH_RESYNC : Waiting for source to send CLOSE_CONN or connection breakage\n");
	}
	repl_close(&gtmrecv_sock_fd);
	REVERT;
	repl_log(stdout, TRUE, TRUE, "Received RESYNC SEQNO is %llu [0x%llx]\n", *resync_seqno, *resync_seqno);
	assert(*resync_seqno <= max_reg_seqno);
	return SS_NORMAL;
}
