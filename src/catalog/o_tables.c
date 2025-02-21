/*-------------------------------------------------------------------------
 *
 * o_tables.c
 * 		Routines for orioledb tables system tree.
 *
 * Copyright (c) 2021-2022, Oriole DB Inc.
 *
 * IDENTIFICATION
 *	  contrib/orioledb/src/catalog/o_tables.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "orioledb.h"

#include "btree/btree.h"
#include "btree/undo.h"
#include "checkpoint/checkpoint.h"
#include "catalog/o_indices.h"
#include "catalog/o_tables.h"
#include "catalog/o_sys_cache.h"
#include "recovery/recovery.h"
#include "recovery/wal.h"
#include "transam/oxid.h"
#include "tuple/toast.h"
#include "utils/planner.h"

#include "access/heapam.h"
#include "access/transam.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/execExpr.h"
#include "executor/functions.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgrtab.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"

/*
 * Relation locks from recovery workers may conflict with PostgreSQL WAL locks
 * that leads to deadlocks. We need to have own relation locks for
 * checkpoint process to avoid this.
 */
#define CHECKPOINT_LOCK_BIT ((uint32) 1 << (32 - 1))

PG_FUNCTION_INFO_V1(orioledb_table_description);
PG_FUNCTION_INFO_V1(orioledb_table_oids);

typedef struct
{
	OTablesCallback callback;
	void	   *arg;
} OTablesForeachArg;

typedef struct
{
	OXid		oxid;
	CommitSeqNo csn;
	Oid			datoid;
} OTablesDropAllArg;

typedef struct
{
	OXid		oxid;
	CommitSeqNo csn;
	Oid			type_oid;
	Form_pg_type type_data;
} OTablesDropAllWithTypeArg;

typedef struct
{
	OIndexType	type;
	ORelOids	oids;
	OIndexNumber ixNum;
} OTableIndexOidsKey;

static void o_table_tupdesc_init_entry(TupleDesc desc, AttrNumber att_num, char *name, OTableField *field);
static void o_tables_foreach_callback(ORelOids oids, void *arg);
static void o_tables_drop_all_callback(ORelOids oids, void *arg);
static void o_table_oids_array_callback(ORelOids oids, void *arg);
static inline void o_tables_rel_fill_locktag(LOCKTAG *tag, ORelOids *oids, int lockmode, bool checkpoint);
static Pointer serialize_o_table(OTable *o_table, int *size);
static OTable *deserialize_o_table(Pointer data, Size length);

static BTreeDescr *
oTablesGetBTreeDesc(void *arg)
{
	BTreeDescr *desc = (BTreeDescr *) arg;

	return desc;
}

static uint32
oTablesGetKeySize(void *arg)
{
	return sizeof(OTableChunkKey);
}

static uint32
oTablesGetMaxChunkSize(void *key, void *arg)
{
	uint32		max_chunk_size;

	max_chunk_size = MAXALIGN_DOWN((O_BTREE_MAX_TUPLE_SIZE * 3 - MAXALIGN(sizeof(OTableChunkKey))) / 3) - offsetof(OTableChunk, data);

	return max_chunk_size;
}

static void
oTablesUpdateKey(void *key, uint32 offset, void *arg)
{
	OTableChunkKey *ckey = (OTableChunkKey *) key;

	ckey->offset = offset;
}

static void *
oTablesGetNextKey(void *key, void *arg)
{
	OTableChunkKey *ckey = (OTableChunkKey *) key;
	static OTableChunkKey nextKey;

	nextKey = *ckey;
	nextKey.oids.relnode++;
	nextKey.offset = 0;

	return (Pointer) &nextKey;
}

static OTuple
oTablesCreateTuple(void *key, Pointer data, uint32 offset,
				   int length, void *arg)
{
	OTableChunkKey *ckey = (OTableChunkKey *) key;
	OTableChunk *chunk;
	OTuple		result;

	ckey->offset = offset;

	chunk = (OTableChunk *) palloc(offsetof(OTableChunk, data) + length);
	chunk->key = *ckey;
	chunk->dataLength = length;
	memcpy(chunk->data, data + offset, length);

	result.data = (Pointer) chunk;
	result.formatFlags = 0;

	return result;
}

static OTuple
oTablesCreateKey(void *key, uint32 offset, void *arg)
{
	OTableChunkKey *ckey = (OTableChunkKey *) key;
	OTableChunkKey *ckey_copy;
	OTuple		result;

	ckey_copy = (OTableChunkKey *) palloc(sizeof(OTableChunkKey));
	*ckey_copy = *ckey;

	result.data = (Pointer) ckey_copy;
	result.formatFlags = 0;

	return result;
}

static Pointer
oTablesGetTupleData(OTuple tuple, void *arg)
{
	OTableChunk *chunk = (OTableChunk *) tuple.data;

	return chunk->data;
}

static uint32
oTablesGetTupleOffset(OTuple tuple, void *arg)
{
	OTableChunk *chunk = (OTableChunk *) tuple.data;

	return chunk->key.offset;
}

static uint32
oTablesGetTupleDataSize(OTuple tuple, void *arg)
{
	OTableChunk *chunk = (OTableChunk *) tuple.data;

	return chunk->dataLength;
}

static TupleFetchCallbackResult
oTablesVersionCallback(OTuple tuple, OXid tupOxid, CommitSeqNo csn, void *arg,
					   TupleFetchCallbackCheckType check_type)
{
	OTableChunkKey *tupleKey = (OTableChunkKey *) tuple.data;
	OTableChunkKey *boundKey = (OTableChunkKey *) arg;

	if (check_type != OTupleFetchCallbackVersionCheck)
		return OTupleFetchNext;

	if (!(COMMITSEQNO_IS_INPROGRESS(csn) && tupOxid == get_current_oxid_if_any()))
		return OTupleFetchNext;

	if (boundKey->version == O_TABLE_INVALID_VERSION)
		boundKey->version = tupleKey->version;

	if (tupleKey->version > boundKey->version)
		return OTupleFetchNext;
	else if (tupleKey->version == boundKey->version)
		return OTupleFetchMatch;
	else
		return OTupleFetchNotMatch;
}

