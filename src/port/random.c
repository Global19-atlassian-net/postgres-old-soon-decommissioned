/*-------------------------------------------------------------------------
 *
 * random.c
 *	  random() wrapper
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <stdlib.h>
#include <math.h>
#include <errno.h>

long
random()
{
	return lrand48();
}
