/*
 * This file implements universal function dispatching and promotion (which
 * is necessary to happen before dispatching).
 * This is part of the UFunc object.  Promotion and dispatching uses the
 * following things:
 *
 * - operand_DTypes:  The datatypes as passed in by the user.
 * - signature: The DTypes fixed by the user with `dtype=` or `signature=`.
 * - ufunc._loops: A list of all ArrayMethods and promoters, it contains
 *   tuples `(dtypes, ArrayMethod)` or `(dtypes, promoter)`.
 * - ufunc._dispatch_cache: A cache to store previous promotion and/or
 *   dispatching results.
 * - The actual arrays are used to support the old code paths where necessary.
 *   (this includes any value-based casting/promotion logic)
 *
 * In general, `operand_Dtypes` is always overridden by `signature`.  If a
 * DType is included in the `signature` it must match precisely.
 *
 * The process of dispatching and promotion can be summarized in the following
 * steps:
 *
 * 1. Override any `operand_DTypes` from `signature`.
 * 2. Check if the new `operand_Dtypes` is cached (if it is, got to 4.)
 * 3. Find the best matching "loop".  This is done using multiple dispatching
 *    on all `operand_DTypes` and loop `dtypes`.  A matching loop must be
 *    one whose DTypes are superclasses of the `operand_DTypes` (that are
 *    defined).  The best matching loop must be better than any other matching
 *    loop.  This result is cached.
 * 4. If the found loop is a promoter: We call the promoter. It can modify
 *    the `operand_DTypes` currently.  Then go back to step 2.
 *    (The promoter can call arbitrary code, so it could even add the matching
 *    loop first.)
 * 5. The final `ArrayMethod` is found, its registered `dtypes` is copied
 *    into the `signature` so that it is available to the ufunc loop.
 *
 */
#define NPY_NO_DEPRECATED_API NPY_API_VERSION
#define _MULTIARRAYMODULE
#define _UMATHMODULE

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "numpy/ndarraytypes.h"
#include "common.h"

#include "dispatching.h"
#include "dtypemeta.h"
#include "common_dtype.h"
#include "npy_hashtable.h"
#include "legacy_array_method.h"
#include "ufunc_object.h"
#include "ufunc_type_resolution.h"


#define PROMOTION_DEBUG_TRACING 0


/* forward declaration */
static NPY_INLINE PyObject *
promote_and_get_info_and_ufuncimpl(PyUFuncObject *ufunc,
        PyArrayObject *const ops[],
        PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *op_dtypes[],
        npy_bool allow_legacy_promotion);


/**
 * Function to add a new loop to the ufunc.  This mainly appends it to the
 * list (as it currently is just a list).
 *
 * @param ufunc The universal function to add the loop to.
 * @param info The tuple (dtype_tuple, ArrayMethod/promoter).
 * @param ignore_duplicate If 1 and a loop with the same `dtype_tuple` is
 *        found, the function does nothing.
 */
NPY_NO_EXPORT int
PyUFunc_AddLoop(PyUFuncObject *ufunc, PyObject *info, int ignore_duplicate)
{
    /*
     * Validate the info object, this should likely move to to a different
     * entry-point in the future (and is mostly unnecessary currently).
     */
    if (!PyTuple_CheckExact(info) || PyTuple_GET_SIZE(info) != 2) {
        PyErr_SetString(PyExc_TypeError,
                "Info must be a tuple: "
                "(tuple of DTypes or None, ArrayMethod or promoter)");
        return -1;
    }
    PyObject *DType_tuple = PyTuple_GetItem(info, 0);
    if (PyTuple_GET_SIZE(DType_tuple) != ufunc->nargs) {
        PyErr_SetString(PyExc_TypeError,
                "DType tuple length does not match ufunc number of operands");
        return -1;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(DType_tuple); i++) {
        PyObject *item = PyTuple_GET_ITEM(DType_tuple, i);
        if (item != Py_None
                && !PyObject_TypeCheck(item, &PyArrayDTypeMeta_Type)) {
            PyErr_SetString(PyExc_TypeError,
                    "DType tuple may only contain None and DType classes");
            return -1;
        }
    }
    PyObject *meth_or_promoter = PyTuple_GET_ITEM(info, 1);
    if (!PyObject_TypeCheck(meth_or_promoter, &PyArrayMethod_Type)
            && !PyCapsule_IsValid(meth_or_promoter, "numpy._ufunc_promoter")) {
        PyErr_SetString(PyExc_TypeError,
                "Second argument to info must be an ArrayMethod or promoter");
        return -1;
    }

    if (ufunc->_loops == NULL) {
        ufunc->_loops = PyList_New(0);
        if (ufunc->_loops == NULL) {
            return -1;
        }
    }

    PyObject *loops = ufunc->_loops;
    Py_ssize_t length = PyList_Size(loops);
    for (Py_ssize_t i = 0; i < length; i++) {
        PyObject *item = PyList_GetItem(loops, i);
        PyObject *cur_DType_tuple = PyTuple_GetItem(item, 0);
        int cmp = PyObject_RichCompareBool(cur_DType_tuple, DType_tuple, Py_EQ);
        if (cmp < 0) {
            return -1;
        }
        if (cmp == 0) {
            continue;
        }
        if (ignore_duplicate) {
            return 0;
        }
        PyErr_Format(PyExc_TypeError,
                "A loop/promoter has already been registered with '%s' for %R",
                ufunc_get_name_cstr(ufunc), DType_tuple);
        return -1;
    }

    if (PyList_Append(loops, info) < 0) {
        return -1;
    }
    return 0;
}


