/*-------------------------------------------------------------------------
 *
 * pg_sasl_prepare.c
 *		Set of functions for a minimal extension template
 *
 * Copyright (c) 1996-2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  pg_sasl_prepare/pg_sasl_prepare.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

/* local includes */
#include "utf8_table.h"

PG_MODULE_MAGIC;

/* Utilities for array manipulation */
#define ARRPTR(x)  ((int32 *) ARR_DATA_PTR(x))
#define ARRNELEMS(x)  ArrayGetNItems(ARR_NDIM(x), ARR_DIMS(x))

/* Constants for calculations wih Hangul characters */
#define SBASE		0xEAB080	/* U+AC00 */
#define LBASE		0xE18480	/* U+1100 */
#define VBASE		0xE185A1	/* U+1161 */
#define TBASE		0xE186A7	/* U+11A7 */
#define LCOUNT		19
#define VCOUNT		21
#define TCOUNT		28
#define NCOUNT		VCOUNT * TCOUNT
#define SCOUNT		LCOUNT * NCOUNT

/*
 * Create a new int array with room for "num" elements.
 * Taken from contrib/intarray/.
 */
static ArrayType *
new_intArrayType(int num)
{
	ArrayType  *r;
	int		 nbytes = ARR_OVERHEAD_NONULLS(1) + sizeof(int) * num;

	r = (ArrayType *) palloc0(nbytes);

	SET_VARSIZE(r, nbytes);
	ARR_NDIM(r) = 1;
	r->dataoffset = 0;		/* marker for no null bitmap */
	ARR_ELEMTYPE(r) = INT4OID;
	ARR_DIMS(r)[0] = num;
	ARR_LBOUND(r)[0] = 1;

	return r;
}

/*
 * comparison routine for bsearch() of main conversion table.
 * this routine is intended for UTF8 code -> conversion entry
 */
static int
conv_compare(const void *p1, const void *p2)
{
	uint32		v1, v2;

	v1 = *(const uint32 *) p1;
	v2 = ((const pg_utf_decomposition *) p2)->utf;
	return (v1 > v2) ? 1 : ((v1 == v2) ? 0 : -1);
}

/*
 * Set of comparison functions for sub-tables.
 */
