/*
  allocation and garbage collection
  . non-moving, precise mark and sweep collector
  . pool-allocates small objects, keeps big objects on a simple list
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#ifdef BOEHM_GC
#include <gc.h>
#endif
#include "llt.h"
#include "julia.h"

/*
  integration steps:
  - convert idtable to use an Array
  - give GC access to relevant C local variables
  - allocate ios objects with malloc, use finalizer
*/

#define GC_PAGE_SZ 16384//bytes

typedef struct _page_t {
    struct _page_t *next;
    char data[GC_PAGE_SZ - sizeof(void*)];
} page_t;

typedef struct _gcval_t {
    union {
        struct _gcval_t *next;
        uptrint_t flags;
        struct {
            uptrint_t marked:1;
            uptrint_t finalize:1;
            //uptrint_t typed:1;
#ifdef BITS64
            uptrint_t otherbits:62;
#else
            uptrint_t otherbits:30;
#endif
        };
    };
    char _data[1];
} gcval_t;

typedef struct _pool_t {
    size_t osize;
    page_t *pages;
    gcval_t *freelist;
} pool_t;

typedef struct _bigval_t {
    struct _bigval_t *next;
    union {
        uptrint_t flags;
        struct {
            uptrint_t marked:1;
            uptrint_t finalize:1;
            //uptrint_t typed:1;
#ifdef BITS64
            uptrint_t otherbits:62;
#else
            uptrint_t otherbits:30;
#endif
        };
    };
    char _data[1];
} bigval_t;

typedef struct _jl_gcframe_t {
    jl_value_t **roots;
    size_t nroots;
    struct _jl_gcframe_t *prev;
} jl_gcframe_t;

#define gc_val(o)     ((gcval_t*)(((void**)(o))-1))
#define gc_marked(o)  (gc_val(o)->marked)
#define gc_setmark(o) (gc_val(o)->marked=1)
#define gc_unmark(o)  (gc_val(o)->marked=0)

#define gcv_isfree(v)  ((v)->otherbits!=0)

static bigval_t *big_objects = NULL;

#define N_POOLS 16
static pool_t pools[N_POOLS];

static size_t allocd_bytes = 0;
static size_t collect_interval = 8192*1024;

// size classes:
// <=8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048
//   0   1   2   3   4   5   6    7    8    9   10   11   12    13    14    15

static int szclass(size_t sz)
{
    if (sz <= 8) return 0;
    if (sz <= 128) {
        if (sz <= 16) return 1;
        if (sz <= 32) {
            if (sz <= 24) return 2;
            return 3;
        }
        if (sz <= 64) {
            if (sz <= 48) return 4;
            return 5;
        }
        if (sz <= 96) return 6;
        return 7;
    }
    if (sz <= 512) {
        if (sz <= 256) {
            if (sz <= 192) return 8;
            return 9;
        }
        if (sz <= 384) return 10;
        return 11;
    }
    if (sz <= 1024) {
        if (sz <= 768) return 12;
        return 13;
    }
    if (sz <= 1536) return 14;
    return 15;
}

static void *alloc_big(size_t sz)
{
    bigval_t *v = (bigval_t*)malloc(sz + 2*sizeof(void*));
    v->next = big_objects;
    v->flags = 0;
    big_objects = v;
    return &v->_data[0];
}

static void sweep_big()
{
    bigval_t *v = big_objects;
    bigval_t **pv = &big_objects;
    while (v != NULL) {
        bigval_t *nxt = v->next;
        if (v->marked) {
            pv = &v->next;
            v->marked = 0;
        }
        else {
            *pv = nxt;
            // todo: move to to-free list, free after finalization
            free(v);
        }
        v = nxt;
    }
}

static void add_page(pool_t *p)
{
    page_t *pg = malloc(sizeof(page_t));
    gcval_t *v = (gcval_t*)&pg->data[0];
    char *lim = (char*)pg + GC_PAGE_SZ - p->osize;
    gcval_t *oldfl = p->freelist;
    gcval_t **pfl = &p->freelist;
    while ((char*)v <= lim) {
        *pfl = v;
        pfl = &v->next;
        v = (gcval_t*)((char*)v + p->osize);
    }
    *pfl = oldfl;
    pg->next = p->pages;
    p->pages = pg->next;
}