/**
 * Resolves the implementation to use, this uses typical multiple dispatching
 * methods of finding the best matching implementation or resolver.
 * (Based on `isinstance()`, the knowledge that non-abstract DTypes cannot
 * be subclassed is used, however.)
 *
 * NOTE: This currently does not take into account output dtypes which do not
 *       have to match.  The possible extension here is that if an output
 *       is given (and thus an output dtype), but not part of the signature
 *       we could ignore it for matching, but *prefer* a loop that matches
 *       better.
 *       Why is this not done currently?  First, it seems a niche feature that
 *       loops can only be distinguished based on the output dtype.  Second,
 *       there are some nasty theoretical things because:
 *
 *            np.add(f4, f4, out=f8)
 *            np.add(f4, f4, out=f8, dtype=f8)
 *
 *       are different, the first uses the f4 loop, the second the f8 loop.
 *       The problem is, that the current cache only uses the op_dtypes and
 *       both are `(f4, f4, f8)`.  The cache would need to store also which
 *       output was provided by `dtype=`/`signature=`.
 *
 * @param ufunc
 * @param op_dtypes The DTypes that are either passed in (defined by an
 *        operand) or defined by the `signature` as also passed in as
 *        `fixed_DTypes`.
 * @param out_info Returns the tuple describing the best implementation
 *        (consisting of dtypes and ArrayMethod or promoter).
 *        WARNING: Returns a borrowed reference!
 * @returns -1 on error 0 on success.  Note that the output can be NULL on
 *          success if nothing is found.
 */