ToastAPI	oTablesToastAPI = {
	.getBTreeDesc = oTablesGetBTreeDesc,
	.getKeySize = oTablesGetKeySize,
	.getMaxChunkSize = oTablesGetMaxChunkSize,
	.updateKey = oTablesUpdateKey,
	.getNextKey = oTablesGetNextKey,
	.createTuple = oTablesCreateTuple,
	.createKey = oTablesCreateKey,
	.getTupleData = oTablesGetTupleData,
	.getTupleOffset = oTablesGetTupleOffset,
	.getTupleDataSize = oTablesGetTupleDataSize,
	.deleteLogFullTuple = false,
	.versionCallback = oTablesVersionCallback
};

void
o_tables_foreach_oids(OTablesOidsCallback callback,
					  CommitSeqNo csn,
					  void *arg)
{
	OTableChunkKey chunk_key;
	ORelOids	oids = {0, 0, 0},
				old_oids PG_USED_FOR_ASSERTS_ONLY;
	BTreeIterator *it;
	OTuple		tuple;
	BTreeDescr *desc = get_sys_tree(SYS_TREES_O_TABLES);

	chunk_key.oids = oids;
	chunk_key.offset = 0;

	it = o_btree_iterator_create(desc, (Pointer) &chunk_key, BTreeKeyBound,
								 csn, ForwardScanDirection);

	tuple = o_btree_iterator_fetch(it, NULL, NULL,
								   BTreeKeyNone, false, NULL);
	old_oids = oids;
	while (!O_TUPLE_IS_NULL(tuple))
	{
		OTableChunk *chunk = (OTableChunk *) tuple.data;

		oids = chunk->key.oids;
		Assert(ORelOidsIsValid(oids));
		Assert(!ORelOidsIsEqual(old_oids, oids));
		old_oids = oids;

		callback(oids, arg);

		pfree(tuple.data);
		btree_iterator_free(it);

		oids.relnode += 1;		/* go to the next oid */
		chunk_key.oids = oids;
		chunk_key.offset = 0;

		it = o_btree_iterator_create(desc, (Pointer) &chunk_key, BTreeKeyBound,
									 csn, ForwardScanDirection);
		tuple = o_btree_iterator_fetch(it, NULL, NULL,
									   BTreeKeyNone, false, NULL);
	}
}

/*
 * It can be much more efficient.
 */
void
o_tables_foreach(OTablesCallback callback,
				 CommitSeqNo csn,
				 void *arg)
{
	OTablesForeachArg foreach_arg;

	foreach_arg.callback = callback;
	foreach_arg.arg = arg;

	o_tables_foreach_oids(o_tables_foreach_callback, csn, &foreach_arg);
}

static char *
o_deparse_expression(char *expr_str, Oid relid)
{
	Datum		expr;
	text	   *expr_text = cstring_to_text(expr_str);

	expr = DirectFunctionCall2(pg_get_expr, (Datum) expr_text,
							   ObjectIdGetDatum(relid));
	return TextDatumGetCString(expr);
}

void
o_table_fill_index(OTable *o_table, OIndexNumber ix_num,
				   OIndexType type, Relation index_rel)
{
	OTableIndex	   *index = &o_table->indices[ix_num];
	ListCell	   *index_expr_elem = list_head(index_rel->rd_indexprs);
	int				ix_exprfield_num;
	ListCell	   *lc;
	MemoryContext	mcxt,
					old_mcxt;
	int				keyno;
	Datum			datum;
	oidvector	   *indclass;
	oidvector	   *indcollation;
	bool			isnull;

	index->index_mctx = NULL;
	mcxt = OGetIndexContext(index);
	old_mcxt = MemoryContextSwitchTo(mcxt);
	if (index_rel->rd_indexprs != NIL)
	{
		index->nexprfields = list_length(index_rel->rd_indexprs);
		index->exprfields = palloc0(index->nexprfields * sizeof(OTableField));
	}
	index->predicate = (List *)
		expression_planner((Expr *) index_rel->rd_indpred);
	if (index->predicate)
	{
		index->predicate_str =
			o_deparse_expression(nodeToString(index->predicate),
								 o_table->oids.reloid);
	}
	o_collect_funcexpr((Node *) index->predicate);
	index->expressions = NIL;
	foreach(lc, index_rel->rd_indexprs)
	{
		Expr	   *e = (Expr *) lfirst(lc);
		Expr	   *node;

		node = expression_planner(e);
		index->expressions = lappend(index->expressions, node);
	}
	o_collect_funcexpr((Node *) index->expressions);
	MemoryContextSwitchTo(old_mcxt);

	/* Must get indclass the hard way */
	datum = SysCacheGetAttr(INDEXRELID, index_rel->rd_indextuple,
							Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	indclass = (oidvector *) DatumGetPointer(datum);

	/* Extract indcollation from the pg_index tuple */
	datum = SysCacheGetAttr(INDEXRELID, index_rel->rd_indextuple,
							Anum_pg_index_indcollation, &isnull);
	Assert(!isnull);
	indcollation = (oidvector *) DatumGetPointer(datum);

	ix_exprfield_num = 0;
	for (keyno = 0; keyno < index_rel->rd_index->indnkeyatts; keyno++)
	{
		AttrNumber			attnum = index_rel->rd_index->indkey.values[keyno];
		OTableIndexField   *ix_field;
		int16				opt = index_rel->rd_indoption[keyno];

		ix_field = &index->fields[keyno];
		if (AttributeNumberIsValid(attnum))
		{
			/* Field validation performed in o_validate_index_elements */
			ix_field->attnum = attnum - 1;
		}
		else
		{
			/* Expressional index */
			Node		   *indexkey;
			HeapTuple		tuple;
			Form_pg_type	typeTup;
			OTableField	   *exprField;
			Oid				field_typeid;

			Assert(index_rel->rd_indexprs);
			indexkey = lfirst(index_expr_elem);
			index_expr_elem = lnext(index_rel->rd_indexprs, index_expr_elem);
			exprField = &index->exprfields[ix_exprfield_num++];

			/*
			 * Lookup the expression type in pg_type for the type length etc.
			 */
			field_typeid = exprType(indexkey);
			tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(field_typeid));
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for type %u", field_typeid);
			typeTup = (Form_pg_type) GETSTRUCT(tuple);

			/*
			 * Assign some of the attributes values. Leave the rest.
			 */
			namestrcpy(&(exprField->name),
					   o_deparse_expression(nodeToString(indexkey),
											o_table->oids.reloid));
			exprField->typid = field_typeid;
			exprField->typlen = typeTup->typlen;
			exprField->byval = typeTup->typbyval;
			exprField->storage = typeTup->typstorage;
			exprField->align = typeTup->typalign;
			exprField->typmod = exprTypmod(indexkey);
			exprField->collation = exprCollation(indexkey);

			ReleaseSysCache(tuple);

			/*
			 * Make sure the expression yields a type that's safe to store in
			 * an index.  We need this defense because we have index opclasses
			 * for pseudo-types such as "record", and the actually stored type
			 * had better be safe; eg, a named composite type is okay, an
			 * anonymous record type is not.  The test is the same as for
			 * whether a table column is of a safe type (which is why we
			 * needn't check for the non-expression case).
			 */
			CheckAttributeType("EXPR_FIELD",
							   exprField->typid, exprField->collation,
							   NIL, 0);

			ix_field->attnum = EXPR_ATTNUM;
		}
		ix_field->collation = indcollation->values[keyno];
		ix_field->opclass = indclass->values[keyno];
		ix_field->ordering = SORTBY_DEFAULT;
		ix_field->nullsOrdering = SORTBY_NULLS_DEFAULT;
		if (opt & INDOPTION_DESC)
		{
			ix_field->ordering = SORTBY_DESC;
			if ((opt & INDOPTION_NULLS_FIRST) == 0)
				ix_field->nullsOrdering = SORTBY_NULLS_LAST;
		}
		else if (opt & INDOPTION_NULLS_FIRST)
		{
			ix_field->nullsOrdering = SORTBY_NULLS_FIRST;
		}
	}
}

