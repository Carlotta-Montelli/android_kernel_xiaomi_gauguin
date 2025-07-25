/*
 * Copy from user space to user space
 *
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/linkage.h>

#include <asm/cache.h>
#include <asm/asm-uaccess.h>

/*
 * Copy from user space to user space (alignment handled by the hardware)
 *
 * Parameters:
 *	x0 - to
 *	x1 - from
 *	x2 - n
 * Returns:
 *	x0 - bytes not copied
 */
	.macro ldrb1 reg, ptr, val
	uao_user_alternative 9997f, ldrb, ldtrb, \reg, \ptr, \val
	.endm

	.macro strb1 reg, ptr, val
	uao_user_alternative 9997f, strb, sttrb, \reg, \ptr, \val
	.endm

	.macro ldrh1 reg, ptr, val
	uao_user_alternative 9997f, ldrh, ldtrh, \reg, \ptr, \val
	.endm

	.macro strh1 reg, ptr, val
	uao_user_alternative 9997f, strh, sttrh, \reg, \ptr, \val
	.endm

	.macro ldr1 reg, ptr, val
	uao_user_alternative 9997f, ldr, ldtr, \reg, \ptr, \val
	.endm

	.macro str1 reg, ptr, val
	uao_user_alternative 9997f, str, sttr, \reg, \ptr, \val
	.endm

	.macro ldp1 reg1, reg2, ptr, val
	uao_ldp 9997f, \reg1, \reg2, \ptr, \val
	.endm

	.macro stp1 reg1, reg2, ptr, val
	uao_stp 9997f, \reg1, \reg2, \ptr, \val
	.endm

end	.req	x5
srcin	.req	x15

ENTRY(__arch_copy_in_user)
	uaccess_enable_not_uao x3, x4, x5
	add	end, x0, x2
	mov	srcin, x1
#include "copy_template.S"
	uaccess_disable_not_uao x3, x4
	mov	x0, #0
	ret
ENDPROC(__arch_copy_in_user)

	.section .fixup,"ax"
	.align	2
9997:	cmp	dst, dstin
	b.ne	9998f
	// Before being absolutely sure we couldn't copy anything, try harder
USER(9998f, ldtrb tmp1w, [srcin])
USER(9998f, sttrb tmp1w, [dst])
	add	dst, dst, #1
9998:	sub	x0, end, dst			// bytes not copied
	uaccess_disable_not_uao x3, x4
	ret
	.previous