static int
resolve_implementation_info(PyUFuncObject *ufunc,
        PyArray_DTypeMeta *op_dtypes[], npy_bool only_promoters,
        PyObject **out_info)
{
    int nin = ufunc->nin, nargs = ufunc->nargs;
    Py_ssize_t size = PySequence_Length(ufunc->_loops);
    PyObject *best_dtypes = NULL;
    PyObject *best_resolver_info = NULL;

#if PROMOTION_DEBUG_TRACING
    printf("Promoting for '%s' promoters only: %d\n",
            ufunc->name ? ufunc->name : "<unknown>", (int)only_promoters);
    printf("    DTypes: ");
    PyObject *tmp = PyArray_TupleFromItems(ufunc->nargs, op_dtypes, 1);
    PyObject_Print(tmp, stdout, 0);
    Py_DECREF(tmp);
    printf("\n");
    Py_DECREF(tmp);
#endif

    for (Py_ssize_t res_idx = 0; res_idx < size; res_idx++) {
        /* Test all resolvers  */
        PyObject *resolver_info = PySequence_Fast_GET_ITEM(
                ufunc->_loops, res_idx);

        if (only_promoters && PyObject_TypeCheck(
                    PyTuple_GET_ITEM(resolver_info, 1), &PyArrayMethod_Type)) {
            continue;
        }

        PyObject *curr_dtypes = PyTuple_GET_ITEM(resolver_info, 0);
        /*
         * Test if the current resolver matches, it could make sense to
         * reorder these checks to avoid the IsSubclass check as much as
         * possible.
         */

        npy_bool matches = NPY_TRUE;
        /*
         * NOTE: We currently match the output dtype exactly here, this is
         *       actually only necessary if the signature includes.
         *       Currently, we rely that op-dtypes[nin:nout] is NULLed if not.
         */
        for (Py_ssize_t i = 0; i < nargs; i++) {
            PyArray_DTypeMeta *given_dtype = op_dtypes[i];
            PyArray_DTypeMeta *resolver_dtype = (
                    (PyArray_DTypeMeta *)PyTuple_GET_ITEM(curr_dtypes, i));
            assert((PyObject *)given_dtype != Py_None);
            if (given_dtype == NULL) {
                if (i >= nin) {
                    /* Unspecified out always matches (see below for inputs) */
                    continue;
                }
                /*
                 * This is a reduce-like operation, which always have the form
                 * `(res_DType, op_DType, res_DType)`.  If the first and last
                 * dtype of the loops match, this should be reduce-compatible.
                 */
                if (PyTuple_GET_ITEM(curr_dtypes, 0)
                        == PyTuple_GET_ITEM(curr_dtypes, 2)) {
                    continue;
                }
            }

            if (resolver_dtype == (PyArray_DTypeMeta *)Py_None) {
                /* always matches */
                continue;
            }
            if (given_dtype == resolver_dtype) {
                continue;
            }
            if (!NPY_DT_is_abstract(resolver_dtype)) {
                matches = NPY_FALSE;
                break;
            }

            int subclass = PyObject_IsSubclass(
                    (PyObject *)given_dtype, (PyObject *)resolver_dtype);
            if (subclass < 0) {
                return -1;
            }
            if (!subclass) {
                matches = NPY_FALSE;
                break;
            }
            /*
             * TODO: Could consider allowing reverse subclass relation, i.e.
             *       the operation DType passed in to be abstract.  That
             *       definitely is OK for outputs (and potentially useful,
             *       you could enforce e.g. an inexact result).
             *       It might also be useful for some stranger promoters.
             */
        }
        if (!matches) {
            continue;
        }

        /* The resolver matches, but we have to check if it is better */
        if (best_dtypes != NULL) {
            int current_best = -1;  /* -1 neither, 0 current best, 1 new */
            /*
             * If both have concrete and None in the same position and
             * they are identical, we will continue searching using the
             * first best for comparison, in an attempt to find a better
             * one.
             * In all cases, we give up resolution, since it would be
             * necessary to compare to two "best" cases.
             */
            for (Py_ssize_t i = 0; i < nargs; i++) {
                if (i == ufunc->nin && current_best != -1) {
                    /* inputs prefer one loop and outputs have lower priority */
                    break;
                }

                int best;

                PyObject *prev_dtype = PyTuple_GET_ITEM(best_dtypes, i);
                PyObject *new_dtype = PyTuple_GET_ITEM(curr_dtypes, i);

                if (prev_dtype == new_dtype) {
                    /* equivalent, so this entry does not matter */
                    continue;
                }
                if (op_dtypes[i] == NULL) {
                    /*
                     * If an a dtype is NULL it always matches, so there is no
                     * point in defining one as more precise than the other.
                     */
                    continue;
                }
                /* If either is None, the other is strictly more specific */
                if (prev_dtype == Py_None) {
                    best = 1;
                }
                else if (new_dtype == Py_None) {
                    best = 0;
                }
                /*
                 * If both are concrete and not identical, this is
                 * ambiguous.
                 */
                else if (!NPY_DT_is_abstract((PyArray_DTypeMeta *)prev_dtype) &&
                         !NPY_DT_is_abstract((PyArray_DTypeMeta *)new_dtype)) {
                    /*
                     * Ambiguous unless they are identical (checked above),
                     * or one matches exactly.
                     */
                    if (prev_dtype == (PyObject *)op_dtypes[i]) {
                        best = 0;
                    }
                    else if (new_dtype == (PyObject *)op_dtypes[i]) {
                        best = 1;
                    }
                    else {
                        best = -1;
                    }
                }
                else if (!NPY_DT_is_abstract((PyArray_DTypeMeta *)prev_dtype)) {
                    /* old is not abstract, so better (both not possible) */
                    best = 0;
                }
                else if (!NPY_DT_is_abstract((PyArray_DTypeMeta *)new_dtype)) {
                    /* new is not abstract, so better (both not possible) */
                    best = 1;
                }
                /*
                 * TODO: This will need logic for abstract DTypes to decide if
                 *       one is a subclass of the other (And their subclass
                 *       relation is well defined).  For now, we bail out
                 *       in cas someone manages to get here.
                 */
                else {
                    PyErr_SetString(PyExc_NotImplementedError,
                            "deciding which one of two abstract dtypes is "
                            "a better match is not yet implemented.  This "
                            "will pick the better (or bail) in the future.");
                    *out_info = NULL;
                    return -1;
                }

                if (best == -1) {
                    /* no new info, nothing to update */
                    continue;
                }
                if ((current_best != -1) && (current_best != best)) {
                    /*
                     * We need a clear best, this could be tricky, unless
                     * the signature is identical, we would have to compare
                     * against both of the found ones until we find a
                     * better one.
                     * Instead, only support the case where they are
                     * identical.
                     */
                    /* TODO: Document the above comment, may need relaxing? */
                    current_best = -1;
                    break;
                }
                current_best = best;
            }

            if (current_best == -1) {
                /*
                 * We could not find a best loop, but promoters should be
                 * designed in a way to disambiguate such scenarios, so we
                 * retry the whole lookup using only promoters.
                 * (There is a small chance we already got two promoters.
                 * We just redo it anyway for simplicity.)
                 */
                if (!only_promoters) {
                    return resolve_implementation_info(ufunc,
                            op_dtypes, NPY_TRUE, out_info);
                }
                /*
                 * If this is already the retry, we are out of luck.  Promoters
                 * should be designed in a way that this cannot happen!
                 * (It should be noted, that the retry might not find anything
                 * and we still do a legacy lookup later.)
                 */
                PyObject *given = PyArray_TupleFromItems(
                        ufunc->nargs, (PyObject **)op_dtypes, 1);
                if (given != NULL) {
                    PyErr_Format(PyExc_RuntimeError,
                            "Could not find a loop for the inputs:\n    %S\n"
                            "The two promoters %S and %S matched the input "
                            "equally well.  Promoters must be designed "
                            "to be unambiguous.  NOTE: This indicates an error "
                            "in NumPy or an extending library and should be "
                            "reported.",
                            given, best_dtypes, curr_dtypes);
                    Py_DECREF(given);
                }
                *out_info = NULL;
                return 0;
            }
            else if (current_best == 0) {
                /* The new match is not better, continue looking. */
                continue;
            }
        }
        /* The new match is better (or there was no previous match) */
        best_dtypes = curr_dtypes;
        best_resolver_info = resolver_info;
    }
    if (best_dtypes == NULL) {
        /* The non-legacy lookup failed */
        *out_info = NULL;
        return 0;
    }

    *out_info = best_resolver_info;
    return 0;
}


