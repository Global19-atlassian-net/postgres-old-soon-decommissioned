/*-------------------------------------------------------------------------
 *
 * rewriteDefine.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEDEFINE_H
#define REWRITEDEFINE_H

#include "nodes/parsenodes.h"

extern void DefineQueryRewrite(RuleStmt *args);

extern void RenameRewriteRule(Oid owningRel, const char *oldName,
							  const char *newName);

#endif   /* REWRITEDEFINE_H */