void
o_table_resize_constr(OTable *o_table)
{
	MemoryContext	oldcxt;
	MemoryContext	tbl_cxt;

	tbl_cxt = OGetTableContext(o_table);
	oldcxt = MemoryContextSwitchTo(tbl_cxt);

	if (!o_table->missing)
		o_table->missing = palloc0(o_table->nfields * sizeof(AttrMissing));
	else
		o_table->missing = repalloc(o_table->missing,
									o_table->nfields * sizeof(AttrMissing));
	o_table->missing[o_table->nfields - 1].am_present = false;
	o_table->missing[o_table->nfields - 1].am_value = 0;

	if (!o_table->defvals)
		o_table->defvals = palloc0(o_table->nfields * sizeof(Expr *));
	else
		o_table->defvals = repalloc(o_table->defvals,
									o_table->nfields * sizeof(Expr *));
	o_table->defvals[o_table->nfields - 1] = NULL;

	MemoryContextSwitchTo(oldcxt);
}

void
o_table_fill_constr(OTable *o_table, int i, AttrMissing *attrmiss,
					Expr *defval)
{
	OTableField	   *field = &o_table->fields[i];
	MemoryContext	oldcxt;
	MemoryContext	tbl_cxt;

	tbl_cxt = OGetTableContext(o_table);
	oldcxt = MemoryContextSwitchTo(tbl_cxt);

	if (attrmiss)
	{
		o_table->missing[i].am_present = field->hasmissing &&
										 attrmiss->am_present;
		if (o_table->missing[i].am_present)
			o_table->missing[i].am_value = datumCopy(attrmiss->am_value,
													 field->byval,
													 field->typlen);
		else
			o_table->missing[i].am_value = 0;
	}

	o_table->defvals[i] = copyObject(defval);
	MemoryContextSwitchTo(oldcxt);
}

OTable *
o_table_tableam_create(ORelOids oids, TupleDesc tupdesc)
{
	OTable	   *o_table;
	int			i;

	o_table = palloc0(sizeof(OTable));
	o_table->nfields = tupdesc->natts;
	o_table->primary_init_nfields = o_table->nfields + 1;	/* + ctid field */
	o_table->fields = palloc0(o_table->nfields * sizeof(OTableField));
	o_table->oids = oids;
	o_table->tid_btree_ops_oid = GetDefaultOpClass(TIDOID, BTREE_AM_OID);

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = &tupdesc->attrs[i];
		OTableField *field = &o_table->fields[i];

		orioledb_attr_to_field(field, attr);
	}
	o_table->nindices = 0;
	o_table_resize_constr(o_table);

	return o_table;
}

static OTableField builtin_fields[] =
{
	{{{0}}, INT2OID, InvalidOid, -1, 0, true, false, true, 2, 's', 'p'},
	{{{0}}, INT4OID, InvalidOid, -1, 0, true, false, true, 4, 'i', 'p'},
	{{{0}}, OIDOID, InvalidOid, -1, 0, true, false, true, 4, 'i', 'p'},
	{{{0}}, TIDOID, InvalidOid, -1, 0, false, false, true, 6, 's', 'p'},
	{{{0}}, BYTEAOID, InvalidOid, -1, 0, false, false, true, -1, 'i', 'x'}
};

OTableField *
o_tables_get_builtin_field(Oid type)
{
	int			i;

	for (i = 0; i < sizeof(builtin_fields) / sizeof(builtin_fields[0]); i++)
	{
		if (type == builtin_fields[i].typid)
		{
			return &builtin_fields[i];
		}
	}
	Assert(false);				/* shouldn't get there */
	return NULL;
}