/*
 * A promoter can currently be either a C-Capsule containing a promoter
 * function pointer, or a Python function.  Both of these can at this time
 * only return new operation DTypes (i.e. mutate the input while leaving
 * those defined by the `signature` unmodified).
 */
static PyObject *
call_promoter_and_recurse(PyUFuncObject *ufunc, PyObject *promoter,
        PyArray_DTypeMeta *op_dtypes[], PyArray_DTypeMeta *signature[],
        PyArrayObject *const operands[])
{
    int nargs = ufunc->nargs;
    PyObject *resolved_info = NULL;

    int promoter_result;
    PyArray_DTypeMeta *new_op_dtypes[NPY_MAXARGS];

    if (PyCapsule_CheckExact(promoter)) {
        /* We could also go the other way and wrap up the python function... */
        promoter_function *promoter_function = PyCapsule_GetPointer(promoter,
                "numpy._ufunc_promoter");
        if (promoter_function == NULL) {
            return NULL;
        }
        promoter_result = promoter_function(ufunc,
                op_dtypes, signature, new_op_dtypes);
    }
    else {
        PyErr_SetString(PyExc_NotImplementedError,
                "Calling python functions for promotion is not implemented.");
        return NULL;
    }
    if (promoter_result < 0) {
        return NULL;
    }
    /*
     * If none of the dtypes changes, we would recurse infinitely, abort.
     * (Of course it is nevertheless possible to recurse infinitely.)
     */
    int dtypes_changed = 0;
    for (int i = 0; i < nargs; i++) {
        if (new_op_dtypes[i] != op_dtypes[i]) {
            dtypes_changed = 1;
            break;
        }
    }
    if (!dtypes_changed) {
        goto finish;
    }

    /*
     * Do a recursive call, the promotion function has to ensure that the
     * new tuple is strictly more precise (thus guaranteeing eventual finishing)
     */
    if (Py_EnterRecursiveCall(" during ufunc promotion.") != 0) {
        goto finish;
    }
    resolved_info = promote_and_get_info_and_ufuncimpl(ufunc,
            operands, signature, new_op_dtypes,
            /* no legacy promotion */ NPY_FALSE);

    Py_LeaveRecursiveCall();

  finish:
    for (int i = 0; i < nargs; i++) {
        Py_XDECREF(new_op_dtypes[i]);
    }
    return resolved_info;
}


/*
 * Convert the DType `signature` into the tuple of descriptors that is used
 * by the old ufunc type resolvers in `ufunc_type_resolution.c`.
 *
 * Note that we do not need to pass the type tuple when we use the legacy path
 * for type resolution rather than promotion, since the signature is always
 * correct in that case.
 */
static int
_make_new_typetup(
        int nop, PyArray_DTypeMeta *signature[], PyObject **out_typetup) {
    *out_typetup = PyTuple_New(nop);
    if (*out_typetup == NULL) {
        return -1;
    }

    int none_count = 0;
    for (int i = 0; i < nop; i++) {
        PyObject *item;
        if (signature[i] == NULL) {
            item = Py_None;
            none_count++;
        }
        else {
            if (!NPY_DT_is_legacy(signature[i])
                    || NPY_DT_is_abstract(signature[i])) {
                /*
                 * The legacy type resolution can't deal with these.
                 * This path will return `None` or so in the future to
                 * set an error later if the legacy type resolution is used.
                 */
                PyErr_SetString(PyExc_RuntimeError,
                        "Internal NumPy error: new DType in signature not yet "
                        "supported. (This should be unreachable code!)");
                Py_SETREF(*out_typetup, NULL);
                return -1;
            }
            item = (PyObject *)signature[i]->singleton;
        }
        Py_INCREF(item);
        PyTuple_SET_ITEM(*out_typetup, i, item);
    }
    if (none_count == nop) {
        /* The whole signature was None, simply ignore type tuple */
        Py_DECREF(*out_typetup);
        *out_typetup = NULL;
    }
    return 0;
}


/*
 * Fills in the operation_DTypes with borrowed references.  This may change
 * the content, since it will use the legacy type resolution, which can special
 * case 0-D arrays (using value-based logic).
 */
