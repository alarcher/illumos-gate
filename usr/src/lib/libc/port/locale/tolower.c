/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2010 Nexenta Systems, Inc.  All rights reserved.
 */
#include "lint.h"
#include <ctype.h>

#pragma weak _tolower = tolower
#pragma weak _toupper = toupper

int
tolower(int c)
{
	return (((unsigned)c > 255) ? c : __trans_lower[c]);
}

int
toupper(int c)
{
	return (((unsigned)c > 255) ? c : __trans_upper[c]);
}