#define CONV_COMPARE_SIZE(type)									\
static int														\
conv_compare_size_##type(const void *p1, const void *p2)		\
{																\
	uint32		v1, v2;											\
	v1 = *(const uint32 *) p1;									\
	v2 = ((const pg_utf_decomposition_size_##type *) p2)->utf;	\
	return (v1 > v2) ? 1 : ((v1 == v2) ? 0 : -1);				\
}
/* Update this list of new sub-tables are present in utf8_table.h */
CONV_COMPARE_SIZE(1);
CONV_COMPARE_SIZE(2);
CONV_COMPARE_SIZE(3);
CONV_COMPARE_SIZE(4);
CONV_COMPARE_SIZE(5);
CONV_COMPARE_SIZE(6);
CONV_COMPARE_SIZE(7);
CONV_COMPARE_SIZE(8);
CONV_COMPARE_SIZE(18);

/*
 * Get the entry corresponding to code in the main conversion table.
 * This is useful to avoid repeating the calls to bsearch everywhere.
 */
static pg_utf_decomposition *
get_code_entry(uint32 code)
{
	pg_utf_decomposition *entry;

	/*
	 * bsearch() works as follows:
	 * - a key to check for matches.
	 * - a pointer pointing to the base of the conversion table.
	 * - number of elements in the array to look for,
	 * - size of an array element.
	 * - comparison function.
	 * If a match cannot be found, NULL is returned.
	 */
	entry = bsearch(&code,
					(void *) SASLPrepConv,
					lengthof(SASLPrepConv),
					sizeof(pg_utf_decomposition),
					conv_compare);

	return entry;
}

/*
 * Using an entry from the main decomposition table, return an
 * array which is a pointer to the decomposition.
 */
#define CONV_SEARCH_SIZE(type)					\
{												\
	pg_utf_decomposition_size_##type *item;		\
	uint32	*result;							\
	item = bsearch(&code,						\
		(void *) UtfDecomp_##type,				\
		lengthof(UtfDecomp_##type),				\
		sizeof(pg_utf_decomposition_size_##type), \
		conv_compare_size_##type);				\
	result = item->decomp;						\
	return result;								\
} while(0);
static uint32 *
get_code_decomposition(pg_utf_decomposition *entry)
{
	uint32	code = entry->utf;

	switch (entry->dec_size)
	{
		case 1:
			CONV_SEARCH_SIZE(1);
		case 2:
			CONV_SEARCH_SIZE(2);
		case 3:
			CONV_SEARCH_SIZE(3);
		case 4:
			CONV_SEARCH_SIZE(4);
		case 5:
			CONV_SEARCH_SIZE(5);
		case 6:
			CONV_SEARCH_SIZE(6);
		case 7:
			CONV_SEARCH_SIZE(7);
		case 8:
			CONV_SEARCH_SIZE(8);
		case 18:
			CONV_SEARCH_SIZE(18);
		default:
			Assert(false);
	}

	/* should not come here */
	return NULL;
}

/*
 * Recursively look at the number of elements in the conversion table
 * to calculate how many characters are used for the given code.
 */
static int
get_decomposed_size(uint32 code)
{
	pg_utf_decomposition *entry;
	int		size = 0;
	int		i;
	uint32 *decomp;

	/*
	 * Fast path for Hangul characters not stored in tables to save memory
	 * as decomposition is algorithmic.
	 * See http://unicode.org/reports/tr15/tr15-18.html, annex 10 for details
	 * on the matter.
	 */
	if (code >= SBASE && code < SBASE + SCOUNT)
	{
		uint32	tindex, sindex;

		sindex = code - SBASE;
		tindex = sindex % TCOUNT;

		if (tindex != 0)
			return 3;
		return 2;
	}

	entry = get_code_entry(code);

	/*
	 * Just count current code if no other decompositions.  A NULL entry
	 * is equivalent to a character with class 0 and no decompositions.
	 */
	if (entry == NULL || entry->dec_size == 0)
		return 1;

	/*
	 * If this entry has other decomposition codes look at them as well.
	 * First get its decomposition in the list of tables available.
	 */
	decomp = get_code_decomposition(entry);
	for (i = 0; i < entry->dec_size; i++)
	{
		uint32 lcode = decomp[i];

		size += get_decomposed_size(lcode);
	}

	return size;
}

/*
 * Recompose a set of characters. For hangul characters, the calculation
 * is algorithmic. For others, an inverse lookup at the decomposition
 * table is necessary. Returns true if a recomposition can be done, and
 * false otherwise.
 */
static bool
recompose_code(uint32 start, uint32 code, uint32 *result)
{
	/* No need to care about ascii characters */
	if (start <= 0x7f || code <= 0x7f)
		return false;

	/* Hangul characters go here */
	if (start >= LBASE && start < LBASE + LCOUNT &&
		code >= VBASE && code < VBASE + VCOUNT)
	{
		*result = ((start - LBASE) * VCOUNT + code - VBASE) * TCOUNT + SBASE;
		return true;
	}
	else if (start >= SBASE && start < (SBASE + SCOUNT) &&
			 ((start - SBASE) % TCOUNT) == 0 &&
			 code >= TBASE && code < (TBASE + TCOUNT))
	{
		*result = start + code - TBASE;
		return true;
	}
	else
	{
		int i;

		/*
		 * Do an inverse lookup of the decomposition tables to see if
		 * anything matches. The comparison just needs to be a perfect
		 * match on the sub-table of size two, because the start character
		 * has already been recomposed partially.
		 */
		for (i = 0; i < lengthof(UtfDecomp_2); i++)
		{
			pg_utf_decomposition_size_2 entry = UtfDecomp_2[i];
			if (start == entry.decomp[0] &&
				code == entry.decomp[1])
			{
				*result = entry.utf;
				return true;
			}
		}
    }

	return false;
}

/*
 * Decompose the given code into the array given by caller. The
 * decomposition begins at the position given by caller, saving one
 * lookup at the conversion table. The current position needs to be
 * updated here to let the caller know from where to continue filling
 * in the array result.
 */
static void
decompose_code(uint32 code, int **result, int *current)
{
	pg_utf_decomposition *entry;
	int		i;
	uint32 *decomp;

	/*
	 * Fast path for Hangul characters not stored in tables to save memory
	 * as decomposition is algorithmic.
	 * See http://unicode.org/reports/tr15/tr15-18.html, annex 10 for details
	 * on the matter.
	 */
	if (code >= SBASE && code < SBASE + SCOUNT)
	{
		uint32	l, v, tindex, sindex;
		int   *res = *result;

		sindex = code - SBASE;
		l = LBASE + sindex / (VCOUNT * TCOUNT);
		v = VBASE + (sindex % (VCOUNT * TCOUNT)) / TCOUNT;
		tindex = sindex % TCOUNT;

		res[*current] = l;
		(*current)++;
		res[*current] = v;
		(*current)++;

		if (tindex != 0)
		{
			res[*current] = TBASE + tindex;
			(*current)++;
		}

		return;
	}

	entry = get_code_entry(code);

	/*
	 * Just fill in with the current decomposition if there are no
	 * decomposition codes to recurse to.  A NULL entry is equivalent
	 * to a character with class 0 and no decompositions, so just leave
	 * also in this case.
	 */
	if (entry == NULL || entry->dec_size == 0)
	{
		int *res = *result;

		res[*current] = (int) code;
		(*current)++;
		return;
	}

	/*
	 * If this entry has other decomposition codes look at them as well.
	 */
	decomp = get_code_decomposition(entry);
	for (i = 0; i < entry->dec_size; i++)
	{
		uint32 lcode = decomp[i];

		/* Leave if no more decompositions */
		decompose_code(lcode, result, current);
	}
}


/*
 * pg_sasl_prepare
 *
 * Perform SASLprepare (NKFC) on a integer array identifying individual
 * multibyte UTF-8 characters.
 */
PG_FUNCTION_INFO_V1(pg_sasl_prepare);
Datum
pg_sasl_prepare(PG_FUNCTION_ARGS)
{
	ArrayType  *input = PG_GETARG_ARRAYTYPE_P(0);
	int		   *input_ptr = ARRPTR(input);
	ArrayType  *result;
	int		   *result_ptr;
	int		   *decomp_ptr;
	int		   *recomp_ptr;
	int			count;
	int			size = 0;
	int			decomp_size = 0;
	int			recomp_size = 0;
	/* variables for recomposition */
	int			last_class;
	int			starter_pos;
	int			target_pos;
	uint32		starter_ch;

	/* First do the compatibility decomposition */

	/*
	 * Look recursively at the convertion table to understand the number
	 * of elements that need to be created.
	 */
	for (count = 0; count < ARRNELEMS(input); count++)
	{
		uint32 code = input_ptr[count];

		/*
		 * Recursively look at the conversion table to determine into
		 * how many characters the given code need to be decomposed.
		 */
		decomp_size += get_decomposed_size(code);
	}

	/*
	 * Now fill in each entry recursively. This needs a second pass on
	 * the conversion table.
	 */
	decomp_ptr = (int *) palloc(decomp_size * sizeof(int));
	size = 0;
	for (count = 0; count < ARRNELEMS(input); count++)
	{
		uint32 code = input_ptr[count];

		decompose_code(code, &decomp_ptr, &size);

		/*
		 * XXX: Is it necessary to reorder the combining marks here?
		 */
	}

	/*
	 * Now that the decomposition is done, apply the combining class
	 * for each multibyte character.
	 */
	for (count = 1; count < decomp_size; count++)
	{
		uint32	prev = decomp_ptr[count - 1];
		uint32	next = decomp_ptr[count];
		uint32	tmp;
		pg_utf_decomposition *prevEntry = get_code_entry(prev);
		pg_utf_decomposition *nextEntry = get_code_entry(next);

		/*
		 * If no entries are found, the character used is either an Hangul
		 * character or a character with a class of 0 and no decompositions,
		 * so move to next result.
		 */
		if (prevEntry == NULL || nextEntry == NULL)
			continue;

		/*
		 * Per Unicode (http://unicode.org/reports/tr15/tr15-18.html) annex 4,
		 * a sequence of two adjacent characters in a string is an exchangeable
		 * pair if the combining class (from the Unicode Character Database)
		 * for the first character is greater than the combining class for the
		 * second, and the second is not a starter.  A character is a starter
		 * if its combining class is 0.
		 */
		if (nextEntry->class == 0x0 || prevEntry->class == 0x0)
			continue;

		if (prevEntry->class <= nextEntry->class)
			continue;

		/* exchange can happen */
		tmp = decomp_ptr[count - 1];
		decomp_ptr[count - 1] = decomp_ptr[count];
		decomp_ptr[count] = tmp;

		/* backtrack to check again */
		if (count > 1)
			count -= 2;
	}

	/*
	 * The last phase of NFKC is the recomposition of the multibyte string
	 * that has been reordered previously using combining classes. The
	 * recomposed string cannot be longer than the decomposed one, so
	 * make the allocation of the recomposed string based on that assumption.
	 */
	recomp_ptr = (int *) palloc(decomp_size * sizeof(int));
	last_class = -1;	 /* this eliminates a special check */
	starter_pos = 0;
	target_pos = 1;
	starter_ch = recomp_ptr[0] = decomp_ptr[0];

	for (count = 1; count < decomp_size; count++)
	{
		uint32 ch = (uint32) decomp_ptr[count];
		pg_utf_decomposition *ch_entry = get_code_entry(ch);
		int			ch_class = ch_entry == NULL ? 0 : ch_entry->class;
		pg_wchar	composite;

		if (last_class < ch_class &&
			recompose_code(starter_ch, ch, &composite))
		{
			recomp_ptr[starter_pos] = composite;
			starter_ch = composite;
		}
		else if (ch_class == 0)
		{
			starter_pos = target_pos;
			starter_ch  = ch;
			last_class  = -1;
			recomp_ptr[target_pos++] = ch;
		}
		else
		{
			last_class = ch_class;
			recomp_ptr[target_pos++] = ch;
		}
	}

	recomp_size = target_pos;

	/* And finally fill-in the result */
	result = new_intArrayType(recomp_size);
	result_ptr = ARRPTR(result);
	memcpy(result_ptr, recomp_ptr, recomp_size * sizeof(uint32));
	PG_RETURN_POINTER(result);
}

/*
 * utf8_to_array
 * Convert a UTF-8 string into an integer array.
 */
PG_FUNCTION_INFO_V1(utf8_to_array);
Datum
utf8_to_array(PG_FUNCTION_ARGS)
{
	char	   *input = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ArrayType  *result;
	int		   *result_ptr;
	int			size = 0;
	int			count;
	int			encoding = GetDatabaseEncoding();
	const unsigned char *utf = (unsigned char *) input;

	if (encoding != PG_UTF8)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Database encoding is not UTF-8")));

	/*
	 * Calculate the array size first by doing a first pass on the UTF-8 string
	 */
	while (*utf)
	{
		int l;

		l = pg_utf_mblen(utf);

		if (!pg_utf8_islegal(utf, l))
			elog(ERROR, "incorrect utf-8 input");

		size++;
		utf += l;
	}

	/*
	 * And now fill in the array with all the data from each character by
	 * doing a second pass.
	 */
	result = new_intArrayType(size);
	result_ptr = ARRPTR(result);
	utf = (unsigned char *) input;
	count = 0;
	while (*utf)
	{
		uint32	iutf = 0;
		int		l;

		l = pg_utf_mblen(utf);

		/* Calculate entry for character input for conversion table lookup */
		if (l == 1)
		{
			iutf = *utf++;
		}
		else if (l == 2)
		{
			iutf = *utf++ << 8;
			iutf |= *utf++;
		}
		else if (l == 3)
		{
			iutf = *utf++ << 16;
			iutf |= *utf++ << 8;
			iutf |= *utf++;
		}
		else if (l == 4)
		{
			iutf = *utf++ << 24;
			iutf |= *utf++ << 16;
			iutf |= *utf++ << 8;
			iutf |= *utf++;
		}
		else
			elog(ERROR, "incorrect multibyte length %d", l);

		/* Let's not care about any signing */
		result_ptr[count++] = (int32) iutf;
	}

	Assert(count == ARRNELEMS(result));

	PG_RETURN_POINTER(result);
}

/*
 * array_to_utf8
 * Convert a UTF-8 string into an integer array.
 */
PG_FUNCTION_INFO_V1(array_to_utf8);
Datum
array_to_utf8(PG_FUNCTION_ARGS)
{
	ArrayType	   *input = PG_GETARG_ARRAYTYPE_P(0);
	int			   *input_ptr = ARRPTR(input);
	char		   *result;
	int				size = 0;
	int				count = 0;
	int				i;

	/*
	 * Do a first pass on the array elements to calculate the size of the
	 * string to return.
	 */
	for (i = 0; i < ARRNELEMS(input); i++)
	{
		uint32 code = input_ptr[i];

		if (code & 0xff000000)
			size++;
		if (code & 0x00ff0000)
			size++;
		if (code & 0x0000ff00)
			size++;
		if (code & 0x000000ff)
			size++;
	}

	/* Now fill in the string */
	result = palloc0(size + 1);
	for (i = 0; i < ARRNELEMS(input); i++)
	{
		uint32 code = input_ptr[i];

		if (code & 0xff000000)
			result[count++] = code >> 24;
		if (code & 0x00ff0000)
			result[count++] = code >> 16;
		if (code & 0x0000ff00)
			result[count++] = code >> 8;
		if (code & 0x000000ff)
			result[count++] = code;
	}
	result[count] = '\0';

	Assert(count == size);
	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * utf8_conv_table
 * Return a full copy of the UTF-8 conversion table.
 */
PG_FUNCTION_INFO_V1(utf8_conv_table);
Datum
utf8_conv_table(PG_FUNCTION_ARGS)
{
	TupleDesc		tupdesc;
	Tuplestorestate *tupstore;
	ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext	per_query_ctx;
	MemoryContext	oldcontext;
	int				i;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build tuple descriptor */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	/* Print out all the values on the table */
	for (i = 0; i < lengthof(SASLPrepConv); i++)
	{
		Datum		values[3];
		bool		nulls[3];
		pg_utf_decomposition entry = SASLPrepConv[i];
		int			count;
		ArrayType  *decomp = NULL;
		int		   *decomp_ptr = NULL;

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		/* Fill in values, code first */
		values[0] = Int32GetDatum(entry.utf);

		/* class */
		values[1] = Int16GetDatum((int16) entry.class);

		/* decomposition array */
		if (entry.dec_size == 0)
			nulls[2] = true;
		else
		{
			uint32     *entry_decomp;

			/* Get decomposition of entry */
			entry_decomp = get_code_decomposition(&entry);

			decomp = new_intArrayType(entry.dec_size);
			decomp_ptr = ARRPTR(decomp);
			for (count = 0; count < entry.dec_size; count++)
				decomp_ptr[count] = (int) entry_decomp[count];
			values[2] = PointerGetDatum(decomp);
		}

		/* Save tuple values */
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		if (decomp != NULL)
			pfree(decomp);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}