/*
 * We hold data of some types itself because they used inside o_tables.
 */
void
o_tables_tupdesc_init_builtin(TupleDesc desc, AttrNumber att_num, char *name, Oid type)
{
	o_table_tupdesc_init_entry(desc, att_num, name, o_tables_get_builtin_field(type));
}

/*
 * Returns tuple descriptor made from array
 */
TupleDesc
o_table_fields_make_tupdesc(OTableField *fields, int nfields)
{
	OTableField *field;
	TupleDesc	tupdesc = CreateTemplateTupleDesc(nfields);
	int			i;

	for (i = 0; i < nfields; i++)
	{
		field = &fields[i];
		o_table_tupdesc_init_entry(tupdesc, i + 1, NameStr(field->name), field);
	}
	return tupdesc;
}

void
o_tupdesc_load_constr(TupleDesc tupdesc, OTable *o_table, OIndexDescr *descr)
{
	MemoryContext	oldcxt;
	MemoryContext	idx_cxt;
	int				i;
	int				ctid_off;

	idx_cxt = OGetIndexContext(descr);
	oldcxt = MemoryContextSwitchTo(idx_cxt);
	ctid_off = o_table->has_primary ? 0 : 1;

	tupdesc->constr = (TupleConstr *) palloc0(sizeof(TupleConstr));
	tupdesc->constr->missing = (AttrMissing *)
		palloc0((o_table->nfields + ctid_off) * sizeof(AttrMissing));

	if (!o_table->has_primary)
		tupdesc->constr->missing[0].am_present = false;

	for (i = 0; i < o_table->nfields; i++)
	{
		OTableField *field = &o_table->fields[i];
		AttrMissing *tupdesc_miss = &tupdesc->constr->missing[i + ctid_off];

		tupdesc_miss->am_present = o_table->missing[i].am_present;

		if (o_table->missing[i].am_present)
		{
			tupdesc_miss->am_value =
				datumCopy(o_table->missing[i].am_value, field->byval,
							field->typlen);
		}
	}
	MemoryContextSwitchTo(oldcxt);
}

TupleDesc
o_table_tupdesc(OTable *o_table)
{
	return o_table_fields_make_tupdesc(o_table->fields, o_table->nfields);
}

static int
index_keys_cmp(const void *p1, const void *p2)
{
	const OTableIndexOidsKey *key1 = (const OTableIndexOidsKey *) p1;
	const OTableIndexOidsKey *key2 = (const OTableIndexOidsKey *) p2;

	if (key1->type < key2->type)
		return -1;
	else if (key1->type > key2->type)
		return 1;

	if (key1->oids.datoid < key2->oids.datoid)
		return -1;
	else if (key1->oids.datoid > key2->oids.datoid)
		return 1;

	if (key1->oids.reloid < key2->oids.reloid)
		return -1;
	else if (key1->oids.reloid > key2->oids.reloid)
		return 1;

	if (key1->oids.relnode < key2->oids.relnode)
		return -1;
	else if (key1->oids.relnode > key2->oids.relnode)
		return 1;

	return 0;
}

static OTableIndexOidsKey *
o_table_make_index_keys(OTable *table, int *num)
{
	OTableIndexOidsKey *keys;
	int			keys_num = 0;
	int			i;

	if (!table)
	{
		*num = 0;
		return NULL;
	}

	keys = (OTableIndexOidsKey *) palloc(sizeof(OTableIndexOidsKey) *
										 (table->nindices + 2));

	/* ctid primary index if needed */
	if (table->nindices == 0 ||
		table->indices[PrimaryIndexNumber].type != oIndexPrimary)
	{
		keys[keys_num].type = oIndexPrimary;
		keys[keys_num].ixNum = keys_num;
		keys[keys_num++].oids = table->oids;
	}

	for (i = 0; i < table->nindices; i++)
	{
		keys[keys_num].type = table->indices[i].type;
		keys[keys_num].ixNum = keys_num;
		keys[keys_num++].oids = table->indices[i].oids;
	}

	if (ORelOidsIsValid(table->toast_oids))
	{
		keys[keys_num].type = oIndexToast;
		keys[keys_num].ixNum = TOASTIndexNumber;
		keys[keys_num++].oids = table->toast_oids;
	}

	qsort(keys, keys_num, sizeof(OTableIndexOidsKey), index_keys_cmp);

	*num = keys_num;
	return keys;
}

/*
 * Returns array of ORelOids for each table index (including TOAST).
 *
 * Array is allocated in CurTransactionContext.
 */
ORelOids *
o_table_make_index_oids(OTable *table, int *num)
{
	ORelOids   *oids;
	int			oids_num;
	int			i;

	Assert(table && num);

	oids_num = table->nindices;
	oids = (ORelOids *) palloc(sizeof(ORelOids) * (oids_num + 2));
	for (i = 0; i < oids_num; i++)
		oids[i] = table->indices[i].oids;

	oids[oids_num++] = table->toast_oids;

	/* ctid primary index if needed */
	if (table->nindices == 0 ||
		table->indices[PrimaryIndexNumber].type == oIndexPrimary)
		oids[oids_num++] = table->oids;

	*num = oids_num;
	return oids;
}

/*
 * Updates SYS_TREES_O_INDICES.
 */