static int
legacy_promote_using_legacy_type_resolver(PyUFuncObject *ufunc,
        PyArrayObject *const *ops, PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *operation_DTypes[], int *out_cacheable)
{
    int nargs = ufunc->nargs;
    PyArray_Descr *out_descrs[NPY_MAXARGS] = {NULL};

    PyObject *type_tuple = NULL;
    if (_make_new_typetup(nargs, signature, &type_tuple) < 0) {
        return -1;
    }

    /*
     * We use unsafe casting. This is of course not accurate, but that is OK
     * here, because for promotion/dispatching the casting safety makes no
     * difference.  Whether the actual operands can be casts must be checked
     * during the type resolution step (which may _also_ calls this!).
     */
    if (ufunc->type_resolver(ufunc,
            NPY_UNSAFE_CASTING, (PyArrayObject **)ops, type_tuple,
            out_descrs) < 0) {
        Py_XDECREF(type_tuple);
        /* Not all legacy resolvers clean up on failures: */
        for (int i = 0; i < nargs; i++) {
            Py_CLEAR(out_descrs[i]);
        }
        return -1;
    }
    Py_XDECREF(type_tuple);

    for (int i = 0; i < nargs; i++) {
        Py_XSETREF(operation_DTypes[i], NPY_DTYPE(out_descrs[i]));
        Py_INCREF(operation_DTypes[i]);
        Py_DECREF(out_descrs[i]);
    }
    /*
     * The PyUFunc_SimpleBinaryComparisonTypeResolver has a deprecation
     * warning (ignoring `dtype=`) and cannot be cached.
     * All datetime ones *should* have a warning, but currently don't,
     * but ignore all signature passing also.  So they can also
     * not be cached, and they mutate the signature which of course is wrong,
     * but not doing it would confuse the code later.
     */
    for (int i = 0; i < nargs; i++) {
        if (signature[i] != NULL && signature[i] != operation_DTypes[i]) {
            Py_INCREF(operation_DTypes[i]);
            Py_SETREF(signature[i], operation_DTypes[i]);
            *out_cacheable = 0;
        }
    }
    return 0;
}


/*
 * Note, this function returns a BORROWED references to info since it adds
 * it to the loops.
 */
NPY_NO_EXPORT PyObject *
add_and_return_legacy_wrapping_ufunc_loop(PyUFuncObject *ufunc,
        PyArray_DTypeMeta *operation_dtypes[], int ignore_duplicate)
{
    PyObject *DType_tuple = PyArray_TupleFromItems(ufunc->nargs,
            (PyObject **)operation_dtypes, 0);
    if (DType_tuple == NULL) {
        return NULL;
    }

    PyArrayMethodObject *method = PyArray_NewLegacyWrappingArrayMethod(
            ufunc, operation_dtypes);
    if (method == NULL) {
        Py_DECREF(DType_tuple);
        return NULL;
    }
    PyObject *info = PyTuple_Pack(2, DType_tuple, method);
    Py_DECREF(DType_tuple);
    Py_DECREF(method);
    if (info == NULL) {
        return NULL;
    }
    if (PyUFunc_AddLoop(ufunc, info, ignore_duplicate) < 0) {
        Py_DECREF(info);
        return NULL;
    }
    Py_DECREF(info);  /* now borrowed from the ufunc's list of loops */
    return info;
}


/*
 * The main implementation to find the correct DType signature and ArrayMethod
 * to use for a ufunc.  This function may recurse with `do_legacy_fallback`
 * set to False.
 *
 * If value-based promotion is necessary, this is handled ahead of time by
 * `promote_and_get_ufuncimpl`.
 */
