/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  prototypes for functions in lib/catalog/catalog.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_H
#define CATALOG_H

#include <access/tupdesc.h>

extern char *relpath(char *relname);
extern bool IsSystemRelationName(char *relname);
extern bool IsSharedSystemRelationName(char *relname);
extern Oid	newoid(void);
extern void fillatt(TupleDesc att);

#endif	 /* CATALOG_H */
