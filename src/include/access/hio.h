/*-------------------------------------------------------------------------
 *
 * hio.h
 *	  POSTGRES heap access method input/output definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef HIO_H
#define HIO_H

#include <access/htup.h>
#include <utils/rel.h>


extern void RelationPutHeapTuple(Relation relation, Buffer buffer, 
									HeapTuple tuple);
extern void RelationPutHeapTupleAtEnd(Relation relation, HeapTuple tuple);

#endif	 /* HIO_H */