static NPY_INLINE PyObject *
promote_and_get_info_and_ufuncimpl(PyUFuncObject *ufunc,
        PyArrayObject *const ops[],
        PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *op_dtypes[],
        npy_bool allow_legacy_promotion)
{
    /*
     * Fetch the dispatching info which consists of the implementation and
     * the DType signature tuple.  There are three steps:
     *
     * 1. Check the cache.
     * 2. Check all registered loops/promoters to find the best match.
     * 3. Fall back to the legacy implementation if no match was found.
     */
    PyObject *info = PyArrayIdentityHash_GetItem(ufunc->_dispatch_cache,
                (PyObject **)op_dtypes);
    if (info != NULL && PyObject_TypeCheck(
            PyTuple_GET_ITEM(info, 1), &PyArrayMethod_Type)) {
        /* Found the ArrayMethod and NOT a promoter: return it */
        return info;
    }

    /*
     * If `info == NULL`, loading from cache failed, use the full resolution
     * in `resolve_implementation_info` (which caches its result on success).
     */
    if (info == NULL) {
        if (resolve_implementation_info(ufunc,
                op_dtypes, NPY_FALSE, &info) < 0) {
            return NULL;
        }
        if (info != NULL && PyObject_TypeCheck(
                PyTuple_GET_ITEM(info, 1), &PyArrayMethod_Type)) {
            /*
             * Found the ArrayMethod and NOT promoter.  Before returning it
             * add it to the cache for faster lookup in the future.
             */
            if (PyArrayIdentityHash_SetItem(ufunc->_dispatch_cache,
                    (PyObject **)op_dtypes, info, 0) < 0) {
                return NULL;
            }
            return info;
        }
    }

    /*
     * At this point `info` is NULL if there is no matching loop, or it is
     * a promoter that needs to be used/called:
     */
    if (info != NULL) {
        PyObject *promoter = PyTuple_GET_ITEM(info, 1);

        info = call_promoter_and_recurse(ufunc,
                promoter, op_dtypes, signature, ops);
        if (info == NULL && PyErr_Occurred()) {
            return NULL;
        }
        else if (info != NULL) {
            /* Add result to the cache using the original types: */
            if (PyArrayIdentityHash_SetItem(ufunc->_dispatch_cache,
                    (PyObject **)op_dtypes, info, 0) < 0) {
                return NULL;
            }
            return info;
        }
    }

    /*
     * Even using promotion no loop was found.
     * Using promotion failed, this should normally be an error.
     * However, we need to give the legacy implementation a chance here.
     * (it will modify `op_dtypes`).
     */
    if (!allow_legacy_promotion || ufunc->type_resolver == NULL ||
            (ufunc->ntypes == 0 && ufunc->userloops == NULL)) {
        /* Already tried or not a "legacy" ufunc (no loop found, return) */
        return NULL;
    }

    PyArray_DTypeMeta *new_op_dtypes[NPY_MAXARGS] = {NULL};
    int cacheable = 1;  /* TODO: only the comparison deprecation needs this */
    if (legacy_promote_using_legacy_type_resolver(ufunc,
            ops, signature, new_op_dtypes, &cacheable) < 0) {
        return NULL;
    }
    info = promote_and_get_info_and_ufuncimpl(ufunc,
            ops, signature, new_op_dtypes, NPY_FALSE);
    for (int i = 0; i < ufunc->nargs; i++) {
        Py_XDECREF(new_op_dtypes[i]);
    }

    /* Add this to the cache using the original types: */
    if (cacheable && PyArrayIdentityHash_SetItem(ufunc->_dispatch_cache,
            (PyObject **)op_dtypes, info, 0) < 0) {
        return NULL;
    }
    return info;
}


/**
 * The central entry-point for the promotion and dispatching machinery.
 *
 * It currently may work with the operands (although it would be possible to
 * only work with DType (classes/types).  This is because it has to ensure
 * that legacy (value-based promotion) is used when necessary.
 *
 * NOTE: The machinery here currently ignores output arguments unless
 *       they are part of the signature.  This slightly limits unsafe loop
 *       specializations, which is important for the `ensure_reduce_compatible`
 *       fallback mode.
 *       To fix this, the caching mechanism (and dispatching) can be extended.
 *       When/if that happens, the `ensure_reduce_compatible` could be
 *       deprecated (it should never kick in because promotion kick in first).
 *
 * @param ufunc The ufunc object, used mainly for the fallback.
 * @param ops The array operands (used only for the fallback).
 * @param signature As input, the DType signature fixed explicitly by the user.
 *        The signature is *filled* in with the operation signature we end up
 *        using.
 * @param op_dtypes The operand DTypes (without casting) which are specified
 *        either by the `signature` or by an `operand`.
 *        (outputs and the second input can be NULL for reductions).
 *        NOTE: In some cases, the promotion machinery may currently modify
 *        these including clearing the output.
 * @param force_legacy_promotion If set, we have to use the old type resolution
 *        to implement value-based promotion/casting.
 * @param ensure_reduce_compatible Must be set for reductions, in which case
 *        the found implementation is checked for reduce-like compatibility.
 *        If it is *not* compatible and `signature[2] != NULL`, we assume its
 *        output DType is correct (see NOTE above).
 *        If removed, promotion may require information about whether this
 *        is a reduction, so the more likely case is to always keep fixing this
 *        when necessary, but push down the handling so it can be cached.
 */