static void
o_tables_oids_indexes(OTable *old_table, OTable *new_table,
					  OXid oxid, CommitSeqNo csn)
{
	OTableIndexOidsKey *old_keys = NULL;
	OTableIndexOidsKey *new_keys = NULL;
	int			old_keys_num = 0,
				new_keys_num = 0,
				i = 0,
				j = 0;

	old_keys = o_table_make_index_keys(old_table, &old_keys_num);
	new_keys = o_table_make_index_keys(new_table, &new_keys_num);

	while (i < old_keys_num || j < new_keys_num)
	{
		int			cmp;

		if (i >= old_keys_num)
		{
			cmp = 1;
		}
		else if (j >= new_keys_num)
		{
			cmp = -1;
		}
		else
		{
			cmp = index_keys_cmp(&old_keys[i], &new_keys[j]);

			if (cmp == 0)
			{
				i++;
				j++;
				continue;
			}
		}

		if (cmp < 0)
		{
			bool		result;

			elog(DEBUG2, "o_indices del (%u, %u, %u, %u) - (%u, %u, %u)",
				 old_keys[i].type,
				 old_keys[i].oids.datoid,
				 old_keys[i].oids.reloid,
				 old_keys[i].oids.relnode,
				 old_table->oids.datoid,
				 old_table->oids.reloid,
				 old_table->oids.relnode);

			result = o_indices_del(old_table, old_keys[i].ixNum, oxid, csn);
			if (!result)
				elog(ERROR, "missing entries in o_indices");
			i++;
		}

		if (cmp > 0)
		{
			bool		result PG_USED_FOR_ASSERTS_ONLY;

			elog(DEBUG2, "o_indices add (%u, %u, %u, %u) - (%u, %u, %u)",
				 new_keys[j].type,
				 new_keys[j].oids.datoid,
				 new_keys[j].oids.reloid,
				 new_keys[j].oids.relnode,
				 new_table->oids.datoid,
				 new_table->oids.reloid,
				 new_table->oids.relnode);

			result = o_indices_add(new_table, new_keys[j].ixNum, oxid, csn);
			Assert(result);
			j++;
		}
	}
}

OTable *
o_tables_drop_by_oids(ORelOids oids, OXid oxid, CommitSeqNo csn)
{
	OTableChunkKey key;
	OTable	   *table;
	bool		result;

	key.oids = oids;
	key.offset = 0;

	systrees_modify_start();
	table = o_tables_get(oids);
	o_tables_oids_indexes(table, NULL, oxid, csn);
	result = generic_toast_delete(&oTablesToastAPI, (Pointer) &key, oxid,
								  csn, get_sys_tree(SYS_TREES_O_TABLES));
	systrees_modify_end();

	if (result)
	{
		return table;
	}
	else
	{
		o_table_free(table);
		return NULL;
	}
}

void
o_tables_drop_all(OXid oxid, CommitSeqNo csn, Oid database_id)
{
	OTablesDropAllArg arg;

	arg.oxid = oxid;
	arg.csn = csn;
	arg.datoid = database_id;

	o_tables_foreach_oids(o_tables_drop_all_callback,
						  COMMITSEQNO_NON_DELETED, &arg);
}

bool
o_tables_add(OTable *table, OXid oxid, CommitSeqNo csn)
{
	OTableChunkKey key;
	bool		result;
	Pointer		data;
	int			len;

	data = serialize_o_table(table, &len);

	key.oids = table->oids;
	key.offset = 0;
	key.version = 0;

	systrees_modify_start();
	o_tables_oids_indexes(NULL, table, oxid, csn);
	result = generic_toast_insert(&oTablesToastAPI, (Pointer) &key, data, len,
								  oxid, csn, get_sys_tree(SYS_TREES_O_TABLES));
	systrees_modify_end();
	pfree(data);

	return result;
}

/*
 * Same as o_tables_get, if version not NULL find o_tables with passed version
 */
OTable *
o_tables_get_by_oids_and_version(ORelOids oids, uint32 *version)
{
	OTableChunkKey key,
			   *found_key = NULL;
	Pointer		result;
	Size		dataLength;
	OTable	   *oTable;

	key.oids = oids;
	key.offset = 0;
	if (version)
		key.version = *version;
	else
		key.version = O_TABLE_INVALID_VERSION;

	found_key = &key;
	result = generic_toast_get_any_with_key(&oTablesToastAPI, (Pointer) &key,
											&dataLength,
											COMMITSEQNO_NON_DELETED,
											get_sys_tree(SYS_TREES_O_TABLES),
											(Pointer *) &found_key);

	if (result == NULL)
		return NULL;

	oTable = deserialize_o_table(result, dataLength);
	oTable->version = found_key->version;
	pfree(result);
	pfree(found_key);

	return oTable;
}

/*
 * Find OTable by its oids
 */
OTable *
o_tables_get(ORelOids oids)
{
	return o_tables_get_by_oids_and_version(oids, NULL);
}

/*
 * Find OTable by tree oids
 */
OTable *
o_tables_get_by_tree(ORelOids oids, OIndexType type)
{
	ORelOids	tableOids;
	bool		result;

	/* See if it's index oid first */
	result = o_indices_find_table_oids(oids, type, COMMITSEQNO_INPROGRESS,
									   &tableOids);
	if (!result)
		return NULL;

	return o_tables_get(tableOids);
}

void
o_table_free(OTable *table)
{
	int			i;

	for (i = 0; i < table->nfields; i++)
	{
		if (table->missing[i].am_present && !table->fields[i].byval)
			pfree(DatumGetPointer(table->missing[i].am_value));
	}
	pfree(table->missing);
	for (i = 0; i < table->nindices; i++)
	{
		if (table->indices[i].index_mctx)
			MemoryContextDelete(table->indices[i].index_mctx);
	}

	if (table->indices)
		pfree(table->indices);
	pfree(table->fields);
	if (table->tbl_mctx)
		MemoryContextDelete(table->tbl_mctx);
	pfree(table);
}

bool
o_tables_update(OTable *table, OXid oxid, CommitSeqNo csn)
{
	OTableChunkKey key;
	OTable	   *old_table;
	bool		result;
	Pointer		data;
	int			len;

	data = serialize_o_table(table, &len);

	key.oids = table->oids;
	key.offset = 0;
	key.version = table->version + 1;

	systrees_modify_start();
	old_table = o_tables_get(table->oids);
	o_tables_oids_indexes(old_table, table, oxid, csn);
	result = generic_toast_update(&oTablesToastAPI, (Pointer) &key, data, len,
								  oxid, csn, get_sys_tree(SYS_TREES_O_TABLES));
	systrees_modify_end();

	pfree(data);
	o_table_free(old_table);

	add_invalidate_wal_record(table->oids, table->oids.relnode);
	return result;
}

