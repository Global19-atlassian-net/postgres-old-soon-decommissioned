#ifndef _ECPG_LIB_EXTERN_H
#define _ECPG_LIB_EXTERN_H

#include "postgres_fe.h"
#include "libpq-fe.h"
#include "sqlca.h"

enum COMPAT_MODE { ECPG_COMPAT_PGSQL = 0, ECPG_COMPAT_INFORMIX, ECPG_COMPAT_INFORMIX_SE};
#define INFORMIX_MODE(X) ((X) == ECPG_COMPAT_INFORMIX || (X) == ECPG_COMPAT_INFORMIX_SE)

/* Here are some methods used by the lib. */

/* Stores the backend error message for client access */
void set_backend_err(const char *err, int lineon);

/* Store and retrieve the backend error message for client access */
void set_backend_err(const char *err, int lineon);
char *ECPGerrmsg(void);

/* Returns a pointer to a string containing a simple type name. */
void		ECPGadd_mem(void *ptr, int lineno);

bool ECPGget_data(const PGresult *, int, int, int, enum ECPGttype type,
			 enum ECPGttype, char *, char *, long, long, long, bool, enum COMPAT_MODE, bool);
struct connection *ECPGget_connection(const char *);
char	   *ECPGalloc(long, int);
char	   *ECPGrealloc(void *, long, int);
void		ECPGfree(void *);
bool		ECPGinit(const struct connection *, const char *, const int);
char	   *ECPGstrdup(const char *, int);
const char *ECPGtype_name(enum ECPGttype);
unsigned int ECPGDynamicType(Oid);
void		ECPGfree_auto_mem(void);
void		ECPGclear_auto_mem(void);

/* A generic varchar type. */
struct ECPGgeneric_varchar
{
	int			len;
	char		arr[1];
};

/*
 * type information cache
 */

struct ECPGtype_information_cache
{
	struct ECPGtype_information_cache *next;
	int			oid;
	bool		isarray;
};

/* structure to store one statement */
struct statement
{
	int			lineno;
	char	   *command;
	struct connection *connection;
	enum COMPAT_MODE compat;
	bool force_indicator;
	struct variable *inlist;
	struct variable *outlist;
};

/* structure to store connections */
struct connection
{
	char	   *name;
	PGconn	   *connection;
	bool		committed;
	int			autocommit;
	struct ECPGtype_information_cache *cache_head;
	struct connection *next;
};

/* structure to store descriptors */
struct descriptor
{
	char	   *name;
	PGresult   *result;
	struct descriptor *next;
};

struct variable
{
	enum ECPGttype type;
	void	   *value;
	void	   *pointer;
	long		varcharsize;
	long		arrsize;
	long		offset;
	enum ECPGttype ind_type;
	void	   *ind_value;
	void	   *ind_pointer;
	long		ind_varcharsize;
	long		ind_arrsize;
	long		ind_offset;
	struct variable *next;
};

PGresult  **
			ECPGdescriptor_lvalue(int line, const char *descriptor);

bool ECPGstore_result(const PGresult *results, int act_field,
				 const struct statement * stmt, struct variable * var);

#endif /* _ECPG_LIB_EXTERN_H */