NPY_NO_EXPORT PyArrayMethodObject *
promote_and_get_ufuncimpl(PyUFuncObject *ufunc,
        PyArrayObject *const ops[],
        PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *op_dtypes[],
        npy_bool force_legacy_promotion,
        npy_bool allow_legacy_promotion,
        npy_bool ensure_reduce_compatible)
{
    int nin = ufunc->nin, nargs = ufunc->nargs;

    /*
     * Get the actual DTypes we operate with by mixing the operand array
     * ones with the passed signature.
     */
    for (int i = 0; i < nargs; i++) {
        if (signature[i] != NULL) {
            /*
             * ignore the operand input, we cannot overwrite signature yet
             * since it is fixed (cannot be promoted!)
             */
            Py_INCREF(signature[i]);
            Py_XSETREF(op_dtypes[i], signature[i]);
            assert(i >= ufunc->nin || !NPY_DT_is_abstract(signature[i]));
        }
        else if (i >= nin) {
            /*
             * We currently just ignore outputs if not in signature, this will
             * always give the/a correct result (limits registering specialized
             * loops which include the cast).
             * (See also comment in resolve_implementation_info.)
             */
            Py_CLEAR(op_dtypes[i]);
        }
    }

    if (force_legacy_promotion) {
        /*
         * We must use legacy promotion for value-based logic. Call the old
         * resolver once up-front to get the "actual" loop dtypes.
         * After this (additional) promotion, we can even use normal caching.
         */
        int cacheable = 1;  /* unused, as we modify the original `op_dtypes` */
        if (legacy_promote_using_legacy_type_resolver(ufunc,
                ops, signature, op_dtypes, &cacheable) < 0) {
            return NULL;
        }
    }

    PyObject *info = promote_and_get_info_and_ufuncimpl(ufunc,
            ops, signature, op_dtypes, allow_legacy_promotion);

    if (info == NULL) {
        if (!PyErr_Occurred()) {
            raise_no_loop_found_error(ufunc, (PyObject **)op_dtypes);
        }
        return NULL;
    }

    PyArrayMethodObject *method = (PyArrayMethodObject *)PyTuple_GET_ITEM(info, 1);

    /*
     * In certain cases (only the logical ufuncs really), the loop we found may
     * not be reduce-compatible.  Since the machinery can't distinguish a
     * reduction with an output from a normal ufunc call, we have to assume
     * the result DType is correct and force it for the input (if not forced
     * already).
     * NOTE: This does assume that all loops are "safe" see the NOTE in this
     *       comment.  That could be relaxed, in which case we may need to
     *       cache if a call was for a reduction.
     */
    PyObject *all_dtypes = PyTuple_GET_ITEM(info, 0);
    if (ensure_reduce_compatible && signature[0] == NULL &&
            PyTuple_GET_ITEM(all_dtypes, 0) != PyTuple_GET_ITEM(all_dtypes, 2)) {
        signature[0] = (PyArray_DTypeMeta *)PyTuple_GET_ITEM(all_dtypes, 2);
        Py_INCREF(signature[0]);
        return promote_and_get_ufuncimpl(ufunc,
                ops, signature, op_dtypes,
                force_legacy_promotion, allow_legacy_promotion, NPY_FALSE);
    }

    for (int i = 0; i < nargs; i++) {
        if (signature[i] == NULL) {
            signature[i] = (PyArray_DTypeMeta *)PyTuple_GET_ITEM(all_dtypes, i);
            Py_INCREF(signature[i]);
        }
        else {
            assert((PyObject *)signature[i] == PyTuple_GET_ITEM(all_dtypes, i));
        }
    }

    return method;
}


/*
 * Generic promoter used by as a final fallback on ufuncs.  Most operations are
 * homogeneous, so we can try to find the homogeneous dtype on the inputs
 * and use that.
 * We need to special case the reduction case, where op_dtypes[0] == NULL
 * is possible.
 */
NPY_NO_EXPORT int
default_ufunc_promoter(PyUFuncObject *ufunc,
        PyArray_DTypeMeta *op_dtypes[], PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *new_op_dtypes[])
{
    if (ufunc->type_resolver == &PyUFunc_SimpleBinaryComparisonTypeResolver
            && signature[0] == NULL && signature[1] == NULL
            && signature[2] != NULL && signature[2]->type_num != NPY_BOOL) {
        /* bail out, this is _only_ to give future/deprecation warning! */
        return -1;
    }

    /* If nin < 2 promotion is a no-op, so it should not be registered */
    assert(ufunc->nin > 1);
    if (op_dtypes[0] == NULL) {
        assert(ufunc->nin == 2 && ufunc->nout == 1);  /* must be reduction */
        Py_INCREF(op_dtypes[1]);
        new_op_dtypes[0] = op_dtypes[1];
        Py_INCREF(op_dtypes[1]);
        new_op_dtypes[1] = op_dtypes[1];
        Py_INCREF(op_dtypes[1]);
        new_op_dtypes[2] = op_dtypes[1];
        return 0;
    }
    PyArray_DTypeMeta *common = NULL;
    /*
     * If a signature is used and homogeneous in its outputs use that
     * (Could/should likely be rather applied to inputs also, although outs
     * only could have some advantage and input dtypes are rarely enforced.)
     */
    for (int i = ufunc->nin; i < ufunc->nargs; i++) {
        if (signature[i] != NULL) {
            if (common == NULL) {
                Py_INCREF(signature[i]);
                common = signature[i];
            }
            else if (common != signature[i]) {
                Py_CLEAR(common);  /* Not homogeneous, unset common */
                break;
            }
        }
    }
    /* Otherwise, use the common DType of all input operands */
    if (common == NULL) {
        common = PyArray_PromoteDTypeSequence(ufunc->nin, op_dtypes);
        if (common == NULL) {
            if (PyErr_ExceptionMatches(PyExc_TypeError)) {
                PyErr_Clear();  /* Do not propagate normal promotion errors */
            }
            return -1;
        }
    }

    for (int i = 0; i < ufunc->nargs; i++) {
        PyArray_DTypeMeta *tmp = common;
        if (signature[i]) {
            tmp = signature[i];  /* never replace a fixed one. */
        }
        Py_INCREF(tmp);
        new_op_dtypes[i] = tmp;
    }
    for (int i = ufunc->nin; i < ufunc->nargs; i++) {
        Py_XINCREF(op_dtypes[i]);
        new_op_dtypes[i] = op_dtypes[i];
    }

    Py_DECREF(common);
    return 0;
}


