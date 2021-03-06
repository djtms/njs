
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_types.h>
#include <nxt_clang.h>
#include <nxt_alignment.h>
#include <nxt_string.h>
#include <nxt_stub.h>
#include <nxt_djb_hash.h>
#include <nxt_array.h>
#include <nxt_lvlhsh.h>
#include <nxt_random.h>
#include <nxt_mem_cache_pool.h>
#include <njscript.h>
#include <njs_vm.h>
#include <njs_number.h>
#include <njs_string.h>
#include <njs_object.h>
#include <njs_object_hash.h>
#include <njs_array.h>
#include <njs_function.h>
#include <string.h>


typedef struct {
    njs_continuation_t      cont;
    njs_value_t             *values;
    uint32_t                max;
} njs_array_join_t;


typedef struct {
    union {
        njs_continuation_t  cont;
        u_char              padding[NJS_CONTINUATION_SIZE];
    } u;
    /*
     * This retval value must be aligned so the continuation is padded
     * to aligned size.
     */
    njs_value_t             retval;

    uint32_t                next_index;
    uint32_t                length;
} njs_array_iter_t;


typedef struct {
    njs_array_iter_t        iter;
    njs_value_t             value;
    njs_array_t             *array;
} njs_array_filter_t;


typedef struct {
    njs_array_iter_t        iter;
    njs_array_t             *array;
    uint32_t                index;
} njs_array_map_t;


typedef struct {
    union {
        njs_continuation_t  cont;
        u_char              padding[NJS_CONTINUATION_SIZE];
    } u;
    /*
     * This retval value must be aligned so the continuation is padded
     * to aligned size.
     */
    njs_value_t             retval;

    njs_function_t          *function;
    int32_t                 index;
    uint32_t                current;
} njs_array_sort_t;


static njs_ret_t njs_array_prototype_to_string_continuation(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t nargs, njs_index_t retval);
static njs_ret_t njs_array_prototype_join_continuation(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t nargs, njs_index_t unused);
static njs_value_t *njs_array_copy(njs_value_t *dst, njs_value_t *src);
static njs_ret_t njs_array_index_of(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, nxt_bool_t first);
static njs_ret_t njs_array_prototype_for_each_continuation(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t nargs, njs_index_t unused);
static njs_ret_t njs_array_prototype_some_continuation(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t nargs, njs_index_t unused);
static njs_ret_t njs_array_prototype_every_continuation(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t nargs, njs_index_t unused);
static njs_ret_t njs_array_prototype_filter_continuation(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t nargs, njs_index_t unused);
static njs_ret_t njs_array_prototype_map_continuation(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t nargs, njs_index_t unused);
static njs_ret_t njs_array_prototype_reduce_continuation(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t nargs, njs_index_t unused);
static njs_ret_t njs_array_prototype_reduce_right_continuation(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t nargs, njs_index_t unused);
static nxt_noinline njs_ret_t njs_array_iterator_args(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t nargs);
static nxt_noinline uint32_t njs_array_iterator_next(njs_array_t *array,
    uint32_t n, uint32_t length);
static nxt_noinline njs_ret_t njs_array_iterator_apply(njs_vm_t *vm,
    njs_array_iter_t *iter, njs_value_t *args, nxt_uint_t nargs);
static uint32_t njs_array_reduce_right_next(njs_array_t *array, int32_t n);
static njs_ret_t njs_array_prototype_sort_continuation(njs_vm_t *vm,
    njs_value_t *args, nxt_uint_t nargs, njs_index_t unused);


nxt_noinline njs_array_t *
njs_array_alloc(njs_vm_t *vm, uint32_t length, uint32_t spare)
{
    uint32_t     size;
    njs_array_t  *array;

    array = nxt_mem_cache_alloc(vm->mem_cache_pool, sizeof(njs_array_t));
    if (nxt_slow_path(array == NULL)) {
        return NULL;
    }

    size = length + spare;

    array->data = nxt_mem_cache_align(vm->mem_cache_pool, sizeof(njs_value_t),
                                      size * sizeof(njs_value_t));
    if (nxt_slow_path(array->data == NULL)) {
        return NULL;
    }

    array->start = array->data;
    nxt_lvlhsh_init(&array->object.hash);
    nxt_lvlhsh_init(&array->object.shared_hash);
    array->object.__proto__ = &vm->prototypes[NJS_PROTOTYPE_ARRAY];
    array->object.shared = 0;
    array->size = size;
    array->length = length;

    return array;
}


static njs_ret_t
njs_array_add(njs_vm_t *vm, njs_array_t *array, njs_value_t *value)
{
    njs_ret_t  ret;

    if (array->size == array->length) {
        ret = njs_array_realloc(vm, array, 0, array->size + 1);
        if (nxt_slow_path(ret != NXT_OK)) {
            return ret;
        }
    }

    /* GC: retain value. */
    array->start[array->length++] = *value;

    return NXT_OK;
}


