
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_types.h>
#include <nxt_clang.h>
#include <nxt_alignment.h>
#include <nxt_string.h>
#include <nxt_stub.h>
#include <nxt_utf8.h>
#include <nxt_djb_hash.h>
#include <nxt_array.h>
#include <nxt_lvlhsh.h>
#include <nxt_random.h>
#include <nxt_mem_cache_pool.h>
#include <njscript.h>
#include <njs_vm.h>
#include <njs_variable.h>
#include <njs_parser.h>
#include <string.h>


static njs_variable_t *njs_variable_alloc(njs_vm_t *vm,
    njs_parser_t *parser, nxt_str_t *name);


static nxt_int_t
njs_variables_hash_test(nxt_lvlhsh_query_t *lhq, void *data)
{
    njs_variable_t  *var;

    var = data;

    if (lhq->key.length == var->name_len
        && memcmp(var->name_start, lhq->key.start, lhq->key.length) == 0)
    {
        return NXT_OK;
    }

    return NXT_DECLINED;
}


static const nxt_lvlhsh_proto_t  njs_variables_hash_proto
    nxt_aligned(64) =
{
    NXT_LVLHSH_DEFAULT,
    0,
    njs_variables_hash_test,
    njs_lvlhsh_alloc,
    njs_lvlhsh_free,
};


njs_variable_t *
njs_parser_name_alloc(njs_vm_t *vm, njs_parser_t *parser)
{
    nxt_int_t           ret;
    njs_variable_t      *var;
    nxt_lvlhsh_query_t  lhq;

    var = njs_variable_alloc(vm, parser, &parser->lexer->text);
    if (nxt_slow_path(var == NULL)) {
        return NULL;
    }

    lhq.key_hash = parser->lexer->key_hash;
    lhq.key = parser->lexer->text;
    lhq.replace = 0;
    lhq.value = var;
    lhq.proto = &njs_variables_hash_proto;
    lhq.pool = vm->mem_cache_pool;

    ret = nxt_lvlhsh_insert(&parser->variables_hash, &lhq);

    if (nxt_fast_path(ret == NXT_OK)) {
        return var;
    }

    nxt_alert(&vm->trace, NXT_LEVEL_ERROR,
              "SyntaxError: Duplicate declaration \"%.*s\"",
              (int) parser->lexer->text.length, parser->lexer->text.start);

    return NULL;
}


njs_variable_t *
njs_parser_variable(njs_vm_t *vm, njs_parser_t *parser, nxt_uint_t *level)
{
    nxt_int_t           ret;
    nxt_uint_t          n;
    njs_parser_t        *scope;
    njs_variable_t      *var;
    nxt_lvlhsh_query_t  lhq;

    *level = 0;

    lhq.key_hash = parser->lexer->key_hash;
    lhq.key = parser->lexer->text;
    lhq.proto = &njs_variables_hash_proto;

    scope = parser;

    do {
        var = scope->arguments->start;
        n = scope->arguments->items;

        while (n != 0) {
            if (lhq.key.length == var->name_len
                && memcmp(var->name_start, lhq.key.start, lhq.key.length) == 0)
            {
                return var;
            }

            var++;
            n--;
        }

        if (nxt_lvlhsh_find(&scope->variables_hash, &lhq) == NXT_OK) {
            return lhq.value;
        }

        scope = scope->parent;
        (*level)++;

    } while (scope != NULL);

    *level = 0;

    if (nxt_lvlhsh_find(&vm->variables_hash, &lhq) == NXT_OK) {
        return lhq.value;
    }

    var = njs_variable_alloc(vm, parser, &parser->lexer->text);
    if (nxt_slow_path(var == NULL)) {
        return NULL;
    }

    lhq.replace = 0;
    lhq.value = var;
    lhq.pool = vm->mem_cache_pool;

    ret = nxt_lvlhsh_insert(&parser->variables_hash, &lhq);

    if (nxt_fast_path(ret == NXT_OK)) {
        return var;
    }

    return NULL;
}


njs_function_t *
njs_vm_function(njs_vm_t *vm, nxt_str_t *name)
{
    njs_value_t         *value;
    njs_variable_t      *var;
    nxt_lvlhsh_query_t  lhq;

    lhq.key_hash = nxt_djb_hash(name->start, name->length);
    lhq.key = *name;
    lhq.proto = &njs_variables_hash_proto;

    if (nxt_slow_path(nxt_lvlhsh_find(&vm->variables_hash, &lhq) != NXT_OK)) {
        return NULL;
    }

    var = lhq.value;

    value = (njs_value_t *) ((u_char *) vm->global_scope
                           + njs_offset(var->index) - NJS_INDEX_GLOBAL_OFFSET);

    if (njs_is_function(value)) {
        return value->data.u.function;
    }

    return NULL;
}


static njs_variable_t *
njs_variable_alloc(njs_vm_t *vm, njs_parser_t *parser, nxt_str_t *name)
{
    njs_value_t     *value;
    njs_variable_t  *var;

    var = nxt_mem_cache_zalloc(vm->mem_cache_pool, sizeof(njs_variable_t));

    if (nxt_fast_path(var != NULL)) {
        var->name_start = nxt_mem_cache_alloc(vm->mem_cache_pool, name->length);

        if (nxt_fast_path(var->name_start != NULL)) {

            memcpy(var->name_start, name->start, name->length);
            var->name_len = name->length;

            value = nxt_array_add(parser->scope_values, &njs_array_mem_proto,
                                  vm->mem_cache_pool);
            if (nxt_fast_path(value != NULL)) {
                 *value = njs_value_void;
                 var->index = njs_parser_index(parser, parser->scope);
                 return var;
            }
        }
    }

    return NULL;
}


njs_value_t *
njs_variable_value(njs_parser_t *parser, njs_index_t index)
{
    u_char  *scope;

    scope = parser->scope_values->start;

    return (njs_value_t *) (scope + (njs_offset(index) - parser->scope_offset));
}
