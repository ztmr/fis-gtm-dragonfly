#################################################################
#								#
#	Copyright 2001 Sanchez Computer Associates, Inc.	#
#								#
#	This source code contains the intellectual property	#
#	of its copyright holder(s), and is made available	#
#	under a license.  If you do not know the terms of	#
#	the license, please stop and do not read further.	#
#								#
#################################################################

#	PAGE	,132
	.title	opp_indrzshow.s

#	.386
#	.MODEL	FLAT, C

.include "linkage.si"
	.INCLUDE	"g_msf.si"

	.sbttl	opp_indrzshow
#	PAGE	+
	.DATA
.extern	frame_pointer 	# /* :DWORD */

	.text
.extern	op_indrzshow

# PUBLIC	opp_indrzshow
ENTRY opp_indrzshow  	# /* PROC */
	putframe
	addl	$4,%esp
	call	op_indrzshow
	addl	$8,%esp
	getframe
	ret
# opp_indrzshow ENDP

# END