static void *pool_alloc(pool_t *p)
{
    if (p->freelist == NULL)
        add_page(p);
    assert(p->freelist != NULL);
    gcval_t *v = p->freelist;
    p->freelist = p->freelist->next;
    v->flags = 0;
    return &v->_data[0];
}

static void sweep_pool(pool_t *p)
{
    int empty;
    gcval_t **prev_pfl;
    gcval_t *v;
    page_t *pg = p->pages;
    page_t **ppg = &p->pages;
    gcval_t **pfl = &p->freelist;

    while (pg != NULL) {
        char *lim = (char*)pg + GC_PAGE_SZ - p->osize;
        v = (gcval_t*)&pg->data[0];
        empty = 1;
        prev_pfl = pfl;
        while ((char*)v <= lim) {
            if (!gcv_isfree(v))
                empty = 0;
            if (gcv_isfree(v) || !v->marked) {
                *pfl = v;
                pfl = &v->next;
            }
            else {
                v->marked = 0;
            }
            v = (gcval_t*)((char*)v + p->osize);
        }
        page_t *nextpg = pg->next;
        // if the whole page was already unused, free it
        if (empty) {
            pfl = prev_pfl;
            *ppg = nextpg;
            free(pg);
        }
        else {
            ppg = &pg->next;
        }
        pg = nextpg;
    }
    *pfl = NULL;
}

static void gc_sweep()
{
    sweep_big();
    int i;
    for(i=0; i < N_POOLS; i++)
        sweep_pool(&pools[i]);
}

#define GC_Markval(v) gc_markval((jl_value_t*)(v))
static void gc_markval(jl_value_t *v);

static void gc_mark_stack(jl_gcframe_t *s)
{
    while (s != NULL) {
        size_t i;
        for(i=0; i < s->nroots; i++)
            GC_Markval(s->roots[i]);
        s = s->prev;
    }
}

static void gc_mark_methlist(jl_methlist_t *ml)
{
    while (ml != NULL) {
        gc_setmark(ml);
        GC_Markval(ml->sig);
        GC_Markval(ml->tvars);
        GC_Markval(ml->func);
        ml = ml->next;
    }
}