/*
 * In some cases, we assume that there will only ever be object loops,
 * and the object loop should *always* be chosen.
 * (in those cases more specific loops should not really be registered, but
 * we do not check that.)
 *
 * We default to this for "old-style" ufuncs which have exactly one loop
 * consisting only of objects (during registration time, numba mutates this
 * but presumably).
 */
NPY_NO_EXPORT int
object_only_ufunc_promoter(PyUFuncObject *ufunc,
        PyArray_DTypeMeta *NPY_UNUSED(op_dtypes[]),
        PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *new_op_dtypes[])
{
    PyArray_DTypeMeta *object_DType = PyArray_DTypeFromTypeNum(NPY_OBJECT);

    for (int i = 0; i < ufunc->nargs; i++) {
        if (signature[i] == NULL) {
            Py_INCREF(object_DType);
            new_op_dtypes[i] = object_DType;
        }
    }
    Py_DECREF(object_DType);
    return 0;
}

/*
 * Special promoter for the logical ufuncs.  The logical ufuncs can always
 * use the ??->? and still get the correct output (as long as the output
 * is not supposed to be `object`).
 */
static int
logical_ufunc_promoter(PyUFuncObject *NPY_UNUSED(ufunc),
        PyArray_DTypeMeta *op_dtypes[], PyArray_DTypeMeta *signature[],
        PyArray_DTypeMeta *new_op_dtypes[])
{
    /*
     * If we find any object DType at all, we currently force to object.
     * However, if the output is specified and not object, there is no point,
     * it should be just as well to cast the input rather than doing the
     * unsafe out cast.
     */
    int force_object = 0;

    if (signature[0] == NULL && signature[1] == NULL
            && signature[2] != NULL && signature[2]->type_num != NPY_BOOL) {
        /* bail out, this is _only_ to give future/deprecation warning! */
        return -1;
    }
    if ((op_dtypes[0] != NULL && PyTypeNum_ISSTRING(op_dtypes[0]->type_num))
            || PyTypeNum_ISSTRING(op_dtypes[1]->type_num)) {
        /* bail out on strings: currently casting them to bool is too weird */
        return -1;
    }

    for (int i = 0; i < 3; i++) {
        PyArray_DTypeMeta *item;
        if (signature[i] != NULL) {
            item = signature[i];
            Py_INCREF(item);
            if (item->type_num == NPY_OBJECT) {
                force_object = 1;
            }
        }
        else {
            /* Always override to boolean */
            item = PyArray_DTypeFromTypeNum(NPY_BOOL);
            if (op_dtypes[i] != NULL && op_dtypes[i]->type_num == NPY_OBJECT) {
                force_object = 1;
            }
        }
        new_op_dtypes[i] = item;
    }

    if (!force_object || (op_dtypes[2] != NULL
                          && op_dtypes[2]->type_num != NPY_OBJECT)) {
        return 0;
    }
    /*
     * Actually, we have to use the OBJECT loop after all, set all we can
     * to object (that might not work out, but try).
     *
     * NOTE: Change this to check for `op_dtypes[0] == NULL` to STOP
     *       returning `object` for `np.logical_and.reduce(obj_arr)`
     *       which will also affect `np.all` and `np.any`!
     */
    for (int i = 0; i < 3; i++) {
        if (signature[i] != NULL) {
            continue;
        }
        Py_SETREF(new_op_dtypes[i], PyArray_DTypeFromTypeNum(NPY_OBJECT));
    }
    return 0;
}


NPY_NO_EXPORT int
install_logical_ufunc_promoter(PyObject *ufunc)
{
    if (PyObject_Type(ufunc) != (PyObject *)&PyUFunc_Type) {
        PyErr_SetString(PyExc_RuntimeError,
                "internal numpy array, logical ufunc was not a ufunc?!");
        return -1;
    }
    PyObject *dtype_tuple = PyTuple_Pack(3,
            &PyArrayDescr_Type, &PyArrayDescr_Type, &PyArrayDescr_Type, NULL);
    if (dtype_tuple == NULL) {
        return -1;
    }
    PyObject *promoter = PyCapsule_New(&logical_ufunc_promoter,
            "numpy._ufunc_promoter", NULL);
    if (promoter == NULL) {
        Py_DECREF(dtype_tuple);
        return -1;
    }

    PyObject *info = PyTuple_Pack(2, dtype_tuple, promoter);
    Py_DECREF(dtype_tuple);
    Py_DECREF(promoter);
    if (info == NULL) {
        return -1;
    }

    return PyUFunc_AddLoop((PyUFuncObject *)ufunc, info, 0);
}