void
o_tables_after_update(OTable *o_table, OXid oxid, CommitSeqNo csn)
{
	o_opclass_cache_add_table(o_table);
	o_indices_update(o_table, PrimaryIndexNumber, oxid, csn);
	if (o_table->has_primary)
		o_invalidate_oids(o_table->indices[PrimaryIndexNumber].oids);
	o_invalidate_oids(o_table->oids);
}

bool
o_tables_rel_try_lock_extended(ORelOids *oids, int lockmode,
							   bool *nested, bool checkpoint)
{
	LOCKTAG		locktag;
	LockAcquireResult result;

	o_tables_rel_fill_locktag(&locktag, oids, lockmode, checkpoint);

	if (nested != NULL)
		*nested = DoLocalLockExist(&locktag);

	if (lockmode == AccessExclusiveLock &&
		locktag.locktag_lockmethodid == DEFAULT_LOCKMETHOD)
		locktag.locktag_lockmethodid = NO_LOG_LOCKMETHOD;
	result = LockAcquire(&locktag, lockmode, false, true);

	if (result != LOCKACQUIRE_NOT_AVAIL)
	{
		AcceptInvalidationMessages();
		return true;
	}
	return false;
}

void
o_tables_rel_lock_extended(ORelOids *oids, int lockmode, bool checkpoint)
{
	LOCKTAG		locktag;

	o_tables_rel_fill_locktag(&locktag, oids, lockmode, checkpoint);

	if (lockmode == AccessExclusiveLock && checkpoint)
		locktag.locktag_lockmethodid = NO_LOG_LOCKMETHOD;

	LockAcquire(&locktag, lockmode, false, false);
	AcceptInvalidationMessages();
}

void
o_tables_rel_unlock_extended(ORelOids *oids, int lockmode, bool checkpoint)
{
	LOCKTAG		locktag;

	o_tables_rel_fill_locktag(&locktag, oids, lockmode, checkpoint);

	if (!LockRelease(&locktag, lockmode, false))
	{
		elog(ERROR, "Can not release %s table lock on datoid = %d, "

			 "relnode = %d",
			 lockmode == AccessShareLock ? "share" : "exclusive",
			 oids->datoid, oids->relnode);
	}
}

char *
o_get_type_name(Oid typid, int32 typmod)
{
	return format_type_extended(typid,
								typmod,
								FORMAT_TYPE_TYPEMOD_GIVEN |
								FORMAT_TYPE_ALLOW_INVALID);
}

char *
o_get_collation_name(Oid colid)
{
	if (colid < FirstGenbkiObjectId)
		return get_collation_name(colid);
	else
		return psprintf("%u", colid);
}

static text *
describe_table(ORelOids oids)
{
	OTable	   *table;
	StringInfoData buf,
				format,
				title;
	char	   *column_str = "Column",
			   *type_str = "Type",
			   *collation_str = "Collation";
	int			i,
				max_column_str,
				max_type_str,
				max_collation_str;

	table = o_tables_get(oids);
	if (table == NULL)
		elog(ERROR, "unable to find orioledb table description.");

	max_column_str = strlen(column_str);
	max_type_str = strlen(type_str);
	max_collation_str = strlen(collation_str);
	for (i = 0; i < table->nfields; i++)
	{
		OTableField *field = &table->fields[i];
		char	   *typename = o_get_type_name(field->typid, field->typmod);
		char	   *colname = o_get_collation_name(field->collation);

		if (max_column_str < strlen(NameStr(field->name)))
			max_column_str = strlen(NameStr(field->name));
		if (max_type_str < strlen(typename))
			max_type_str = strlen(typename);
		if (colname != NULL)
		{
			if (max_collation_str < strlen(colname))
				max_collation_str = strlen(colname);
		}
	}

	initStringInfo(&title);
	appendStringInfo(&title, "Compress = %d, Primary compress = %d, TOAST compress = %d\n",
					 table->default_compress,
					 table->primary_compress,
					 table->toast_compress);
	appendStringInfo(&title, " %%%ds | %%%ds | %%%ds | Nullable | Droped \n",
					 max_column_str,
					 max_type_str,
					 max_collation_str);
	initStringInfo(&format);
	appendStringInfo(&format, " %%%ds | %%%ds | %%%ds | %%8s | %%6s \n",
					 max_column_str,
					 max_type_str,
					 max_collation_str);
	initStringInfo(&buf);
	appendStringInfo(&buf, title.data, column_str, type_str, collation_str);

	for (i = 0; i < table->nfields; i++)
	{
		OTableField *field = &table->fields[i];
		char	   *typename = o_get_type_name(field->typid, field->typmod);
		char	   *colname = o_get_collation_name(field->collation);

		appendStringInfo(&buf, format.data,
						 NameStr(field->name),
						 typename,
						 colname ? colname : "(null)",
						 field->notnull ? "false" : "true",
						 field->droped ? "true" : "false");
	}

	return cstring_to_text(buf.data);
}

Datum
orioledb_table_description(PG_FUNCTION_ARGS)
{
	ORelOids	oids;
	Relation	rel;

	if (PG_NARGS() == 1)
	{
		Oid			relid = PG_GETARG_OID(0);

		rel = relation_open(relid, AccessShareLock);
		oids.datoid = MyDatabaseId;
		oids.reloid = relid;
		oids.relnode = rel->rd_node.relNode;
		relation_close(rel, AccessShareLock);
	}
	else if (PG_NARGS() == 3)
	{
		oids.datoid = PG_GETARG_OID(0);
		oids.reloid = PG_GETARG_OID(1);
		oids.relnode = PG_GETARG_OID(2);
	}
	else
	{
		PG_RETURN_NULL();
	}

	PG_RETURN_POINTER(describe_table(oids));
}