static void gc_markval(jl_value_t *v)
{
    assert(v != NULL);
    if (gc_marked(v)) return;
    gc_setmark(v);

    if (jl_is_bits_type(jl_typeof(v))) return;

    // some values have special representations
    if (jl_is_array(v)) {
        jl_array_t *a = (jl_array_t*)v;
        GC_Markval(a->dims);
        if (a->data != &a->_space[0])
            gc_setmark(a->data);
        jl_value_t *elty = jl_tparam0(jl_typeof(v));
        if (!jl_is_bits_type(elty)) {
            size_t i;
            for(i=0; i < a->length; i++) {
                jl_value_t *elt = ((jl_value_t**)a->data)[i];
                if (elt != NULL) GC_Markval(elt);
            }
        }
    }
    else if (jl_is_tuple(v)) {
        size_t i;
        for(i=0; i < ((jl_tuple_t*)v)->length; i++) {
            jl_value_t *elt = ((jl_tuple_t*)v)->data[i];
            if (elt != NULL)
                GC_Markval(elt);
        }
    }
    else if (jl_is_lambda_info(v)) {
        jl_lambda_info_t *li = (jl_lambda_info_t*)v;
        GC_Markval(li->ast);
        GC_Markval(li->sparams);
        GC_Markval(li->tfunc);
        GC_Markval(li->roots);
        GC_Markval(li->specTypes);
        if (li->unspecialized != NULL)
            GC_Markval(li->unspecialized);
    }
    else if (jl_is_func(v)) {
        jl_function_t *f = (jl_function_t*)v;
        if (f->env  !=NULL) GC_Markval(f->env);
        if (f->linfo!=NULL) GC_Markval(f->linfo);
    }
    else if (jl_is_typename(v)) {
        jl_typename_t *tn = (jl_typename_t*)v;
        if (tn->primary != NULL)
            GC_Markval(tn->primary);
    }
    else if (jl_is_tag_type(v)) {
        jl_tag_type_t *tt = (jl_tag_type_t*)v;
        assert(tt->env == NULL);
        assert(tt->linfo == NULL);
        //if (tt->env  !=NULL) GC_Markval(tt->env);
        //if (tt->linfo!=NULL) GC_Markval(tt->linfo);
        GC_Markval(tt->name);
        GC_Markval(tt->super);
        GC_Markval(tt->parameters);
    }
    else if (jl_is_struct_type(v)) {
        jl_struct_type_t *st = (jl_struct_type_t*)v;
        if (st->env  !=NULL) GC_Markval(st->env);
        if (st->linfo!=NULL) GC_Markval(st->linfo);
        GC_Markval(st->name);
        GC_Markval(st->super);
        GC_Markval(st->parameters);
        GC_Markval(st->names);
        GC_Markval(st->types);
        if (st->ctor_factory != NULL)
            GC_Markval(st->ctor_factory);
        if (st->instance != NULL)
            GC_Markval(st->instance);
    }
    else if (jl_is_bits_type(v)) {
        jl_bits_type_t *bt = (jl_bits_type_t*)v;
        assert(bt->env == NULL);
        assert(bt->linfo == NULL);
        //if (bt->env  !=NULL) GC_Markval(bt->env);
        //if (bt->linfo!=NULL) GC_Markval(bt->linfo);
        GC_Markval(bt->name);
        GC_Markval(bt->super);
        GC_Markval(bt->parameters);
        GC_Markval(bt->bnbits);
    }
    else if (jl_is_mtable(v)) {
        jl_methtable_t *mt = (jl_methtable_t*)v;
        size_t i;
        gc_mark_methlist(mt->defs);
        gc_mark_methlist(mt->cache);
        for(i=0; i < mt->n_1arg; i++) {
            if (mt->cache_1arg[i] != NULL)
                GC_Markval(mt->cache_1arg[i]);
        }
    }
    else if (jl_is_task(v)) {
        jl_task_t *ta = (jl_task_t*)v;
        GC_Markval(ta->on_exit);
        GC_Markval(ta->start);
        GC_Markval(ta->result);
        // TODO
        //gc_mark_stack(ta->gc_frames);
        GC_Markval(ta->state.eh_task);
        // TODO
        // GC_Markval(ta->state.current_output_stream);
    }
    else {
        assert(jl_is_struct_type(jl_typeof(v)));
        size_t nf = ((jl_struct_type_t*)jl_typeof(v))->names->length;
        size_t i;
        for(i=0; i < nf; i++)
            GC_Markval(((jl_value_t**)v)[i+1]);
    }
}

static void gc_mark_module(jl_module_t *m)
{
    size_t i;
    void **table = m->bindings.table;
    gc_setmark(m);
    for(i=1; i < m->bindings.size; i+=2) {
        if (table[i] != HT_NOTFOUND) {
            jl_binding_t *b = (jl_binding_t*)table[i];
            gc_setmark(b);
            if (b->value != NULL)
                GC_Markval(b->value);
            GC_Markval(b->type);
        }
    }
}

void jl_mark_type_cache();

static void gc_mark()
{
    // mark all roots

    // active tasks
    GC_Markval(jl_root_task);
    GC_Markval(jl_current_task);

    // modules
    gc_mark_module(jl_system_module);
    gc_mark_module(jl_user_module);

    // invisible builtin values
    GC_Markval(jl_methtable_type);
    GC_Markval(jl_bottom_func);
    GC_Markval(jl_any_func);

    // constants
    GC_Markval(jl_null);
    GC_Markval(jl_true);
    GC_Markval(jl_false);

    // types
    jl_mark_type_cache();
}

void gc_collect()
{
    gc_mark();
    gc_sweep();
    allocd_bytes = 0;
}

void *allocb(size_t sz)
{
    if (allocd_bytes > collect_interval)
        gc_collect();
    allocd_bytes += sz;
    if (sz > 2048)
        return alloc_big(sz);
    return pool_alloc(&pools[szclass(sz)]);
}

void jl_gc_init()
{
    int szc[N_POOLS] = { 8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512,
                         768, 1024, 1536, 2048 };
    int i;
    for(i=0; i < N_POOLS; i++) {
        pools[i].osize = szc[i]+sizeof(void*);
        pools[i].pages = NULL;
        pools[i].freelist = NULL;
    }
}