njs_ret_t
njs_array_string_add(njs_vm_t *vm, njs_array_t *array, u_char *start,
    size_t size, size_t length)
{
    njs_ret_t  ret;

    if (array->size == array->length) {
        ret = njs_array_realloc(vm, array, 0, array->size + 1);
        if (nxt_slow_path(ret != NXT_OK)) {
            return ret;
        }
    }

    return njs_string_create(vm, &array->start[array->length++],
                            start, size, length);
}


njs_ret_t
njs_array_realloc(njs_vm_t *vm, njs_array_t *array, uint32_t prepend,
    uint32_t size)
{
    nxt_uint_t   n;
    njs_value_t  *value, *old;

    if (size != array->size) {
        if (size < 16) {
            size *= 2;

        } else {
            size += size / 2;
        }
    }

    value = nxt_mem_cache_align(vm->mem_cache_pool, sizeof(njs_value_t),
                                (prepend + size) * sizeof(njs_value_t));
    if (nxt_slow_path(value == NULL)) {
        return NXT_ERROR;
    }

    old = array->data;
    array->data = value;

    while (prepend != 0) {
        njs_set_invalid(value);
        value++;
        prepend--;
    }

    memcpy(value, array->start, array->size * sizeof(njs_value_t));

    array->start = value;
    n = array->size;
    array->size = size;

    value += n;
    size -= n;

    while (size != 0) {
        njs_set_invalid(value);
        value++;
        size--;
    }

    nxt_mem_cache_free(vm->mem_cache_pool, old);

    return NXT_OK;
}


njs_ret_t
njs_array_constructor(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    double       num;
    uint32_t     size;
    njs_value_t  *value;
    njs_array_t  *array;

    args = &args[1];
    size = nargs - 1;

    if (size == 1 && njs_is_number(&args[0])) {
        num = args[0].data.u.number;
        size = (uint32_t) num;

        if ((double) size != num) {
            vm->exception = &njs_exception_range_error;
            return NXT_ERROR;
        }

        args = NULL;
    }

    array = njs_array_alloc(vm, size, NJS_ARRAY_SPARE);

    if (nxt_fast_path(array != NULL)) {

        vm->retval.data.u.array = array;
        value = array->start;

        if (args == NULL) {
            while (size != 0) {
                njs_set_invalid(value);
                value++;
                size--;
            }

        } else {
            while (size != 0) {
                njs_retain(args);
                *value++ = *args++;
                size--;
            }
        }

        vm->retval.type = NJS_ARRAY;
        vm->retval.data.truth = 1;

        return NXT_OK;
    }

    return NXT_ERROR;
}


static njs_ret_t
njs_array_is_array(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    const njs_value_t  *value;

    if (nargs > 1 && njs_is_array(&args[1])) {
        value = &njs_string_true;

    } else {
        value = &njs_string_false;
    }

    vm->retval = *value;

    return NXT_OK;
}


static const njs_object_prop_t  njs_array_constructor_properties[] =
{
    /* Array.name == "Array". */
    {
        .type = NJS_PROPERTY,
        .name = njs_string("name"),
        .value = njs_string("Array"),
    },

    /* Array.length == 1. */
    {
        .type = NJS_PROPERTY,
        .name = njs_string("length"),
        .value = njs_value(NJS_NUMBER, 1, 1.0),
    },

    /* Array.prototype. */
    {
        .type = NJS_NATIVE_GETTER,
        .name = njs_string("prototype"),
        .value = njs_native_getter(njs_object_prototype_create),
    },

    /* Array.isArray(). */
    {
        .type = NJS_METHOD,
        .name = njs_string("isArray"),
        .value = njs_native_function(njs_array_is_array, 0, 0),
    },
};


const njs_object_init_t  njs_array_constructor_init = {
    njs_array_constructor_properties,
    nxt_nitems(njs_array_constructor_properties),
};


static njs_ret_t
njs_array_prototype_length(njs_vm_t *vm, njs_value_t *array)
{
    njs_number_set(&vm->retval, array->data.u.array->length);

    njs_release(vm, array);

    return NXT_OK;
}


/*
 * Array.slice(start[, end]).
 * JavaScript 1.2, ECMAScript 3.
 */