Datum
orioledb_table_oids(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	o_tables_foreach_oids(o_table_oids_array_callback,
						  COMMITSEQNO_NON_DELETED, rsinfo);

	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

static void
o_tables_foreach_callback(ORelOids oids, void *arg)
{
	OTablesForeachArg *foreach_arg = (OTablesForeachArg *) arg;
	OTable	   *table;

	Assert(ORelOidsIsValid(oids));

	table = o_tables_get(oids);
	if (table != NULL)
	{
		foreach_arg->callback(table, foreach_arg->arg);
		o_table_free(table);
	}
}

static void
o_tables_drop_all_callback(ORelOids oids, void *arg)
{
	OTablesDropAllArg *drop_arg = (OTablesDropAllArg *) arg;

	if (drop_arg->datoid == oids.datoid)
	{
		OTable	   *table;

		table = o_tables_drop_by_oids(oids, drop_arg->oxid, drop_arg->csn);

		if (table)
		{
			ORelOids   *treeOids;
			int			numTreeOids;

			treeOids = o_table_make_index_oids(table, &numTreeOids);
			add_undo_drop_relnode(oids, treeOids, numTreeOids);
			pfree(treeOids);
			o_table_free(table);
		}
	}
}

static void
o_table_oids_array_callback(ORelOids oids, void *arg)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) arg;
	Datum		values[3];
	bool		nulls[3] = {false};

	values[0] = oids.datoid;
	values[1] = oids.reloid;
	values[2] = oids.relnode;
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

OTableField *
o_table_field_by_name(OTable *table, const char *name)
{
	int			i;

	if (name == NULL)
		return NULL;

	i = o_table_fieldnum(table, name);

	if (i < table->nfields)
		return &table->fields[i];
	else
		return NULL;
}

/*
 * Copy of TupleDescInitEntry() without SysCache usage.
 */
static void
o_table_tupdesc_init_entry(TupleDesc desc, AttrNumber att_num, char *name,
						   OTableField *field)
{
	Form_pg_attribute att;

	/*
	 * sanity checks
	 */
	AssertArg(PointerIsValid(desc));
	AssertArg(att_num >= 1);
	AssertArg(att_num <= desc->natts);
	AssertArg(field != NULL);

	/*
	 * initialize the attribute fields
	 */
	att = TupleDescAttr(desc, att_num - 1);

	att->attrelid = 0;			/* dummy value */

	/*
	 * Note: name can be NULL, because the planner doesn't always fill in
	 * valid resname values in targetlists, particularly for resjunk
	 * attributes. Also, do nothing if caller wants to re-use the old attname.
	 */
	if (name == NULL)
		MemSet(NameStr(att->attname), 0, NAMEDATALEN);
	else if (name != NameStr(att->attname))
		namestrcpy(&(att->attname), name);

	att->attstattarget = -1;
	att->attcacheoff = -1;
	att->atttypmod = field->typmod;

	att->attnum = att_num;
	att->attndims = field->ndims;

	att->attnotnull = field->notnull;
	att->atthasdef = field->hasdef;
	att->atthasmissing = field->hasmissing;
	att->attidentity = '\0';
	att->attisdropped = field->droped;
	att->attislocal = true;
	att->attinhcount = 0;

	/* attacl, attoptions and attfdwoptions are not present in tupledescs */
	att->atttypid = field->typid;
	att->attlen = field->typlen;
	att->attbyval = field->byval;
	att->attalign = field->align;
	att->attstorage = field->storage;
#if PG_VERSION_NUM >= 140000
	att->attcompression = field->compression;
#endif
	att->attcollation = field->collation;
}

static inline void
o_tables_rel_fill_locktag(LOCKTAG *tag, ORelOids *oids, int lockmode, bool checkpoint)
{
	Oid			datoid = checkpoint ? (oids->datoid | CHECKPOINT_LOCK_BIT) : oids->datoid;

	Assert(lockmode == AccessShareLock || lockmode == AccessExclusiveLock);
	Assert(ORelOidsIsValid(*oids) && !IS_SYS_TREE_OIDS(*oids));
	memset(tag, 0, sizeof(LOCKTAG));
	SET_LOCKTAG_RELATION(*tag, datoid, oids->reloid);
	if (checkpoint)
		tag->locktag_type = LOCKTAG_USERLOCK;
}

static void
serialize_o_table_index(OTableIndex *o_table_index, StringInfo str)
{
	appendBinaryStringInfo(str, (Pointer) o_table_index,
						   offsetof(OTableIndex, exprfields));
	appendBinaryStringInfo(str, (Pointer) o_table_index->exprfields,
						   o_table_index->nexprfields * sizeof(OTableField));
	o_serialize_node((Node *) o_table_index->predicate, str);
	if (o_table_index->predicate)
		o_serialize_string(o_table_index->predicate_str, str);
	o_serialize_node((Node *) o_table_index->expressions, str);
}

static Pointer
serialize_o_table(OTable *o_table, int *size)
{
	StringInfoData str;
	int			i;

	initStringInfo(&str);
	appendBinaryStringInfo(&str, (Pointer) o_table,
						   offsetof(OTable, indices));
	for (i = 0; i < o_table->nindices; i++)
	{
		serialize_o_table_index(&o_table->indices[i], &str);
	}
	appendBinaryStringInfo(&str, (Pointer) o_table->fields,
						   o_table->nfields * sizeof(OTableField));

	for (i = 0; i < o_table->nfields; i++)
	{
		Size		field_size;
		Pointer		buf,
					buf_start;

		field_size = datumEstimateSpace(o_table->missing[i].am_value,
										!o_table->missing[i].am_present,
										o_table->fields[i].byval,
										o_table->fields[i].typlen);
		appendBinaryStringInfo(&str, (Pointer) &o_table->missing[i].am_present,
							   sizeof(bool));
		buf = palloc(field_size);
		buf_start = buf;	/* copied because datumSerialize moves buf ptr */
		datumSerialize(o_table->missing[i].am_value,
					   !o_table->missing[i].am_present,
					   o_table->fields[i].byval,
					   o_table->fields[i].typlen,
					   &buf);
		appendBinaryStringInfo(&str, buf_start, field_size);
	}

	for (i = 0; i < o_table->nfields; i++)
		o_serialize_string(nodeToString(o_table->defvals[i]), &str);

	*size = str.len;
	return str.data;
}

