/* $Id$ */

#include <stdlib.h>
#include <math.h>				/* for pow() prototype */
#include <errno.h>

#include "config.h"
#include "rusagestub.h"

long
random()
{
	return lrand48();
}
