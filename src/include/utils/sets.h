/*-------------------------------------------------------------------------
 *
 * sets.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef SETS_H
#define SETS_H

#include "fmgr.h"


/* Temporary name of a set function, before SetDefine changes it. */
#define GENERICSETNAME "ZYX#Set#ZYX"

extern Oid	SetDefine(char *querystr, char *typename);

extern Datum seteval(PG_FUNCTION_ARGS);

#endif	 /* SETS_H */