static void
deserialize_o_table_index(OTableIndex *o_table_index, Pointer *ptr)
{
	int			len;
	MemoryContext mcxt,
				old_mcxt;

	len = offsetof(OTableIndex, exprfields);
	memcpy(o_table_index, *ptr, len);
	*ptr += len;

	o_table_index->index_mctx = NULL;
	mcxt = OGetIndexContext(o_table_index);
	old_mcxt = MemoryContextSwitchTo(mcxt);
	len = o_table_index->nexprfields * sizeof(OTableField);
	o_table_index->exprfields = (OTableField *) palloc0(len);
	memcpy(o_table_index->exprfields, *ptr, len);
	*ptr += len;

	o_table_index->predicate = (List *) o_deserialize_node(ptr);
	if (o_table_index->predicate)
		o_table_index->predicate_str = o_deserialize_string(ptr);
	o_table_index->expressions = (List *) o_deserialize_node(ptr);
	MemoryContextSwitchTo(old_mcxt);
}

OTable *
deserialize_o_table(Pointer data, Size length)
{
	Pointer		ptr = data;
	OTable	   *o_table;
	int			len;
	int			i;

	o_table = (OTable *) palloc0(sizeof(OTable));
	len = offsetof(OTable, indices);
	Assert((ptr - data) + len <= length);
	memcpy(o_table, ptr, len);
	ptr += len;

	len = o_table->nindices * sizeof(OTableIndex);
	o_table->indices = (OTableIndex *) palloc0(len);
	for (i = 0; i < o_table->nindices; i++)
	{
		deserialize_o_table_index(&o_table->indices[i], &ptr);
	}
	Assert((ptr - data) <= length);

	len = o_table->nfields * sizeof(OTableField);
	o_table->fields = (OTableField *) palloc(len);
	Assert((ptr - data) + len <= length);
	memcpy(o_table->fields, ptr, len);
	ptr += len;
	Assert(ptr - data <= length);

	o_table->missing = (AttrMissing *)
		palloc(o_table->nfields * sizeof(AttrMissing));

	for (i = 0; i < o_table->nfields; i++)
	{
		AttrMissing *miss = &o_table->missing[i];
		bool		isnull;

		memcpy(&miss->am_present, ptr, sizeof(bool));
		ptr += sizeof(bool);
		miss->am_value = datumRestore(&ptr, &isnull);
	}
	Assert(ptr - data <= length);

	o_table->defvals = palloc0(o_table->nfields * sizeof(Expr *));
	for (i = 0; i < o_table->nfields; i++)
	{
		char	   *defval_str = o_deserialize_string(&ptr);

		o_table->defvals[i] = stringToNode(defval_str);
		pfree(defval_str);
	}

	Assert(ptr - data == length);
	return o_table;
}

static void
o_tables_drop_columns_with_type_callback(OTable *o_table, void *arg)
{
	int			i;
	bool		updated = false;
	OTablesDropAllWithTypeArg *drop_arg = (OTablesDropAllWithTypeArg *) arg;

	/* Ignore search for rows of own class in table and base types */
	if (drop_arg->type_data->typtype == TYPTYPE_BASE ||
		(drop_arg->type_data->typtype == TYPTYPE_COMPOSITE &&
		 drop_arg->type_data->typrelid == o_table->oids.reloid))
		return;

	/* Drop columns containing type */
	for (i = 0; i < o_table->nfields; i++)
	{
		OTableField *o_field = &o_table->fields[i];

		if (drop_arg->type_oid == o_field->typid && !o_field->droped)
		{
			o_field->droped = true;
			updated = true;
		}
	}

	if (updated)
		o_tables_update(o_table, drop_arg->oxid, drop_arg->csn);
}


/*
 * Drops all columns of a specific type
 */
void
o_tables_drop_columns_by_type(OXid oxid, CommitSeqNo csn, Oid type_oid)
{
	OTablesDropAllWithTypeArg arg;
	HeapTuple	tuple;

	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	Assert(HeapTupleIsValid(tuple));
	ReleaseSysCache(tuple);

	arg.oxid = oxid;
	arg.csn = csn;
	arg.type_oid = type_oid;
	arg.type_data = (Form_pg_type) GETSTRUCT(tuple);

	o_tables_foreach(o_tables_drop_columns_with_type_callback,
					 COMMITSEQNO_INPROGRESS, &arg);
}

void
o_table_fill_oids(OTable *oTable, Relation rel, const RelFileNode *newrnode)
{
	Relation	toastRel,
				indexRel;
	int			i;

	oTable->oids.datoid = MyDatabaseId;
	oTable->oids.reloid = rel->rd_id;
	oTable->oids.relnode = newrnode->relNode;

	toastRel = table_open(rel->rd_rel->reltoastrelid, AccessShareLock);
	oTable->toast_oids.datoid = MyDatabaseId;
	oTable->toast_oids.reloid = toastRel->rd_id;
	oTable->toast_oids.relnode = toastRel->rd_node.relNode;
	table_close(toastRel, AccessShareLock);

	for (i = 0; i < oTable->nindices; i++)
	{
		indexRel = relation_open(oTable->indices[i].oids.reloid, AccessShareLock);
		oTable->indices[i].oids.datoid = MyDatabaseId;
		oTable->indices[i].oids.reloid = indexRel->rd_id;
		oTable->indices[i].oids.relnode = indexRel->rd_node.relNode;
		relation_close(indexRel, AccessShareLock);
	}
}
