/* C library headers must use the implementation-reserved namespace for
 * guards, type plumbing, and implementation extensions so they cannot
 * collide with users.
 */
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

/* SPDX-FileCopyrightText: (C) 2026 Gavin John
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _MEMORY_TOKENS_H
#define _MEMORY_TOKENS_H

#include <ownership.h>

/* Byte-oriented storage refinements.  The final token parameter is the byte
 * count, so zero_vacuous makes every zero-length operation valid without a
 * storage proof.  disjoint_span(other, length) is carried by the first range
 * and relates it to the exact pointer value of the second range. */
tokdef readable_span
	l_unlimited
	implicit_drop
	extent_at_least
	zero_vacuous;

tokdef writable_span
	l_unlimited
	implicit_drop
	extent_at_least
	zero_vacuous;

/* Element-counted variants derive their byte extent from the carrier's
 * pointed-to type.  They keep standard interfaces such as wmemcpy expressed
 * in their native element units without baking ABI byte widths into headers. */
tokdef readable_elements
	l_unlimited
	implicit_drop
	element_extent
	zero_vacuous;

tokdef writable_elements
	l_unlimited
	implicit_drop
	element_extent
	zero_vacuous;

tokdef disjoint_span
	l_unlimited
	implicit_drop
	disjoint_extent
	zero_vacuous;

#endif

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