static njs_ret_t
njs_array_prototype_slice(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    int32_t      start, end, length;
    uint32_t     n;
    njs_array_t  *array;
    njs_value_t  *value;

    start = 0;
    length = 0;

    if (njs_is_array(&args[0])) {
        length = args[0].data.u.array->length;

        if (nargs > 1) {
            start = args[1].data.u.number;

            if (start < 0) {
                start += length;

                if (start < 0) {
                    start = 0;
                }
            }

            end = length;

            if (nargs > 2) {
                end = args[2].data.u.number;

                if (end < 0) {
                    end += length;
                }
            }

            length = end - start;

            if (length < 0) {
                start = 0;
                length = 0;
            }
        }
    }

    array = njs_array_alloc(vm, length, NJS_ARRAY_SPARE);
    if (nxt_slow_path(array == NULL)) {
        return NXT_ERROR;
    }

    vm->retval.data.u.array = array;
    vm->retval.type = NJS_ARRAY;
    vm->retval.data.truth = 1;

    if (length != 0) {
        value = args[0].data.u.array->start;
        n = 0;

        do {
            /* GC: retain long string and object in values[start]. */
            array->start[n++] = value[start++];
            length--;
        } while (length != 0);
    }

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_push(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    njs_ret_t    ret;
    nxt_uint_t   i;
    njs_array_t  *array;

    if (njs_is_array(&args[0])) {
        array = args[0].data.u.array;

        if (nargs != 0) {
            if (nargs > array->size - array->length) {
                ret = njs_array_realloc(vm, array, 0, array->size + nargs);
                if (nxt_slow_path(ret != NXT_OK)) {
                    return ret;
                }
            }

            for (i = 1; i < nargs; i++) {
                /* GC: njs_retain(&args[i]); */
                array->start[array->length++] = args[i];
            }
        }

        njs_number_set(&vm->retval, array->length);
    }

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_pop(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    njs_array_t        *array;
    const njs_value_t  *retval, *value;

    retval = &njs_value_void;

    if (njs_is_array(&args[0])) {
        array = args[0].data.u.array;

        if (array->length != 0) {
            array->length--;
            value = &array->start[array->length];

            if (njs_is_valid(value)) {
                retval = value;
            }
        }
    }

    vm->retval = *retval;

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_unshift(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    njs_ret_t    ret;
    nxt_uint_t   n;
    njs_array_t  *array;

    if (njs_is_array(&args[0])) {
        array = args[0].data.u.array;
        n = nargs - 1;

        if (n != 0) {
            if ((intptr_t) n > (array->start - array->data)) {
                ret = njs_array_realloc(vm, array, n, array->size);
                if (nxt_slow_path(ret != NXT_OK)) {
                    return ret;
                }
            }

            array->length += n;
            n = nargs;

            do {
                n--;
                /* GC: njs_retain(&args[n]); */
                array->start--;
                array->start[0] = args[n];
            } while (n > 1);
        }

        njs_number_set(&vm->retval, array->length);
    }

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_shift(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    njs_array_t        *array;
    const njs_value_t  *retval, *value;

    retval = &njs_value_void;

    if (njs_is_array(&args[0])) {
        array = args[0].data.u.array;

        if (array->length != 0) {
            array->length--;

            value = &array->start[0];
            array->start++;

            if (njs_is_valid(value)) {
                retval = value;
            }
        }
    }

    vm->retval = *retval;

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_splice(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    njs_ret_t    ret;
    nxt_int_t    items, delta;
    nxt_uint_t   i, n, start, delete, length;
    njs_array_t  *array, *deleted;

    array = NULL;
    start = 0;
    delete = 0;

    if (njs_is_array(&args[0])) {
        array = args[0].data.u.array;

        if (nargs > 1) {
            start = args[1].data.u.number;

            if (nargs > 2) {
                delete = args[2].data.u.number;

            } else {
                delete = array->length - start;
            }
        }
    }

    deleted = njs_array_alloc(vm, delete, 0);
    if (nxt_slow_path(deleted == NULL)) {
        return NXT_ERROR;
    }

    if (array != NULL && (delete != 0 || nargs > 3)) {
        length = array->length;

        /* Move deleted items to a new array to return. */
        for (i = 0, n = start; i < delete && n < length; i++, n++) {
            /* No retention required. */
            deleted->start[i] = array->start[n];
        }

        items = nargs - 3;
        items = items >= 0 ? items : 0;
        delta = items - delete;

        if (delta != 0) {
            /*
             * Relocate the rest of items.
             * Index of the first item is in "n".
             */
            if (delta > 0) {
                ret = njs_array_realloc(vm, array, 0, array->size + delta);
                if (nxt_slow_path(ret != NXT_OK)) {
                    return ret;
                }
            }

            memmove(&array->start[start + items], &array->start[n],
                    (array->length - n) * sizeof(njs_value_t));

            array->length += delta;
        }

        /* Copy new items. */
        n = start;

        for (i = 3; i < nargs; i++) {
            /* GC: njs_retain(&args[i]); */
            array->start[n++] = args[i];
        }
    }

    vm->retval.data.u.array = deleted;
    vm->retval.type = NJS_ARRAY;
    vm->retval.data.truth = 1;

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_reverse(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    nxt_uint_t   i, n, length;
    njs_value_t  value;
    njs_array_t  *array;

    if (njs_is_array(&args[0])) {
        array = args[0].data.u.array;
        length = array->length;

        if (length > 1) {
            for (i = 0, n = length - 1; i < n; i++, n--) {
                value = array->start[i];
                array->start[i] = array->start[n];
                array->start[n] = value;
            }
        }

        vm->retval.data.u.array = array;
        vm->retval.type = NJS_ARRAY;
        vm->retval.data.truth = 1;

    } else {
        /* STUB */
        vm->retval = args[0];
    }

    return NXT_OK;
}


/*
 * ECMAScript 5.1: try first to use object method "join", then
 * use the standard built-in method Object.prototype.toString().
 * Array.toString() must be a continuation otherwise it may
 * endlessly call Array.join().
 */

static njs_ret_t
njs_array_prototype_to_string(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t retval)
{
    njs_object_prop_t   *prop;
    njs_continuation_t  *cont;
    nxt_lvlhsh_query_t  lhq;

    cont = (njs_continuation_t *) njs_continuation(vm->frame);
    cont->function = njs_array_prototype_to_string_continuation;

    if (njs_is_object(&args[0])) {
        lhq.key_hash = NJS_JOIN_HASH;
        lhq.key = nxt_string_value("join");

        prop = njs_object_property(vm, args[0].data.u.object, &lhq);

        if (nxt_fast_path(prop != NULL && njs_is_function(&prop->value))) {
            return njs_function_apply(vm, prop->value.data.u.function,
                                      args, nargs, retval);
        }
    }

    return njs_object_prototype_to_string(vm, args, nargs, retval);
}


static njs_ret_t
njs_array_prototype_to_string_continuation(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t retval)
{
    /* Skip retval update. */
    vm->frame->skip = 1;

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_join(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    uint32_t          max;
    nxt_uint_t        i, n;
    njs_array_t       *array;
    njs_value_t       *value, *values;
    njs_array_join_t  *join;

    if (!njs_is_array(&args[0])) {
        goto empty;
    }

    array = args[0].data.u.array;

    if (array->length == 0) {
        goto empty;
    }

    join = (njs_array_join_t *) njs_continuation(vm->frame);
    join->values = NULL;
    join->max = 0;
    max = 0;

    for (i = 0; i < array->length; i++) {
        value = &array->start[i];
        if (!njs_is_string(value)
            && njs_is_valid(value)
            && !njs_is_null_or_void(value))
        {
            max++;
        }
    }

    if (max != 0) {
        values = nxt_mem_cache_align(vm->mem_cache_pool, sizeof(njs_value_t),
                                     sizeof(njs_value_t) * max);
        if (nxt_slow_path(values == NULL)) {
            return NXT_ERROR;
        }

        join = (njs_array_join_t *) njs_continuation(vm->frame);
        join->cont.function = njs_array_prototype_join_continuation;
        join->values = values;
        join->max = max;

        n = 0;

        for (i = 0; i < array->length; i++) {
            value = &array->start[i];
            if (!njs_is_string(value)
                && njs_is_valid(value)
                && !njs_is_null_or_void(value))
            {
                values[n++] = *value;

                if (n >= max) {
                    break;
                }
            }
        }
    }

    return njs_array_prototype_join_continuation(vm, args, nargs, unused);

empty:

    vm->retval = njs_string_empty;

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_join_continuation(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    u_char             *p;
    size_t             size, length, mask;
    uint32_t           max;
    nxt_uint_t         i, n;
    njs_array_t        *array;
    njs_value_t        *value, *values;
    njs_array_join_t   *join;
    njs_string_prop_t  separator, string;

    join = (njs_array_join_t *) njs_continuation(vm->frame);
    values = join->values;
    max = join->max;

    size = 0;
    length = 0;
    n = 0;
    mask = -1;

    array = args[0].data.u.array;

    for (i = 0; i < array->length; i++) {
        value = &array->start[i];

        if (njs_is_valid(value) && !njs_is_null_or_void(value)) {

            if (!njs_is_string(value)) {
                value = &values[n++];

                if (!njs_is_string(value)) {
                    vm->frame->trap_scratch.data.u.value = value;

                    return NJS_TRAP_STRING_ARG;
                }
            }

            (void) njs_string_prop(&string, value);

            size += string.size;
            length += string.length;

            if (string.length == 0 && string.size != 0) {
                mask = 0;
            }
        }
    }

    if (nargs > 1) {
        value = &args[1];

    } else {
        value = (njs_value_t *) &njs_string_comma;
    }

    (void) njs_string_prop(&separator, value);

    size += separator.size * (array->length - 1);
    length += separator.length * (array->length - 1);

    length &= mask;

    p = njs_string_alloc(vm, &vm->retval, size, length);
    if (nxt_slow_path(p == NULL)) {
        return NXT_ERROR;
    }

    n = 0;

    for (i = 0; i < array->length; i++) {
        value = &array->start[i];

        if (njs_is_valid(value) && !njs_is_null_or_void(value)) {
            if (!njs_is_string(value)) {
                value = &values[n++];
            }

            (void) njs_string_prop(&string, value);

            p = memcpy(p, string.start, string.size);
            p += string.size;
        }

        if (i < array->length - 1) {
            p = memcpy(p, separator.start, separator.size);
            p += separator.size;
        }
    }

    for (i = 0; i < max; i++) {
        njs_release(vm, &values[i]);
    }

    nxt_mem_cache_free(vm->mem_cache_pool, values);

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_concat(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    size_t       length;
    nxt_uint_t   i;
    njs_value_t  *value;
    njs_array_t  *array;

    length = 0;

    for (i = 0; i < nargs; i++) {
        if (njs_is_array(&args[i])) {
            length += args[i].data.u.array->length;

        } else {
            length++;
        }
    }

    array = njs_array_alloc(vm, length, NJS_ARRAY_SPARE);
    if (nxt_slow_path(array == NULL)) {
        return NXT_ERROR;
    }

    vm->retval.data.u.array = array;
    vm->retval.type = NJS_ARRAY;
    vm->retval.data.truth = 1;

    value = array->start;

    for (i = 0; i < nargs; i++) {
        value = njs_array_copy(value, &args[i]);
    }

    return NXT_OK;
}


static njs_value_t *
njs_array_copy(njs_value_t *dst, njs_value_t *src)
{
    nxt_uint_t  n;

    n = 1;

    if (njs_is_array(src)) {
        n = src->data.u.array->length;
        src = src->data.u.array->start;
    }

    while (n != 0) {
        /* GC: njs_retain src */
        *dst++ = *src++;
        n--;
    }

    return dst;
}


static njs_ret_t
njs_array_prototype_index_of(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    return njs_array_index_of(vm, args, nargs, 1);
}


static njs_ret_t
njs_array_prototype_last_index_of(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    return njs_array_index_of(vm, args, nargs, 0);
}


static njs_ret_t
njs_array_index_of(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    nxt_bool_t first)
{
    nxt_int_t    i, index, length;
    njs_value_t  *value;
    njs_array_t  *array;

    index = -1;

    if (nargs > 1) {
        i = 0;
        array = args[0].data.u.array;
        length = array->length;

        if (nargs > 2) {
            i = args[2].data.u.number;

            if (i < 0) {
                i += length;

                if (i < 0) {
                    i = 0;
                }
            }
        }

        value = &args[1];

        while (i < length) {
            if (njs_values_strict_equal(value, &array->start[i])) {
                index = i;

                if (first) {
                    break;
                }
            }

            i++;
        }
    }

    njs_number_set(&vm->retval, index);

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_for_each(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    nxt_int_t         ret;
    njs_array_iter_t  *iter;

    ret = njs_array_iterator_args(vm, args, nargs);
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    iter = njs_continuation(vm->frame);
    iter->u.cont.function = njs_array_prototype_for_each_continuation;

    return njs_array_prototype_for_each_continuation(vm, args, nargs, unused);
}


static njs_ret_t
njs_array_prototype_for_each_continuation(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    njs_array_iter_t  *iter;

    iter = njs_continuation(vm->frame);

    if (iter->next_index >= args[0].data.u.array->length) {
        vm->retval = njs_value_void;
        return NXT_OK;
    }

    return njs_array_iterator_apply(vm, iter, args, nargs);
}


static njs_ret_t
njs_array_prototype_some(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    nxt_int_t         ret;
    njs_array_iter_t  *iter;

    ret = njs_array_iterator_args(vm, args, nargs);
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    iter = njs_continuation(vm->frame);
    iter->u.cont.function = njs_array_prototype_some_continuation;
    iter->retval.data.truth = 0;

    return njs_array_prototype_some_continuation(vm, args, nargs, unused);
}


static njs_ret_t
njs_array_prototype_some_continuation(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    njs_array_iter_t   *iter;
    const njs_value_t  *retval;

    iter = njs_continuation(vm->frame);

    if (njs_is_true(&iter->retval)) {
        retval = &njs_value_true;

    } else if (iter->next_index >= args[0].data.u.array->length) {
        retval = &njs_value_false;

    } else {
        return njs_array_iterator_apply(vm, iter, args, nargs);
    }

    vm->retval = *retval;

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_every(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    nxt_int_t         ret;
    njs_array_iter_t  *iter;

    ret = njs_array_iterator_args(vm, args, nargs);
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    iter = njs_continuation(vm->frame);
    iter->u.cont.function = njs_array_prototype_every_continuation;
    iter->retval.data.truth = 1;

    return njs_array_prototype_every_continuation(vm, args, nargs, unused);
}


static njs_ret_t
njs_array_prototype_every_continuation(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    njs_array_iter_t   *iter;
    const njs_value_t  *retval;

    iter = njs_continuation(vm->frame);

    if (!njs_is_true(&iter->retval)) {
        retval = &njs_value_false;

    } else if (iter->next_index >= args[0].data.u.array->length) {
        retval = &njs_value_true;

    } else {
        return njs_array_iterator_apply(vm, iter, args, nargs);
    }

    vm->retval = *retval;

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_filter(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    nxt_int_t           ret;
    njs_array_filter_t  *filter;

    ret = njs_array_iterator_args(vm, args, nargs);
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    filter = njs_continuation(vm->frame);
    filter->iter.u.cont.function = njs_array_prototype_filter_continuation;
    filter->iter.retval.data.truth = 0;

    filter->array = njs_array_alloc(vm, 0, NJS_ARRAY_SPARE);
    if (nxt_slow_path(filter->array == NULL)) {
        return NXT_ERROR;
    }

    return njs_array_prototype_filter_continuation(vm, args, nargs, unused);
}


static njs_ret_t
njs_array_prototype_filter_continuation(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    nxt_int_t           ret;
    njs_array_t         *array;
    njs_array_filter_t  *filter;

    filter = njs_continuation(vm->frame);

    if (njs_is_true(&filter->iter.retval)) {
        ret = njs_array_add(vm, filter->array, &filter->value);
        if (nxt_slow_path(ret != NXT_OK)) {
            return ret;
        }
    }

    array = args[0].data.u.array;

    if (filter->iter.next_index >= array->length) {
        vm->retval.data.u.array = filter->array;
        vm->retval.type = NJS_ARRAY;
        vm->retval.data.truth = 1;

        return NXT_OK;
    }

    /* GC: filter->value */
    filter->value = array->start[filter->iter.next_index];

    return njs_array_iterator_apply(vm, &filter->iter, args, nargs);
}


static njs_ret_t
njs_array_prototype_map(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    size_t           size;
    nxt_int_t        ret;
    njs_value_t      *value;
    njs_array_t      *array;
    njs_array_map_t  *map;

    ret = njs_array_iterator_args(vm, args, nargs);
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    map = njs_continuation(vm->frame);
    map->iter.u.cont.function = njs_array_prototype_map_continuation;
    njs_set_invalid(&map->iter.retval);

    array = args[0].data.u.array;

    map->array = njs_array_alloc(vm, array->length, 0);
    if (nxt_slow_path(map->array == NULL)) {
        return NXT_ERROR;
    }

    value = map->array->start;
    size = array->length;

    while (size != 0) {
        njs_set_invalid(value);
        value++;
        size--;
    }

    return njs_array_prototype_map_continuation(vm, args, nargs, unused);
}


static njs_ret_t
njs_array_prototype_map_continuation(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    njs_array_map_t  *map;

    map = njs_continuation(vm->frame);

    if (njs_is_valid(&map->iter.retval)) {
        map->array->start[map->index] = map->iter.retval;
    }

    if (map->iter.next_index >= args[0].data.u.array->length) {
        vm->retval.data.u.array = map->array;
        vm->retval.type = NJS_ARRAY;
        vm->retval.data.truth = 1;

        return NXT_OK;
    }

    map->index = map->iter.next_index;

    return njs_array_iterator_apply(vm, &map->iter, args, nargs);
}


static njs_ret_t
njs_array_prototype_reduce(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs,
    njs_index_t unused)
{
    uint32_t          n;
    nxt_int_t         ret;
    njs_array_t       *array;
    njs_array_iter_t  *iter;

    ret = njs_array_iterator_args(vm, args, nargs);
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    iter = njs_continuation(vm->frame);
    iter->u.cont.function = njs_array_prototype_reduce_continuation;

    if (nargs > 2) {
        iter->retval = args[2];

    } else {
        array = args[0].data.u.array;
        n = iter->next_index;

        if (n >= array->length) {
            vm->exception = &njs_exception_type_error;
            return NXT_ERROR;
        }

        iter->retval = array->start[n];

        iter->next_index = njs_array_iterator_next(array, n + 1, array->length);
    }

    return njs_array_prototype_reduce_continuation(vm, args, nargs, unused);
}


static njs_ret_t
njs_array_prototype_reduce_continuation(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    nxt_int_t         n;
    njs_array_t       *array;
    njs_value_t       arguments[5];
    njs_array_iter_t  *iter;

    iter = njs_continuation(vm->frame);

    if (iter->next_index >= args[0].data.u.array->length) {
        vm->retval = iter->retval;
        return NXT_OK;
    }

    arguments[0] = njs_value_void;

    /* GC: array elt, array */
    arguments[1] = iter->retval;

    array = args[0].data.u.array;
    n = iter->next_index;

    arguments[2] = array->start[n];

    njs_number_set(&arguments[3], n);

    arguments[4] = args[0];

    iter->next_index = njs_array_iterator_next(array, n + 1, iter->length);

    return njs_function_apply(vm, args[1].data.u.function, arguments, 5,
                              (njs_index_t) &iter->retval);
}


static nxt_noinline njs_ret_t
njs_array_iterator_args(njs_vm_t *vm, njs_value_t *args, nxt_uint_t nargs)
{
    njs_array_t       *array;
    njs_array_iter_t  *iter;

    if (nargs > 1 && njs_is_array(&args[0]) && njs_is_function(&args[1])) {

        array = args[0].data.u.array;
        iter = njs_continuation(vm->frame);
        iter->length = array->length;
        iter->next_index = njs_array_iterator_next(array, 0, array->length);

        return NXT_OK;
    }

    vm->exception = &njs_exception_type_error;

    return NXT_ERROR;
}


static nxt_noinline uint32_t
njs_array_iterator_next(njs_array_t *array, uint32_t n, uint32_t length)
{
    length = nxt_min(length, array->length);

    while (n < length) {
        if (njs_is_valid(&array->start[n])) {
            return n;
        }

        n++;
    }

    return -1;
}


static nxt_noinline njs_ret_t
njs_array_iterator_apply(njs_vm_t *vm, njs_array_iter_t *iter,
    njs_value_t *args, nxt_uint_t nargs)
{
    nxt_int_t    n;
    njs_array_t  *array;
    njs_value_t  arguments[4];

    /*
     * The cast "*(njs_value_t *) &" is required by SunC.
     * Simple "(njs_value_t)" does not help.
     */
    arguments[0] = (nargs > 2) ? args[2] : *(njs_value_t *) &njs_value_void;
    /* GC: array elt, array */

    /*
     * All array iterators functions call njs_array_iterator_args()
     * function which set a correct iter->next_index value.  A large
     * value of iter->next_index must be checked before calling
     * njs_array_iterator_apply().
     */
    array = args[0].data.u.array;
    n = iter->next_index;
    arguments[1] = array->start[n];

    njs_number_set(&arguments[2], n);

    arguments[3] = args[0];

    iter->next_index = njs_array_iterator_next(array, n + 1, iter->length);

    return njs_function_apply(vm, args[1].data.u.function, arguments, 4,
                              (njs_index_t) &iter->retval);
}


static njs_ret_t
njs_array_prototype_reduce_right(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    int32_t           n;
    njs_array_t       *array;
    njs_array_iter_t  *iter;

    if (nargs < 2 || !njs_is_array(&args[0]) || !njs_is_function(&args[1])) {
        goto type_error;
    }

    iter = njs_continuation(vm->frame);
    iter->u.cont.function = njs_array_prototype_reduce_right_continuation;

    array = args[0].data.u.array;
    iter->next_index = njs_array_reduce_right_next(array, array->length);

    if (nargs > 2) {
        iter->retval = args[2];

    } else {
        n = iter->next_index;

        if (n < 0) {
            goto type_error;
        }

        iter->retval = array->start[n];

        iter->next_index = njs_array_reduce_right_next(array, n);
    }

    return njs_array_prototype_reduce_right_continuation(vm, args, nargs,
                                                         unused);
type_error:

    vm->exception = &njs_exception_type_error;

    return NXT_ERROR;
}


static njs_ret_t
njs_array_prototype_reduce_right_continuation(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    nxt_int_t         n;
    njs_array_t       *array;
    njs_value_t       arguments[5];
    njs_array_iter_t  *iter;

    iter = njs_continuation(vm->frame);

    if ((int32_t) iter->next_index < 0) {
        vm->retval = iter->retval;
        return NXT_OK;
    }

    arguments[0] = njs_value_void;

    /* GC: array elt, array */
    arguments[1] = iter->retval;

    array = args[0].data.u.array;
    n = iter->next_index;
    arguments[2] = array->start[n];

    njs_number_set(&arguments[3], n);

    arguments[4] = args[0];

    iter->next_index = njs_array_reduce_right_next(array, n);

    return njs_function_apply(vm, args[1].data.u.function, arguments, 5,
                              (njs_index_t) &iter->retval);
}


static nxt_noinline uint32_t
njs_array_reduce_right_next(njs_array_t *array, int32_t n)
{
    n = nxt_min(n, (int32_t) array->length) - 1;

    while (n >= 0) {
        if (njs_is_valid(&array->start[n])) {
            return n;
        }

        n--;
    }

    return n;
}


static njs_ret_t
njs_array_string_sort(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    nxt_int_t   ret;
    nxt_uint_t  i;

    for (i = 1; i < nargs; i++) {
        if (!njs_is_string(&args[i])) {
            vm->frame->trap_scratch.data.u.value = &args[i];
            return NJS_TRAP_STRING_ARG;
        }
    }

    ret = njs_string_cmp(&args[1], &args[2]);

    njs_number_set(&vm->retval, ret);

    return NXT_OK;
}


static const njs_function_t  njs_array_string_sort_function = {
    .object.shared = 1,
    .native = 1,
    .continuation_size = NJS_CONTINUATION_SIZE,
    .args_types = { NJS_SKIP_ARG, NJS_STRING_ARG, NJS_STRING_ARG },
    .args_offset = 1,
    .u.native = njs_array_string_sort,
};


static njs_ret_t
njs_array_prototype_sort(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    njs_array_sort_t  *sort;

    if (njs_is_array(&args[0]) && args[0].data.u.array->length > 1) {

        sort = njs_continuation(vm->frame);
        sort->u.cont.function = njs_array_prototype_sort_continuation;
        sort->current = 0;
        sort->retval = njs_value_zero;

        if (nargs > 1 && njs_is_function(&args[1])) {
            sort->function = args[1].data.u.function;

        } else {
            sort->function = (njs_function_t *) &njs_array_string_sort_function;
        }

        return njs_array_prototype_sort_continuation(vm, args, nargs, unused);
    }

    vm->retval = args[0];

    return NXT_OK;
}


static njs_ret_t
njs_array_prototype_sort_continuation(njs_vm_t *vm, njs_value_t *args,
    nxt_uint_t nargs, njs_index_t unused)
{
    nxt_int_t         n;
    njs_array_t       *array;
    njs_value_t       value, *start, arguments[3];
    njs_array_sort_t  *sort;

    array = args[0].data.u.array;
    start = array->start;

    sort = njs_continuation(vm->frame);

    if (njs_is_number(&sort->retval)) {

        /*
         * The sort function is impelemented with the insertion sort algorithm.
         * Its worst and average computational complexity is O^2.  This point
         * should be considired as return point from comparison function so
         * "goto next" moves control to the appropriate step of the algorithm.
         * The first iteration also goes there because sort->retval is zero.
         */
        if (sort->retval.data.u.number <= 0) {
            goto next;
        }

        n = sort->index;

    swap:

        value = start[n];
        start[n] = start[n - 1];
        n--;
        start[n] = value;

        do {
            if (n > 0) {

                if (njs_is_valid(&start[n]) && njs_is_valid(&start[n - 1])) {
                    arguments[0] = njs_value_void;

                    /* GC: array elt, array */
                    arguments[1] = start[n - 1];
                    arguments[2] = start[n];

                    sort->index = n;

                    return njs_function_apply(vm, sort->function, arguments, 3,
                                              (njs_index_t) &sort->retval);
                }

                if (!njs_is_valid(&start[n - 1]) && njs_is_valid(&start[n])) {
                    /* Move invalid values to the end of array. */
                    goto swap;
                }
            }

        next:

            sort->current++;
            n = sort->current;

        } while (sort->current < array->length);
    }

    vm->retval = args[0];

    return NXT_OK;
}


static const njs_object_prop_t  njs_array_prototype_properties[] =
{
    {
        .type = NJS_NATIVE_GETTER,
        .name = njs_string("length"),
        .value = njs_native_getter(njs_array_prototype_length),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("slice"),
        .value = njs_native_function(njs_array_prototype_slice, 0,
                     NJS_OBJECT_ARG, NJS_INTEGER_ARG, NJS_INTEGER_ARG),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("push"),
        .value = njs_native_function(njs_array_prototype_push, 0, 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("pop"),
        .value = njs_native_function(njs_array_prototype_pop, 0, 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("unshift"),
        .value = njs_native_function(njs_array_prototype_unshift, 0, 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("shift"),
        .value = njs_native_function(njs_array_prototype_shift, 0, 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("splice"),
        .value = njs_native_function(njs_array_prototype_splice, 0,
                    NJS_OBJECT_ARG, NJS_INTEGER_ARG, NJS_INTEGER_ARG),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("reverse"),
        .value = njs_native_function(njs_array_prototype_reverse, 0,
                    NJS_OBJECT_ARG),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("toString"),
        .value = njs_native_function(njs_array_prototype_to_string,
                     NJS_CONTINUATION_SIZE, 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("join"),
        .value = njs_native_function(njs_array_prototype_join,
                     njs_continuation_size(njs_array_join_t),
                     NJS_OBJECT_ARG, NJS_STRING_ARG),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("concat"),
        .value = njs_native_function(njs_array_prototype_concat, 0, 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("indexOf"),
        .value = njs_native_function(njs_array_prototype_index_of, 0,
                     NJS_OBJECT_ARG, NJS_SKIP_ARG, NJS_INTEGER_ARG),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("lastIndexOf"),
        .value = njs_native_function(njs_array_prototype_last_index_of, 0,
                     NJS_OBJECT_ARG, NJS_SKIP_ARG, NJS_INTEGER_ARG),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("forEach"),
        .value = njs_native_function(njs_array_prototype_for_each,
                     njs_continuation_size(njs_array_iter_t), 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("some"),
        .value = njs_native_function(njs_array_prototype_some,
                     njs_continuation_size(njs_array_iter_t), 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("every"),
        .value = njs_native_function(njs_array_prototype_every,
                     njs_continuation_size(njs_array_iter_t), 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("filter"),
        .value = njs_native_function(njs_array_prototype_filter,
                     njs_continuation_size(njs_array_filter_t), 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("map"),
        .value = njs_native_function(njs_array_prototype_map,
                     njs_continuation_size(njs_array_map_t), 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("reduce"),
        .value = njs_native_function(njs_array_prototype_reduce,
                     njs_continuation_size(njs_array_iter_t), 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("reduceRight"),
        .value = njs_native_function(njs_array_prototype_reduce_right,
                     njs_continuation_size(njs_array_iter_t), 0),
    },

    {
        .type = NJS_METHOD,
        .name = njs_string("sort"),
        .value = njs_native_function(njs_array_prototype_sort,
                     njs_continuation_size(njs_array_iter_t), 0),
    },
};


const njs_object_init_t  njs_array_prototype_init = {
    njs_array_prototype_properties,
    nxt_nitems(njs_array_prototype_properties),
};
