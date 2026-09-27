/* Hosted（libc）構成では POSIX/GNU 拡張の宣言が必要なので feature macro を
 * 定義する。freestanding（PYTHON_CODE_TO_C_NO_STDLIB）では定義しない:
 * 単一ヘッダーを ISO C11 のまま保ち、取り込み先の OS へ POSIX/GNU 拡張を
 * 持ち込まないため。 */
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
/* 機能マクロが要求水準を満たしているかを確認する（-Wunused-macros への対処と、
 * 古い環境で暗黙に別の宣言へ落ちることを防ぐ役割を兼ねる）。 */
#if _POSIX_C_SOURCE < 200112L
#error "python_code_to_c requires POSIX.1-2001 or later (strtok_r, strdup, snprintf)"
#endif
#endif
#include "runtime/python_code_to_c_runtime.h"
#include "modules/python_code_to_c_pygame.h"
#include <stddef.h>
#ifndef INT64_MAX
#define INT64_MAX ((int64_t)((uint64_t)-1 >> 1))
#endif
#ifndef INT64_MIN
#define INT64_MIN (-INT64_MAX - 1)
#endif
#if defined(__linux__) && !defined(PYTHON_CODE_TO_C_NO_STDLIB)
#  include <pthread.h>
#endif

#ifdef PYTHON_CODE_TO_C_NO_STDLIB
/* ============================================================
 * 組込み向けフォールバックlibc（線形/バンプアロケータ）
 * ============================================================
 * 既定ではここで malloc/free/realloc/calloc と setjmp/longjmp を提供する。
 * PYTHON_CODE_TO_C_NO_LIBC_STUBS を定義すると、これらは一切定義されず
 * カーネル実装（runtime.h が宣言するプロトタイプ）へリンクされる。
 * その場合 p2c_runtime_init() のヒープ引数は無視される。
 *
 * malloc 系は共有ヒープ層（p2c_heap_*）の薄い別名である。カーネルが
 * p2c_platform_set_allocator() でヒープを注入していれば、raw な malloc も
 * 変換器・ランタイム内部の確保も「1つのヒープ」を共有する（別ヒープを作らない）。
 * 未注入なら、下の線形ヒープ（p2c_runtime_init() が受け取った領域）が
 * 唯一のヒープになる。 */
#ifndef PYTHON_CODE_TO_C_NO_LIBC_STUBS
static char *heap_start = NULL;
static size_t heap_size = 0;
static size_t heap_used = 0;
static size_t heap_peak = 0;
static size_t heap_failures = 0;

/* 提供元の生確保: 線形（バンプ）ヒープ。
 * 共有ヒープ層（p2c_heap_*）のフォールバックと p2c_heap_usable() の判定から
 * だけ呼ばれる。ブロックには共通ヘッダ（P2C_HeapBlockHeader）を前置するため、
 * 確保元を問わずサイズとマジックが分かり、realloc が旧ブロックの外を読むことが
 * ない。 */
void *p2c_heap_alloc_raw(size_t size) {
    if (size == 0) size = 1;
    size_t payload = P2C_ALIGN_UP8(size);
    /* 巨大要求で heap_used + total がラップしないよう、差分比較で判定する。 */
    if (payload > (size_t)-1 - sizeof(P2C_HeapBlockHeader)) { heap_failures++; return NULL; }
    size_t total = payload + sizeof(P2C_HeapBlockHeader);
    if (!heap_start || heap_used > heap_size || total > heap_size - heap_used) {
        heap_failures++;
        return NULL;
    }
    /* 埋め込み用リニアヒープのブロックはヘッダ長がアライン倍数なので整列している。
     * char* からの直接キャストは -Wcast-align=strict を誤検出させるため void* を経由する。 */
    P2C_HeapBlockHeader *hdr = (P2C_HeapBlockHeader*)(void*)(heap_start + heap_used);
    heap_used += total;
    if (heap_used > heap_peak) heap_peak = heap_used;
    hdr->size  = size;
    hdr->magic = P2C_HEAP_BLOCK_MAGIC_LINEAR;
    return (void*)(hdr + 1);
}

void *p2c_heap_realloc_raw(void *ptr, size_t new_size) {
    if (!ptr) return p2c_heap_alloc_raw(new_size);
    if (new_size == 0) new_size = 1;
    /* 線形ヒープのブロックはヘッダに旧サイズが記録されているため、旧ブロックの
     * 長さぶんだけコピーする（以前は要求サイズぶんコピーしていたため、
     * 縮小→拡大の並びで旧ブロックの外を読む可能性があった）。 */
    P2C_HeapBlockHeader *hdr = ((P2C_HeapBlockHeader*)ptr) - 1;
    size_t old_size = (hdr->magic == P2C_HEAP_BLOCK_MAGIC_LINEAR) ? hdr->size : new_size;
    void *moved = p2c_heap_alloc_raw(new_size);
    if (!moved) return NULL;
    memcpy(moved, ptr, old_size < new_size ? old_size : new_size);
    return moved;
}

/* 線形ヒープは個別解放しない（p2c_runtime_shutdown() が領域ごと再利用する）。 */
void p2c_heap_free_raw(void *ptr) { (void)ptr; }

/* ランタイム同梱 libc スタブの malloc 系は共有ヒープ層の別名。
 * カーネルアロケータを注入した構成ではそちらへ委譲され、raw malloc と
 * ランタイム/変換器の内部確保が同じヒープを指す。size==0 は1バイト確保として
 * 扱い、必ず一意な非NULLポインタを返す（空コンテナ確保がOOMと誤認されないため）。 */
void* malloc(size_t size) { return p2c_heap_alloc(size); }
void* calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) { heap_failures++; return NULL; } /* オーバーフロー検出 */
    return p2c_heap_calloc(nmemb, size);
}
void* realloc(void *ptr, size_t size) { return p2c_heap_realloc(ptr, size); }
void free(void *ptr) { p2c_heap_free(ptr); }
#if !defined(P2C_HAVE_COMPILER_SETJMP)
/* コンパイラ組み込み (__builtin_setjmp/__builtin_longjmp) が無い環境では、
 * 本物の longjmp を自前で用意できない。以前は longjmp が無限ループで
 * 「黙って固まる」実装だったため、try/except を含む生成コードが
 * ベアメタルでハングしていた。ここでは診断を出して停止するに留め、
 * カーネルには次のどれかを求める:
 *   - PYTHON_CODE_TO_C_NO_LIBC_STUBS を定義して自前の setjmp/longjmp を提供する
 *     (examples/embed/x86_64_setjmp.c が参考実装)
 *   - PYTHON_CODE_TO_C_NO_COMPILER_SETJMP を定義しつつ、同じく実装をリンクする */
int setjmp(jmp_buf env) { (void)env; return 0; }
void longjmp(jmp_buf env, int val) {
    (void)env;
    (void)val;
    p2c_platform_abort("Python Code to C: this platform must provide setjmp/longjmp "
                       "(define PYTHON_CODE_TO_C_NO_LIBC_STUBS or use a compiler with "
                       "__builtin_setjmp)");
}
#endif /* !P2C_HAVE_COMPILER_SETJMP */
#endif /* !PYTHON_CODE_TO_C_NO_LIBC_STUBS */
#else
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#endif

static void free_none(P2C_Object *obj) { (void)obj; }
static P2C_Object* str_none(P2C_Object *obj) { (void)obj; return NULL; }
static P2C_MethodDef no_methods[] = { {NULL, NULL, NULL} };

/* GC: 他のP2C_Object*を直接保持しうる型（コンテナ・インスタンス・クラス・
 * モジュール）は、自分が指している子オブジェクトそれぞれについて
 * visit(child, ctx) を呼び出すgc_traverseを実装する。int/str/exception等の
 * 「葉」の型は他のP2C_Object*を保持しないためgc_traverse=NULLでよい
 * （下のP2C_ClassDefテーブルで明示的にNULLと書いている）。 */
static void gc_traverse_list(P2C_Object *obj, P2C_GcVisitFn visit, void *ctx) {
    for (size_t i = 0; i < obj->u.v_list.len; i++) visit(obj->u.v_list.items[i], ctx);
}
static void gc_traverse_tuple(P2C_Object *obj, P2C_GcVisitFn visit, void *ctx) {
    for (size_t i = 0; i < obj->u.v_tuple.len; i++) visit(obj->u.v_tuple.items[i], ctx);
}
static void gc_traverse_dict(P2C_Object *obj, P2C_GcVisitFn visit, void *ctx) {
    for (P2C_DictEntry *e = obj->u.v_dict.order_head; e; e = e->order_next) {
        visit(e->key, ctx);
        visit(e->val, ctx);
    }
}
static void gc_traverse_set(P2C_Object *obj, P2C_GcVisitFn visit, void *ctx) {
    for (P2C_DictEntry *e = obj->u.v_dict.order_head; e; e = e->order_next) visit(e->key, ctx);
}
/* インスタンス/クラス/モジュールの属性マップ(P2C_Map: キーはchar*、
 * 値はP2C_Object*)を辿る共通ヘルパー。 */
static void gc_traverse_attr_map(P2C_Map *map, P2C_GcVisitFn visit, void *ctx) {
    if (!map || !map->buckets) return;
    for (size_t i = 0; i < map->bucket_count; i++) {
        for (P2C_MapEntry *e = map->buckets[i]; e; e = e->next) visit((P2C_Object*)e->val, ctx);
    }
}
static void gc_traverse_instance(P2C_Object *obj, P2C_GcVisitFn visit, void *ctx) {
    visit(obj->u.v_instance.klass, ctx);
    gc_traverse_attr_map(obj->u.v_instance.attrs, visit, ctx);
}
static void gc_traverse_class(P2C_Object *obj, P2C_GcVisitFn visit, void *ctx) {
    gc_traverse_attr_map(obj->u.v_class.attrs, visit, ctx);
}
static void gc_traverse_module(P2C_Object *obj, P2C_GcVisitFn visit, void *ctx) {
    gc_traverse_attr_map(obj->u.v_module.attrs, visit, ctx);
}
static void gc_traverse_function(P2C_Object *obj, P2C_GcVisitFn visit, void *ctx) {
    visit(obj->u.v_function.env, ctx);
}
static void gc_traverse_cell(P2C_Object *obj, P2C_GcVisitFn visit, void *ctx) {
    visit(obj->u.v_cell.value, ctx);
}
static void gc_traverse_iterator(P2C_Object *obj, P2C_GcVisitFn visit, void *ctx) {
    visit(obj->u.v_iterator.seq, ctx);
    visit(obj->u.v_iterator.custom, ctx);
    visit(obj->u.v_iterator.locals, ctx);
    visit(obj->u.v_iterator.awaiting, ctx);
    visit(obj->u.v_iterator.result, ctx);
    visit(obj->u.v_iterator.exception, ctx);
}
static void gc_traverse_exception(P2C_Object *obj, P2C_GcVisitFn visit, void *ctx) {
    visit(obj->u.v_exception.cause, ctx);
}

P2C_ClassDef P2C_Class_NoneType   = {"NoneType", OBJ_NONE,     free_none, str_none, no_methods, NULL, NULL};
P2C_ClassDef P2C_Class_Bool       = {"bool",     OBJ_BOOL,     free_none, str_none, no_methods, NULL, NULL};
P2C_ClassDef P2C_Class_Int        = {"int",      OBJ_INT,      free_none, str_none, no_methods, NULL, NULL};
P2C_ClassDef P2C_Class_Float      = {"float",    OBJ_FLOAT,    free_none, str_none, no_methods, NULL, NULL};
P2C_ClassDef P2C_Class_Str        = {"str",      OBJ_STR,      free_none, str_none, no_methods, NULL, NULL};
P2C_ClassDef P2C_Class_List       = {"list",     OBJ_LIST,     free_none, str_none, no_methods, NULL, gc_traverse_list};
P2C_ClassDef P2C_Class_Dict       = {"dict",     OBJ_DICT,     free_none, str_none, no_methods, NULL, gc_traverse_dict};
P2C_ClassDef P2C_Class_Set        = {"set",      OBJ_SET,      free_none, str_none, no_methods, NULL, gc_traverse_set};
P2C_ClassDef P2C_Class_Tuple      = {"tuple",    OBJ_TUPLE,    free_none, str_none, no_methods, NULL, gc_traverse_tuple};
P2C_ClassDef P2C_Class_Function   = {"function", OBJ_FUNCTION, free_none, str_none, no_methods, NULL, gc_traverse_function};
P2C_ClassDef P2C_Class_ClassObject= {"class",    OBJ_CLASS,    free_none, str_none, no_methods, NULL, gc_traverse_class};
P2C_ClassDef P2C_Class_Instance   = {"instance", OBJ_INSTANCE, free_none, str_none, no_methods, NULL, gc_traverse_instance};
P2C_ClassDef P2C_Class_Module     = {"module",   OBJ_MODULE,   free_none, str_none, no_methods, NULL, gc_traverse_module};
P2C_ClassDef P2C_Class_Exception  = {"Exception",OBJ_EXCEPTION,free_none, str_none, no_methods, NULL, gc_traverse_exception};
P2C_ClassDef P2C_Class_Iterator   = {"iterator", OBJ_ITERATOR,  free_none, str_none, no_methods, NULL, gc_traverse_iterator};
P2C_ClassDef P2C_Class_Cell       = {"cell",     OBJ_CELL,      free_none, str_none, no_methods, NULL, gc_traverse_cell};
P2C_ClassDef P2C_Class_Ellipsis   = {"ellipsis", OBJ_ELLIPSIS,  free_none, str_none, no_methods, NULL, NULL};

P2C_Object P2C_None  = {&P2C_Class_NoneType, 1, {0}, NULL, 0};
P2C_Object P2C_True  = {&P2C_Class_Bool, 1, {.v_bool = true}, NULL, 0};
P2C_Object P2C_False = {&P2C_Class_Bool, 1, {.v_bool = false}, NULL, 0};
/* `...` はNone/True/Falseと同じ静的センチネル。GCの追跡対象外として扱う。 */
P2C_Object P2C_Ellipsis = {&P2C_Class_Ellipsis, 1, {0}, NULL, 0};
#define P2C_SMALL_INT_MIN (-5)
#define P2C_SMALL_INT_MAX 256
#define P2C_SMALL_INT_COUNT ((P2C_SMALL_INT_MAX - P2C_SMALL_INT_MIN) + 1)
static P2C_Object g_small_ints[P2C_SMALL_INT_COUNT];
static bool g_small_ints_ready = false;
static void p2c_small_ints_init(void) {
    if (g_small_ints_ready) return;
    for (size_t i = 0; i < P2C_SMALL_INT_COUNT; i++) {
        P2C_Object *o = &g_small_ints[i];
        o->cls = &P2C_Class_Int;
        o->refcount = 1;
        o->u.v_int = (int64_t)i + P2C_SMALL_INT_MIN;
        o->gc_next = NULL;
        o->gc_marked = 0;
    }
    g_small_ints_ready = true;
}
static P2C_Map *g_module_registry = NULL;

/* ============================================================
 * GC (ガベージコレクタ) グローバル状態
 * stop-the-world mark-and-sweep + 保守的 C スタックスキャン。
 *
 * オブジェクトの生存判定は「到達可能性」のみ。refcount は
 * ネイティブモジュール (pygame 等) が GC の外側 (C グローバル変数や
 * 自前の C ヒープ) に P2C_Object* を一時保持したいときのための
 * 「ピン留め」カウンタとして残す (incref/decref を使う)。
 * ============================================================ */
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <setjmp.h>
#endif

static P2C_Object  *g_gc_all         = NULL;
static P2C_Object **g_gc_roots[P2C_GC_ROOT_CAPACITY];
static size_t       g_gc_root_count  = 0;
static bool         g_gc_collecting  = false; /* GC再入防止 */
static void        *g_gc_stack_bottom = NULL; /* スタックスキャンの基点 (main の SF 内) */
static bool         g_gc_enabled     = true;
static size_t       g_gc_threshold   = 256 * 1024; /* 自動収集しきい値 (bytes) */
static size_t       g_gc_bytes_alloc = 0;          /* 前回収集後の累積確保 bytes */
static size_t       g_gc_collections = 0;
static size_t       g_gc_last_freed  = 0;
static size_t       g_gc_obj_count   = 0;
static bool         g_runtime_active = false;
static void        *g_gc_stack_lo    = NULL; /* OSが報告するスタック下限 (NULL可) */
static void        *g_gc_stack_hi    = NULL; /* OS/カーネルが報告するスタック上端 */
static bool         g_gc_scan_warned = false; /* スキャン不能の診断を一度だけ出す */
/* GC管理下オブジェクトのアドレス範囲。スタックスキャン時に、候補語が
 * この範囲外なら索引を引かずに捨てる（保守的スキャンの定数コスト削減）。 */
static uintptr_t    g_gc_addr_lo     = (uintptr_t)-1;
static uintptr_t    g_gc_addr_hi     = 0;
/* 直近の収集で走査したスタック語数（診断用。走査範囲がスタック全体に
 * 広がっていないことをテスト/カーネルが確認できる）。 */
static size_t       g_gc_scan_words  = 0;
/* 直近の収集でルート化した TLS 一時値の数（診断用）。
 * 式評価中の左オペランドと処理中の例外は、スタックではなく TLS に置かれる
 * ため保守的スタックスキャンでは見えない。収集時に明示的なルートとして
 * 扱っており、その個数をこのカウンタで観測できる。 */
static size_t       g_gc_temp_roots  = 0;

/* TLS の一時値（式評価中のオペランド、処理中の例外）をルート化する。
 * 定義は binop / f-string セクション（ファイル後方）にある。 */
static size_t gc_mark_tls_temporaries(void);

/* メモリ確保失敗（OOM）フック。既定は未設定で、従来どおり呼び出し元へNULLが
 * 返る。カーネル/埋め込みホストは p2c_runtime_set_oom_handler() で
 * 「ログして停止」または MemoryError 送出を選べる。 */
static P2C_OomHandler g_oom_handler = NULL;
static void          *g_oom_user    = NULL;

/* スタックスキャン用のアドレス索引（オープンアドレッシング）。
 * 収集のたびに g_gc_all から再構築し、スタック上の語がGCオブジェクトかどうかを
 * 参照1回（平均O(1)）で判定する。索引バッファは収集をまたいで再利用し、
 * 確保に失敗した場合は従来の線形探索へフォールバックする。 */
static void        **g_scan_index   = NULL;
static size_t        g_scan_index_cap = 0; /* 常に2の冪（0は未確保） */
static size_t        g_scan_index_used = 0;

P2C_THREAD_LOCAL P2C_ExceptFrame *p2c_exc_stack = NULL;
P2C_THREAD_LOCAL P2C_Object *p2c_active_exception = NULL;

/* ランタイムの内部確保は common 層の共有ヒープ抽象（p2c_heap_*）を通る。
 * 変換器コア（文字列ビルダ等）も同じ経路を使うため、
 *   - カーネルが p2c_platform_set() で渡したヒープ、または
 *   - libc / 自作スタブの malloc 系
 * のどちらか一方がランタイムと変換器の共通ヒープになる
 * （2つのアロケータが同じ領域を二重に使う事故を防ぐ）。 */

/* 確保失敗をOOMフックへ通知してNULLを返す共通経路（定義本体はOOMセクション）。
 * context は静的領域を指す短い名前でなければならない（解放不要）。 */
static void *p2c_malloc_checked(size_t size, const char *context) {
    void *out = p2c_heap_alloc(size);
    if (!out) p2c_runtime_notify_oom(size, context);
    return out;
}

static void *p2c_realloc_checked(void *ptr, size_t new_size, const char *context) {
    void *out = p2c_heap_realloc(ptr, new_size);
    if (!out && new_size != 0) p2c_runtime_notify_oom(new_size, context);
    return out;
}

static void *p2c_calloc_checked(size_t nmemb, size_t size, const char *context) {
    void *out = p2c_heap_calloc(nmemb, size);
    if (!out && nmemb != 0 && size != 0) p2c_runtime_notify_oom(nmemb * size, context);
    return out;
}

static char* p2c_strdup_local(const char *s) {
    size_t len = s ? strlen(s) : 0;
    char *out = (char*)p2c_malloc_checked(len + 1, "p2c_strdup");
    if (!out) return NULL;
    if (len) memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static char* p2c_strdup_n_local(const char *s, size_t len) {
    char *out = (char*)p2c_malloc_checked(len + 1, "p2c_strdup_n");
    if (!out) return NULL;
    if (len) memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

/* ============================================================
 * メモリ確保失敗（OOM）の通知
 * ============================================================
 * 確保主体（オブジェクト/文字列/コンテナ成長）がNULLを返す直前に、
 * 登録済みハンドラへ通知する。既定（ハンドラ未設定）では従来どおり静かに
 * NULLを返すため、既存の「確保失敗時はNULLを返してロールバックする」契約は
 * 一切変わらない。カーネル/埋め込みホストがハンドラを登録した場合にのみ、
 * 診断出力・MemoryError送出・独自パニックといった方針を選べる。
 *
 * 標準ハンドラは、runtime_init時に事前確保した MemoryError シングルトンを使う
 * ため、OOM中の再確保（再帰OOM）を起こさない。 */
static P2C_Object *g_oom_exception = NULL; /* 事前確保済みMemoryError（GCルート登録済み） */
static bool       g_oom_in_handler = false; /* ハンドラ再入防止 */

void p2c_runtime_set_oom_handler(P2C_OomHandler handler, void *user) {
    g_oom_handler = handler;
    g_oom_user = user;
}

void p2c_runtime_notify_oom(size_t requested, const char *context) {
    if (!g_oom_handler || g_oom_in_handler) return;
    g_oom_in_handler = true;
    g_oom_handler(requested, context ? context : "allocation", g_oom_user);
    g_oom_in_handler = false;
}

void p2c_oom_raise_memory_error(size_t requested, const char *context, void *user) {
    (void)requested;
    (void)user;
    const char *where = context ? context : "allocation";
    if (g_oom_exception && p2c_exc_stack) {
        /* p2c_raise() は longjmp で戻らないため、再入フラグは先に解除しておく
         * （except節の中での再確保失敗も通知できるようにする）。 */
        g_oom_in_handler = false;
        p2c_raise(g_oom_exception);
        return;
    }
    p2c_platform_write("MemoryError: ");
    p2c_platform_write(where);
    p2c_platform_write(" could not allocate memory (no active try/except)\n");
    p2c_platform_abort("out of memory");
}

/* 確保失敗をOOMフックへ通知してNULLを返す共通経路（実体はファイル前方の
 * p2c_malloc_checked/p2c_realloc_checked）。 */

/* ============================================================
 * 組み込みホスト向けヒープ統計
 * ============================================================ */
size_t p2c_runtime_alloc_failures(void) {
#ifdef PYTHON_CODE_TO_C_NO_STDLIB
#  ifndef PYTHON_CODE_TO_C_NO_LIBC_STUBS
    return heap_failures;
#  else
    return 0; /* カーネル提供アロケータの失敗回数はカーネル側が把握する */
#  endif
#else
    return 0;
#endif
}

static void free_map_shallow(P2C_Map *map) {
    if (!map) return;
    if (map->buckets) {
        for (size_t i = 0; i < map->bucket_count; i++) {
            P2C_MapEntry *e = map->buckets[i];
            while (e) {
                P2C_MapEntry *next = e->next;
                if (e->key) p2c_heap_free(e->key);
                p2c_heap_free(e);
                e = next;
            }
        }
        p2c_heap_free(map->buckets);
    }
    p2c_heap_free(map);
}

static P2C_Map* new_attr_map(void) {
    P2C_Map *map = (P2C_Map*)p2c_malloc_checked(sizeof(P2C_Map), "attribute map");
    if (!map) return NULL;
    map->alloc = NULL;
    map->bucket_count = 32;
    map->len = 0;
    map->hash_fn = p2c_hash_str;
    map->eq_fn = p2c_eq_str;
    map->key_free = NULL;
    map->val_free = NULL;
    map->buckets = (P2C_MapEntry**)p2c_heap_calloc(map->bucket_count, sizeof(P2C_MapEntry*));
    if (!map->buckets) { p2c_runtime_notify_oom(map->bucket_count * sizeof(P2C_MapEntry*), "attribute map buckets"); p2c_heap_free(map); return NULL; }
    return map;
}

static void attr_map_set(P2C_Map *map, const char *name, P2C_Object *val) {
    if (!map || !name) return;
    size_t h = p2c_hash_str(name) % map->bucket_count;
    P2C_MapEntry *e = map->buckets[h];
    while (e) {
        if (p2c_eq_str(e->key, name)) {
            e->val = val;
            return;
        }
        e = e->next;
    }
    e = (P2C_MapEntry*)p2c_calloc_checked(1, sizeof(P2C_MapEntry), "attribute entry");
    if (!e) return;
    e->key = p2c_strdup_local(name);
    if (!e->key) { p2c_heap_free(e); return; }
    e->val = val;
    e->next = map->buckets[h];
    map->buckets[h] = e;
    map->len++;
}

static P2C_Object* attr_map_get(P2C_Map *map, const char *name) {
    if (!map || !name) return NULL;
    size_t h = p2c_hash_str(name) % map->bucket_count;
    P2C_MapEntry *e = map->buckets[h];
    while (e) {
        if (p2c_eq_str(e->key, name)) return (P2C_Object*)e->val;
        e = e->next;
    }
    return NULL;
}

/* 属性を削除する。存在すれば true、存在しなければ false を返す
 * （new_attr_map()が作るマップはalloc=NULLでp2c_map_remove()を使うと
 * エントリ自体がリークするため、attr_map_set/getと同じ流儀で直接
 * calloc/freeを管理する専用の削除関数を用意する）。 */
static bool attr_map_remove(P2C_Map *map, const char *name) {
    if (!map || !name) return false;
    size_t h = p2c_hash_str(name) % map->bucket_count;
    P2C_MapEntry *e = map->buckets[h];
    P2C_MapEntry *prev = NULL;
    while (e) {
        if (p2c_eq_str(e->key, name)) {
            if (prev) prev->next = e->next; else map->buckets[h] = e->next;
            p2c_heap_free(e->key);
            p2c_heap_free(e);
            map->len--;
            return true;
        }
        prev = e; e = e->next;
    }
    return false;
}

static bool p2c_obj_is_exception_instance(P2C_Object *obj) {
    if (!obj) return false;
    if (obj->cls && obj->cls->type_tag == OBJ_EXCEPTION) return true;
    if (obj->cls && obj->cls->type_tag == OBJ_INSTANCE && obj->u.v_instance.klass &&
        obj->u.v_instance.klass->cls && obj->u.v_instance.klass->cls->type_tag == OBJ_CLASS) {
        const char *base_name = obj->u.v_instance.klass->u.v_class.base_name;
        return base_name && strcmp(base_name, "Exception") == 0;
    }
    return false;
}

P2C_Object* p2c_obj_new(P2C_ClassDef *cls) {
    /* 閾値を超えていたら collect してから確保する */
    if (g_gc_enabled && g_gc_stack_bottom &&
        g_gc_bytes_alloc >= g_gc_threshold) {
        /* runtime.h が宣言している p2c_gc_collect を、この後方の定義より先に呼ぶ */
        p2c_gc_collect();
    }
    P2C_Object *obj = (P2C_Object*)p2c_malloc_checked(sizeof(P2C_Object), "object");
    if (!obj) return NULL;
    obj->cls       = cls;
    obj->refcount  = 0;       /* GC管理下では refcount はピン留めカウンタ */
    memset(&obj->u, 0, sizeof(obj->u));
    /* GC 追跡リストの先頭に繋ぐ */
    obj->gc_next   = g_gc_all;
    obj->gc_marked = 0;
    g_gc_all = obj;
    g_gc_bytes_alloc += sizeof(P2C_Object);
    g_gc_obj_count++;
    /* スタックスキャン用のアドレス範囲を更新する。 */
    {
        uintptr_t addr = (uintptr_t)obj;
        if (addr < g_gc_addr_lo) g_gc_addr_lo = addr;
        if (addr > g_gc_addr_hi) g_gc_addr_hi = addr;
    }
    return obj;
}

static void p2c_gc_discard_new_object(P2C_Object *obj) {
    if (!obj) return;
    P2C_Object **link = &g_gc_all;
    while (*link) {
        if (*link == obj) {
            *link = obj->gc_next;
            if (g_gc_obj_count > 0) g_gc_obj_count--;
            break;
        }
        link = &(*link)->gc_next;
    }
    p2c_heap_free(obj);
}

void p2c_obj_incref(P2C_Object *obj) {
    if (obj && obj != &P2C_None && obj != &P2C_True && obj != &P2C_False) obj->refcount++;
}

/* オブジェクトの型固有データを解放するヘルパー (GC sweep から呼ぶ)。
 * P2C_Object 本体の free() は呼び出し元が行う。 */
static void gc_free_obj_data(P2C_Object *obj) {
    switch (obj->cls ? obj->cls->type_tag : OBJ_NONE) {
        case OBJ_STR:
            p2c_heap_free(obj->u.v_str.data);
            break;
        case OBJ_LIST:
            p2c_heap_free(obj->u.v_list.items);
            break;
        case OBJ_TUPLE:
            p2c_heap_free(obj->u.v_tuple.items);
            break;
        case OBJ_DICT:
        case OBJ_SET:
            if (obj->u.v_dict.buckets) {
                for (size_t i = 0; i < obj->u.v_dict.bucket_count; i++) {
                    P2C_DictEntry *e = obj->u.v_dict.buckets[i];
                    while (e) {
                        P2C_DictEntry *next = e->next;
                        p2c_heap_free(e);
                        e = next;
                    }
                }
                p2c_heap_free(obj->u.v_dict.buckets);
            }
            break;
        case OBJ_FUNCTION:
            p2c_heap_free(obj->u.v_function.name);
            break;
        case OBJ_CLASS:
            p2c_heap_free(obj->u.v_class.name);
            p2c_heap_free(obj->u.v_class.base_name);
            p2c_heap_free(obj->u.v_class.mro);
            free_map_shallow(obj->u.v_class.attrs);
            break;
        case OBJ_INSTANCE:
            free_map_shallow(obj->u.v_instance.attrs);
            break;
        case OBJ_MODULE:
            p2c_heap_free(obj->u.v_module.name);
            free_map_shallow(obj->u.v_module.attrs);
            break;
        case OBJ_EXCEPTION:
            p2c_heap_free(obj->u.v_exception.msg);
            p2c_heap_free(obj->u.v_exception.type_name);
            break;
        default:
            break;
    }
    if (obj->cls && obj->cls->free_fn && obj->cls->free_fn != free_none)
        obj->cls->free_fn(obj);
}

/* p2c_obj_decref: refcount (ピン留めカウンタ) を減らすだけ。
 * オブジェクトの解放は p2c_gc_collect() が担う。 */
void p2c_obj_decref(P2C_Object *obj) {
    if (!obj || obj == &P2C_None || obj == &P2C_True || obj == &P2C_False || obj == &P2C_Ellipsis) return;
    if (obj->refcount > 0) obj->refcount--;
}

P2C_Object* p2c_obj_from_bool(bool v) { return v ? &P2C_True : &P2C_False; }
P2C_Object* p2c_obj_from_int(int64_t v) {
    if (v >= P2C_SMALL_INT_MIN && v <= P2C_SMALL_INT_MAX) {
        p2c_small_ints_init();
        return &g_small_ints[(size_t)(v - P2C_SMALL_INT_MIN)];
    }
    P2C_Object *o = p2c_obj_new(&P2C_Class_Int);
    if (o) o->u.v_int = v;
    return o;
}
P2C_Object* p2c_obj_from_float(double v) { P2C_Object *o = p2c_obj_new(&P2C_Class_Float); if (o) o->u.v_float = v; return o; }
P2C_Object* p2c_obj_from_str(const char *s) { return p2c_obj_from_str_n(s, s ? strlen(s) : 0); }
P2C_Object* p2c_obj_from_str_n(const char *s, size_t len) {
    P2C_Object *o = p2c_obj_new(&P2C_Class_Str);
    if (!o) return NULL;
    o->u.v_str.data = p2c_strdup_n_local(s ? s : "", len);
    if (!o->u.v_str.data) { p2c_gc_discard_new_object(o); return NULL; }
    o->u.v_str.len = len;
    return o;
}

bool p2c_obj_is_int(P2C_Object *obj)   { return obj && obj->cls && obj->cls->type_tag == OBJ_INT; }
bool p2c_obj_is_float(P2C_Object *obj) { return obj && obj->cls && obj->cls->type_tag == OBJ_FLOAT; }
bool p2c_obj_is_str(P2C_Object *obj)   { return obj && obj->cls && obj->cls->type_tag == OBJ_STR; }
bool p2c_obj_is_list(P2C_Object *obj)  { return obj && obj->cls && obj->cls->type_tag == OBJ_LIST; }
bool p2c_obj_is_dict(P2C_Object *obj)  { return obj && obj->cls && obj->cls->type_tag == OBJ_DICT; }
bool p2c_obj_is_set(P2C_Object *obj)   { return obj && obj->cls && obj->cls->type_tag == OBJ_SET; }
bool p2c_obj_is_bool(P2C_Object *obj)  { return obj && obj->cls && obj->cls->type_tag == OBJ_BOOL; }
bool p2c_obj_is_tuple(P2C_Object *obj) { return obj && obj->cls && obj->cls->type_tag == OBJ_TUPLE; }

/* Pythonのfloat比較は「正確な一致」が仕様である（例: 0.1 + 0.2 == 0.3 は False）。
 * epsilon付き比較へ置き換えると意味が変わってしまうため、等価判定をこの2つに
 * 集約し、意図した厳密比較であることを明示したうえで -Wfloat-equal を
 * 局所的に許可する。NaN判定は p2c_float_ne(v, v) で行う。 */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif
static bool p2c_float_eq(double a, double b) { return a == b; }
static bool p2c_float_ne(double a, double b) { return a != b; }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

bool p2c_obj_is_truthy(P2C_Object *obj) {
    if (!obj || obj == &P2C_None) return false;
    switch (obj->cls ? obj->cls->type_tag : OBJ_NONE) {
        case OBJ_BOOL: return obj->u.v_bool;
        case OBJ_INT: return obj->u.v_int != 0;
        case OBJ_FLOAT: return p2c_float_ne(obj->u.v_float, 0.0);
        case OBJ_STR: return obj->u.v_str.len > 0;
        case OBJ_LIST: return obj->u.v_list.len > 0;
        case OBJ_DICT: return obj->u.v_dict.len > 0;
        case OBJ_SET: return obj->u.v_dict.len > 0;
        case OBJ_TUPLE: return obj->u.v_tuple.len > 0;
        default: return true;
    }
}

int64_t p2c_obj_as_int(P2C_Object *obj) {
    if (!obj) return 0;
    switch (obj->cls ? obj->cls->type_tag : OBJ_NONE) {
        case OBJ_INT: return obj->u.v_int;
        case OBJ_FLOAT: return (int64_t)obj->u.v_float;
        case OBJ_BOOL: return obj->u.v_bool ? 1 : 0;
        case OBJ_STR: {
            if (!obj->u.v_str.data) return 0;
            char *end = NULL;
            long long v = strtoll(obj->u.v_str.data, &end, 10);
            if (end == obj->u.v_str.data) { p2c_raise(p2c_make_exception("ValueError", "invalid literal for int()")); return 0; }
            return (int64_t)v;
        }
        default: return 0;
    }
}

double p2c_obj_as_float(P2C_Object *obj) {
    if (!obj) return 0.0;
    switch (obj->cls ? obj->cls->type_tag : OBJ_NONE) {
        case OBJ_FLOAT: return obj->u.v_float;
        case OBJ_INT: return (double)obj->u.v_int;
        case OBJ_BOOL: return obj->u.v_bool ? 1.0 : 0.0;
        case OBJ_STR: {
            if (!obj->u.v_str.data) return 0.0;
            char *end = NULL;
            double v = strtod(obj->u.v_str.data, &end);
            if (end == obj->u.v_str.data) { p2c_raise(p2c_make_exception("ValueError", "could not convert string to float")); return 0.0; }
            return v;
        }
        default: return 0.0;
    }
}

const char* p2c_obj_as_str(P2C_Object *obj) {
    if (!obj) return "";
    if (p2c_obj_is_str(obj) && obj->u.v_str.data) return obj->u.v_str.data;
    if (obj == &P2C_True) return "True";
    if (obj == &P2C_False) return "False";
    if (obj == &P2C_None) return "None";
    return "";
}

/* 演算子ディスパッチでダンダーメソッド(__add__等)を呼ぶ。
 * p2c_has_method / p2c_call_attr の宣言は runtime.h にある。 */
/* インスタンスが対応するダンダーメソッドを持っていればそれを呼び出し、
 * 結果を *out に入れて true を返す。無ければ false（呼び出し元は
 * 既存の組み込み型向けフォールバック処理を続ける）。 */
static bool try_binop_dunder(P2C_Object *a, P2C_Object *b, const char *method, P2C_Object **out) {
    if (!a || !a->cls || a->cls->type_tag != OBJ_INSTANCE) return false;
    if (!p2c_has_method(a, method)) return false;
    P2C_Object *args[1] = { b };
    *out = p2c_call_attr(a, method, args, 1);
    return true;
}

static bool p2c_int_add_checked(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
    *out = a + b;
    return true;
}

static bool p2c_int_sub_checked(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b)) return false;
    *out = a - b;
    return true;
}

static bool p2c_int_mul_checked(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if ((a == INT64_MIN && b == -1) || (b == INT64_MIN && a == -1)) return false;
    if (a > 0) {
        if ((b > 0 && a > INT64_MAX / b) || (b < 0 && b < INT64_MIN / a)) return false;
    } else if ((b > 0 && a < INT64_MIN / b) || (b < 0 && a < INT64_MAX / b)) {
        return false;
    }
    *out = a * b;
    return true;
}

static P2C_Object* p2c_int_overflow(void) {
    p2c_raise(p2c_make_exception("OverflowError", "integer operation exceeds signed 64-bit range"));
    return &P2C_None;
}

P2C_Object* p2c_obj_add(P2C_Object *a, P2C_Object *b) {
    if (!a || !b) return &P2C_None;
    { P2C_Object *r; if (try_binop_dunder(a, b, "__add__", &r)) return r; }
    if (p2c_obj_is_list(a) && p2c_obj_is_list(b)) {
        P2C_Object *out = p2c_list_new();
        if (!out) return &P2C_None;
        for (size_t i = 0; i < a->u.v_list.len; i++) p2c_list_append(out, a->u.v_list.items[i]);
        for (size_t i = 0; i < b->u.v_list.len; i++) p2c_list_append(out, b->u.v_list.items[i]);
        return out;
    }
    if (p2c_obj_is_str(a) || p2c_obj_is_str(b)) return p2c_str_concat(a, b);
    if (p2c_obj_is_float(a) || p2c_obj_is_float(b)) return p2c_obj_from_float(p2c_obj_as_float(a) + p2c_obj_as_float(b));
    int64_t result = 0;
    if (!p2c_int_add_checked(p2c_obj_as_int(a), p2c_obj_as_int(b), &result)) return p2c_int_overflow();
    return p2c_obj_from_int(result);
}
static P2C_Object* p2c_set_binary_op(P2C_Object *a, P2C_Object *b, int op);

P2C_Object* p2c_obj_sub(P2C_Object *a, P2C_Object *b) {
    { P2C_Object *r; if (try_binop_dunder(a, b, "__sub__", &r)) return r; }
    if (p2c_obj_is_set(a) || p2c_obj_is_set(b)) return p2c_set_binary_op(a, b, 2);
    if (p2c_obj_is_float(a) || p2c_obj_is_float(b)) return p2c_obj_from_float(p2c_obj_as_float(a) - p2c_obj_as_float(b));
    int64_t result = 0;
    if (!p2c_int_sub_checked(p2c_obj_as_int(a), p2c_obj_as_int(b), &result)) return p2c_int_overflow();
    return p2c_obj_from_int(result);
}
P2C_Object* p2c_obj_mul(P2C_Object *a, P2C_Object *b) {
    { P2C_Object *r; if (try_binop_dunder(a, b, "__mul__", &r)) return r; }
    /* int*str の順序（右辺が文字列）にも対応するため、必要なら入れ替える */
    if (p2c_obj_is_int(a) && p2c_obj_is_str(b)) { P2C_Object *t = a; a = b; b = t; }
    if (p2c_obj_is_str(a) && p2c_obj_is_int(b)) {
        int64_t repeat = p2c_obj_as_int(b);
        if (repeat <= 0) return p2c_obj_from_str("");
        size_t len = a->u.v_str.len;
        P2C_Object *out = p2c_obj_new(&P2C_Class_Str);
        if (!out) return &P2C_None;
        out->u.v_str.data = (char*)p2c_malloc_checked((size_t)repeat * len + 1, "str repeat");
        if (!out->u.v_str.data) { p2c_gc_discard_new_object(out); return &P2C_None; }
        for (int64_t i = 0; i < repeat; i++) memcpy(out->u.v_str.data + (size_t)i * len, a->u.v_str.data, len);
        out->u.v_str.data[(size_t)repeat * len] = '\0';
        out->u.v_str.len = (size_t)repeat * len;
        return out;
    }
    /* list*int / int*list (例: [0] * 5) */
    if (p2c_obj_is_int(a) && p2c_obj_is_list(b)) { P2C_Object *t = a; a = b; b = t; }
    if (p2c_obj_is_list(a) && p2c_obj_is_int(b)) {
        int64_t repeat = p2c_obj_as_int(b);
        P2C_Object *out = p2c_list_new();
        if (!out) return &P2C_None;
        for (int64_t r = 0; r < repeat; r++) {
            for (size_t i = 0; i < a->u.v_list.len; i++) p2c_list_append(out, a->u.v_list.items[i]);
        }
        return out;
    }
    /* tuple*int / int*tuple */
    if (a && a->cls && a->cls->type_tag == OBJ_INT && b && b->cls && b->cls->type_tag == OBJ_TUPLE) { P2C_Object *t = a; a = b; b = t; }
    if (a && a->cls && a->cls->type_tag == OBJ_TUPLE && b && p2c_obj_is_int(b)) {
        int64_t repeat = p2c_obj_as_int(b);
        size_t src_len = a->u.v_tuple.len;
        size_t total = (repeat > 0) ? (size_t)repeat * src_len : 0;
        P2C_Object *out = p2c_obj_new(&P2C_Class_Tuple);
        if (!out) return &P2C_None;
        if (total == 0) {
            out->u.v_tuple.items = NULL;
            out->u.v_tuple.len = 0;
            return out;
        }
        out->u.v_tuple.items = (P2C_Object**)p2c_malloc_checked(total * sizeof(P2C_Object*), "tuple items");
        if (!out->u.v_tuple.items) {
            out->u.v_tuple.len = 0;
            p2c_raise(p2c_make_exception("MemoryError", "tuple repetition allocation failed"));
            return &P2C_None;
        }
        out->u.v_tuple.len = total;
        size_t idx = 0;
        for (int64_t r = 0; r < repeat; r++) for (size_t i = 0; i < src_len; i++) out->u.v_tuple.items[idx++] = a->u.v_tuple.items[i];
        return out;
    }
    if (p2c_obj_is_float(a) || p2c_obj_is_float(b)) return p2c_obj_from_float(p2c_obj_as_float(a) * p2c_obj_as_float(b));
    int64_t result = 0;
    if (!p2c_int_mul_checked(p2c_obj_as_int(a), p2c_obj_as_int(b), &result)) return p2c_int_overflow();
    return p2c_obj_from_int(result);
}
P2C_Object* p2c_obj_div(P2C_Object *a, P2C_Object *b) {
    { P2C_Object *r; if (try_binop_dunder(a, b, "__truediv__", &r)) return r; }
    if (p2c_float_eq(p2c_obj_as_float(b), 0.0)) { p2c_raise(p2c_make_exception("ZeroDivisionError", "division by zero")); return &P2C_None; }
    return p2c_obj_from_float(p2c_obj_as_float(a) / p2c_obj_as_float(b));
}
P2C_Object* p2c_obj_floordiv(P2C_Object *a, P2C_Object *b) {
    { P2C_Object *r; if (try_binop_dunder(a, b, "__floordiv__", &r)) return r; }
    if (p2c_obj_is_float(a) || p2c_obj_is_float(b)) {
        double db = p2c_obj_as_float(b);
        if (p2c_float_eq(db, 0.0)) { p2c_raise(p2c_make_exception("ZeroDivisionError", "float floor division by zero")); return &P2C_None; }
        return p2c_obj_from_float(floor(p2c_obj_as_float(a) / db));
    }
    int64_t ib = p2c_obj_as_int(b);
    if (ib == 0) { p2c_raise(p2c_make_exception("ZeroDivisionError", "integer division by zero")); return &P2C_None; }
    int64_t ia = p2c_obj_as_int(a);
    if (ia == INT64_MIN && ib == -1) return p2c_int_overflow();
    /* Pythonの // は結果を負の無限大方向へ切り捨てる（Cの / は0方向への切り捨て）。
     * 符号が異なり、かつ割り切れない場合は1補正する。 */
    int64_t q = ia / ib;
    int64_t r = ia % ib;
    if (r != 0 && ((r < 0) != (ib < 0))) q -= 1;
    return p2c_obj_from_int(q);
}
P2C_Object* p2c_obj_mod(P2C_Object *a, P2C_Object *b) {
    { P2C_Object *r; if (try_binop_dunder(a, b, "__mod__", &r)) return r; }
    if (p2c_obj_is_float(a) || p2c_obj_is_float(b)) {
        double da = p2c_obj_as_float(a), db = p2c_obj_as_float(b);
        if (p2c_float_eq(db, 0.0)) { p2c_raise(p2c_make_exception("ZeroDivisionError", "float modulo")); return &P2C_None; }
        double r = fmod(da, db);
        if (p2c_float_ne(r, 0.0) && ((r < 0.0) != (db < 0.0))) r += db;
        return p2c_obj_from_float(r);
    }
    int64_t ib = p2c_obj_as_int(b);
    if (ib == 0) { p2c_raise(p2c_make_exception("ZeroDivisionError", "modulo by zero")); return &P2C_None; }
    int64_t ia = p2c_obj_as_int(a);
    if (ia == INT64_MIN && ib == -1) return p2c_obj_from_int(0);
    /* Pythonの % は除数と同じ符号の結果を返す（Cの % は被除数と同じ符号）。 */
    int64_t r = ia % ib;
    if (r != 0 && ((r < 0) != (ib < 0))) r += ib;
    return p2c_obj_from_int(r);
}
P2C_Object* p2c_obj_pow(P2C_Object *a, P2C_Object *b) {
    if (p2c_obj_is_float(a) || p2c_obj_is_float(b)) return p2c_obj_from_float(pow(p2c_obj_as_float(a), p2c_obj_as_float(b)));
    int64_t base = p2c_obj_as_int(a), exp = p2c_obj_as_int(b), result = 1;
    if (exp < 0) {
        if (base == 0) {
            p2c_raise(p2c_make_exception("ZeroDivisionError", "zero cannot be raised to a negative power"));
            return &P2C_None;
        }
        return p2c_obj_from_float(pow((double)base, (double)exp));
    }
    while (exp > 0) {
        if ((exp & 1) != 0 && !p2c_int_mul_checked(result, base, &result)) return p2c_int_overflow();
        exp >>= 1;
        if (exp > 0 && !p2c_int_mul_checked(base, base, &base)) return p2c_int_overflow();
    }
    return p2c_obj_from_int(result);
}
P2C_Object* p2c_obj_bitand(P2C_Object *a, P2C_Object *b) {
    if (p2c_obj_is_set(a) || p2c_obj_is_set(b)) return p2c_set_binary_op(a, b, 1);
    return p2c_obj_from_int(p2c_obj_as_int(a) & p2c_obj_as_int(b));
}
P2C_Object* p2c_obj_bitor(P2C_Object *a, P2C_Object *b) {
    if (p2c_obj_is_dict(a) || p2c_obj_is_dict(b)) {
        if (!p2c_obj_is_dict(a) || !p2c_obj_is_dict(b)) {
            p2c_raise(p2c_make_exception("TypeError", "unsupported operand types for |"));
            return &P2C_None;
        }
        P2C_Object *out = p2c_dict_new();
        for (P2C_DictEntry *entry = a->u.v_dict.order_head; entry; entry = entry->order_next) p2c_dict_set(out, entry->key, entry->val);
        for (P2C_DictEntry *entry = b->u.v_dict.order_head; entry; entry = entry->order_next) p2c_dict_set(out, entry->key, entry->val);
        return out;
    }
    if (p2c_obj_is_set(a) || p2c_obj_is_set(b)) return p2c_set_binary_op(a, b, 0);
    return p2c_obj_from_int(p2c_obj_as_int(a) | p2c_obj_as_int(b));
}
P2C_Object* p2c_obj_bitxor(P2C_Object *a, P2C_Object *b) {
    if (p2c_obj_is_set(a) || p2c_obj_is_set(b)) return p2c_set_binary_op(a, b, 3);
    return p2c_obj_from_int(p2c_obj_as_int(a) ^ p2c_obj_as_int(b));
}
P2C_Object* p2c_obj_invert(P2C_Object *a) { return p2c_obj_from_int(~p2c_obj_as_int(a)); }
P2C_Object* p2c_obj_lshift(P2C_Object *a, P2C_Object *b) {
    int64_t value = p2c_obj_as_int(a), count = p2c_obj_as_int(b);
    if (count < 0) { p2c_raise(p2c_make_exception("ValueError", "negative shift count")); return &P2C_None; }
    if (count >= 63) { p2c_raise(p2c_make_exception("OverflowError", "shift count exceeds Alpha0.6 64-bit integer range")); return &P2C_None; }
    int64_t factor = ((int64_t)1) << count;
    int64_t result = 0;
    if (!p2c_int_mul_checked(value, factor, &result)) {
        p2c_raise(p2c_make_exception("OverflowError", "left shift exceeds Alpha0.6 64-bit integer range"));
        return &P2C_None;
    }
    return p2c_obj_from_int(result);
}
P2C_Object* p2c_obj_rshift(P2C_Object *a, P2C_Object *b) {
    int64_t value = p2c_obj_as_int(a), count = p2c_obj_as_int(b);
    if (count < 0) { p2c_raise(p2c_make_exception("ValueError", "negative shift count")); return &P2C_None; }
    if (count >= 63) return p2c_obj_from_int(value < 0 ? -1 : 0);
    int64_t divisor = ((int64_t)1) << count;
    int64_t q = value / divisor, r = value % divisor;
    if (r != 0 && value < 0) q -= 1;
    return p2c_obj_from_int(q);
}

static bool p2c_obj_is_hashable(P2C_Object *obj) {
    if (!obj || !obj->cls) return true;
    switch (obj->cls->type_tag) {
        case OBJ_LIST:
        case OBJ_DICT:
        case OBJ_SET:
            return false;
        case OBJ_TUPLE:
            for (size_t i = 0; i < obj->u.v_tuple.len; i++) {
                if (!p2c_obj_is_hashable(obj->u.v_tuple.items[i])) return false;
            }
            return true;
        default:
            return true;
    }
}

static bool p2c_require_hashable(P2C_Object *obj) {
    if (p2c_obj_is_hashable(obj)) return true;
    p2c_raise(p2c_make_exception("TypeError", "unhashable object"));
    return false;
}

static uint32_t p2c_obj_hash(P2C_Object *obj) {
    if (!obj) return 0;
    switch (obj->cls ? obj->cls->type_tag : OBJ_NONE) {
        case OBJ_INT: return (uint32_t)(obj->u.v_int ^ (obj->u.v_int >> 32));
        case OBJ_BOOL: return obj->u.v_bool ? 1u : 0u;
        case OBJ_FLOAT: {
            double value = obj->u.v_float;
            if (p2c_float_eq(value, 0.0)) return 0u;
            if (value > (double)INT64_MIN && value < (double)INT64_MAX) {
                int64_t integer = (int64_t)value;
                if (p2c_float_eq((double)integer, value)) return (uint32_t)(integer ^ (integer >> 32));
            }
            union { double d; uint64_t u; } cvt; cvt.d = value;
            return (uint32_t)(cvt.u ^ (cvt.u >> 32));
        }
        case OBJ_STR: return p2c_hash_str(obj->u.v_str.data ? obj->u.v_str.data : "");
        case OBJ_TUPLE: {
            uint32_t hash = 2166136261u;
            for (size_t i = 0; i < obj->u.v_tuple.len; i++) {
                hash ^= p2c_obj_hash(obj->u.v_tuple.items[i]);
                hash *= 16777619u;
            }
            return hash ^ (uint32_t)obj->u.v_tuple.len;
        }
        default: return (uint32_t)(((uintptr_t)obj) >> 3);
    }
}

static bool p2c_obj_equal_raw(P2C_Object *a, P2C_Object *b) {
    if (a == b) return true;
    if (!a || !b || !a->cls || !b->cls) return false;
    if (a->cls->type_tag != b->cls->type_tag) {
        if ((p2c_obj_is_int(a) || p2c_obj_is_float(a) || p2c_obj_is_bool(a)) && (p2c_obj_is_int(b) || p2c_obj_is_float(b) || p2c_obj_is_bool(b))) {
            return p2c_float_eq(p2c_obj_as_float(a), p2c_obj_as_float(b));
        }
        return false;
    }
    switch (a->cls->type_tag) {
        case OBJ_INT: return a->u.v_int == b->u.v_int;
        case OBJ_FLOAT: return p2c_float_eq(a->u.v_float, b->u.v_float);
        case OBJ_BOOL: return a->u.v_bool == b->u.v_bool;
        case OBJ_STR: return strcmp(p2c_obj_as_str(a), p2c_obj_as_str(b)) == 0;
        case OBJ_LIST:
            /* Pythonのlist比較は要素ごとの値比較（ポインタの同一性ではない）。 */
            if (a->u.v_list.len != b->u.v_list.len) return false;
            for (size_t i = 0; i < a->u.v_list.len; i++) if (!p2c_obj_equal_raw(a->u.v_list.items[i], b->u.v_list.items[i])) return false;
            return true;
        case OBJ_TUPLE:
            if (a->u.v_tuple.len != b->u.v_tuple.len) return false;
            for (size_t i = 0; i < a->u.v_tuple.len; i++) if (!p2c_obj_equal_raw(a->u.v_tuple.items[i], b->u.v_tuple.items[i])) return false;
            return true;
        case OBJ_SET:
            if (a->u.v_dict.len != b->u.v_dict.len) return false;
            for (P2C_DictEntry *e = a->u.v_dict.order_head; e; e = e->order_next) {
                if (!p2c_obj_is_truthy(p2c_obj_contains(b, e->key))) return false;
            }
            return true;
        case OBJ_DICT:
            if (a->u.v_dict.len != b->u.v_dict.len) return false;
            for (P2C_DictEntry *e = a->u.v_dict.order_head; e; e = e->order_next) {
                if (!p2c_obj_is_truthy(p2c_obj_contains(b, e->key))) return false;
                if (!p2c_obj_equal_raw(e->val, p2c_dict_get(b, e->key))) return false;
            }
            return true;
        case OBJ_INSTANCE:
            /* ユーザー定義の __eq__ があればそれを使う（以前はここが常にポインタの
             * 同一性比較になっており、__eq__をオーバーライドしても一切反映されなかった）。 */
            if (p2c_has_method(a, "__eq__")) {
                P2C_Object *args[1] = { b };
                P2C_Object *result = p2c_call_attr(a, "__eq__", args, 1);
                return p2c_obj_is_truthy(result);
            }
            return a == b;
        default: return a == b;
    }
}

P2C_Object* p2c_obj_eq(P2C_Object *a, P2C_Object *b) { return p2c_obj_from_bool(p2c_obj_equal_raw(a, b)); }
P2C_Object* p2c_obj_ne(P2C_Object *a, P2C_Object *b) { return p2c_obj_from_bool(!p2c_obj_equal_raw(a, b)); }
P2C_Object* p2c_obj_lt(P2C_Object *a, P2C_Object *b) { { P2C_Object *r; if (try_binop_dunder(a, b, "__lt__", &r)) return r; } return p2c_obj_from_bool((p2c_obj_is_float(a)||p2c_obj_is_float(b)) ? (p2c_obj_as_float(a) < p2c_obj_as_float(b)) : (p2c_obj_as_int(a) < p2c_obj_as_int(b))); }
P2C_Object* p2c_obj_le(P2C_Object *a, P2C_Object *b) { { P2C_Object *r; if (try_binop_dunder(a, b, "__le__", &r)) return r; } return p2c_obj_from_bool((p2c_obj_is_float(a)||p2c_obj_is_float(b)) ? (p2c_obj_as_float(a) <= p2c_obj_as_float(b)) : (p2c_obj_as_int(a) <= p2c_obj_as_int(b))); }
P2C_Object* p2c_obj_gt(P2C_Object *a, P2C_Object *b) { { P2C_Object *r; if (try_binop_dunder(a, b, "__gt__", &r)) return r; } return p2c_obj_from_bool((p2c_obj_is_float(a)||p2c_obj_is_float(b)) ? (p2c_obj_as_float(a) > p2c_obj_as_float(b)) : (p2c_obj_as_int(a) > p2c_obj_as_int(b))); }
P2C_Object* p2c_obj_ge(P2C_Object *a, P2C_Object *b) { { P2C_Object *r; if (try_binop_dunder(a, b, "__ge__", &r)) return r; } return p2c_obj_from_bool((p2c_obj_is_float(a)||p2c_obj_is_float(b)) ? (p2c_obj_as_float(a) >= p2c_obj_as_float(b)) : (p2c_obj_as_int(a) >= p2c_obj_as_int(b))); }
P2C_Object* p2c_bool_and(P2C_Object *a, P2C_Object *b) { return p2c_obj_from_bool(p2c_obj_is_truthy(a) && p2c_obj_is_truthy(b)); }
P2C_Object* p2c_bool_or(P2C_Object *a, P2C_Object *b) { return p2c_obj_from_bool(p2c_obj_is_truthy(a) || p2c_obj_is_truthy(b)); }
P2C_Object* p2c_bool_not(P2C_Object *a) { return p2c_obj_from_bool(!p2c_obj_is_truthy(a)); }

/* x is y: 同一オブジェクトかどうか（Noneチェック等に使う）。
 * python_code_to_cのオブジェクトは基本的に値セマンティクスなので、int/float/strの
 * `is`はpython3とは厳密には一致しないが、`x is None`の最重要ケースは正しく動く。 */
P2C_Object* p2c_obj_is(P2C_Object *container, P2C_Object *item) {
    return p2c_obj_from_bool(container == item);
}

/* item in container: リスト・タプル・文字列・辞書（キー）に対応。
 * 引数順は (container, item)（Python の `item in container` 構文に合わせ、
 * コード生成側で引数の順番を swap している）。 */
P2C_Object* p2c_obj_contains(P2C_Object *container, P2C_Object *item) {
    if (!container || !item) return &P2C_False;
    if (container->cls && container->cls->type_tag == OBJ_INSTANCE && p2c_has_method(container, "__contains__")) {
        P2C_Object *args[1] = { item };
        return p2c_obj_from_bool(p2c_obj_is_truthy(p2c_call_attr(container, "__contains__", args, 1)));
    }
    /* リスト・タプル: 各要素と等値比較 */
    if (p2c_obj_is_list(container) || container->cls == &P2C_Class_Tuple) {
        P2C_Object **items;
        size_t len;
        if (p2c_obj_is_list(container)) {
            items = container->u.v_list.items;
            len = container->u.v_list.len;
        } else {
            items = container->u.v_tuple.items;
            len = container->u.v_tuple.len;
        }
        for (size_t i = 0; i < len; i++) {
            if (p2c_obj_equal_raw(items[i], item)) return &P2C_True;
        }
        return &P2C_False;
    }
    /* 文字列: substring check */
    if (p2c_obj_is_str(container) && p2c_obj_is_str(item)) {
        return p2c_obj_from_bool(strstr(container->u.v_str.data, item->u.v_str.data) != NULL);
    }
    /* 辞書: キーの存在確認 */
    if (container->cls == &P2C_Class_Dict || container->cls == &P2C_Class_Set) {
        if (!p2c_require_hashable(item)) return &P2C_False;
        if (!container->u.v_dict.buckets) return &P2C_False;
        for (size_t i = 0; i < container->u.v_dict.bucket_count; i++) {
            for (P2C_DictEntry *e = container->u.v_dict.buckets[i]; e; e = e->next) {
                if (p2c_obj_equal_raw(e->key, item)) return &P2C_True;
            }
        }
        return &P2C_False;
    }
    /* range オブジェクト（rangeはリストとして展開されているので基本到達しないが念のため）*/
    return &P2C_False;
}

P2C_Object* p2c_list_new(void) {
    P2C_Object *o = p2c_obj_new(&P2C_Class_List);
    if (!o) return NULL;
    o->u.v_list.items = NULL; o->u.v_list.len = 0; o->u.v_list.cap = 0; return o;
}
P2C_Object* p2c_list_from_array(P2C_Object **items, size_t len) {
    P2C_Object *o = p2c_list_new();
    if (!o || len == 0) return o;
    if (len > (size_t)-1 / sizeof(P2C_Object*)) return o;
    o->u.v_list.items = (P2C_Object**)p2c_malloc_checked(len * sizeof(P2C_Object*), "list items");
    if (!o->u.v_list.items) return o;
    memcpy(o->u.v_list.items, items, len * sizeof(P2C_Object*));
    o->u.v_list.len = len;
    o->u.v_list.cap = len;
    return o;
}
void p2c_list_append(P2C_Object *list, P2C_Object *item) {
    if (!list || !p2c_obj_is_list(list)) return;
    if (list->u.v_list.len >= list->u.v_list.cap) {
        size_t new_cap = list->u.v_list.cap ? list->u.v_list.cap * 2 : 8;
        P2C_Object **new_items = (P2C_Object**)p2c_realloc_checked(list->u.v_list.items, new_cap * sizeof(P2C_Object*), "list growth");
        if (!new_items) return;
        list->u.v_list.items = new_items; list->u.v_list.cap = new_cap;
    }
    list->u.v_list.items[list->u.v_list.len++] = item;
}
P2C_Object* p2c_list_get(P2C_Object *list, size_t idx) {
    if (!list || !p2c_obj_is_list(list) || idx >= list->u.v_list.len) { p2c_raise(p2c_make_exception("IndexError", "list index out of range")); return &P2C_None; }
    return list->u.v_list.items[idx];
}
void p2c_list_set(P2C_Object *list, size_t idx, P2C_Object *val) {
    if (!list || !p2c_obj_is_list(list) || idx >= list->u.v_list.len) { p2c_raise(p2c_make_exception("IndexError", "list assignment index out of range")); return; }
    list->u.v_list.items[idx] = val;
}
size_t p2c_list_len(P2C_Object *list) { return (list && p2c_obj_is_list(list)) ? list->u.v_list.len : 0; }
P2C_Object* p2c_list_pop(P2C_Object *list) {
    if (!list || !p2c_obj_is_list(list) || list->u.v_list.len == 0) {
        p2c_raise(p2c_make_exception("IndexError", "pop from empty list"));
        return &P2C_None;
    }
    return list->u.v_list.items[--list->u.v_list.len];
}
/* del list[idx] : 指定インデックスの要素を削除し、後続要素を1つずつ
 * 前に詰める。負のインデックス（末尾からの相対位置）にも対応する。 */
void p2c_list_delete_at(P2C_Object *list, int64_t idx) {
    if (!list || !p2c_obj_is_list(list)) { p2c_raise(p2c_make_exception("TypeError", "object is not a list")); return; }
    size_t len = list->u.v_list.len;
    if (idx < 0) idx += (int64_t)len;
    if (idx < 0 || (size_t)idx >= len) { p2c_raise(p2c_make_exception("IndexError", "list assignment index out of range")); return; }
    for (size_t i = (size_t)idx; i + 1 < len; i++) list->u.v_list.items[i] = list->u.v_list.items[i + 1];
    list->u.v_list.len--;
}

P2C_Object* p2c_dict_new(void) {
    P2C_Object *o = p2c_obj_new(&P2C_Class_Dict);
    if (!o) return NULL;
    o->u.v_dict.bucket_count = 32;
    o->u.v_dict.len = 0;
    o->u.v_dict.order_head = NULL;
    o->u.v_dict.order_tail = NULL;
    o->u.v_dict.buckets = (P2C_DictEntry**)p2c_calloc_checked(o->u.v_dict.bucket_count, sizeof(P2C_DictEntry*), "dict buckets");
    if (!o->u.v_dict.buckets) { p2c_gc_discard_new_object(o); return NULL; }
    return o;
}
P2C_Object* p2c_set_new(void) {
    P2C_Object *o = p2c_obj_new(&P2C_Class_Set);
    if (!o) return NULL;
    o->u.v_dict.bucket_count = 32;
    o->u.v_dict.len = 0;
    o->u.v_dict.order_head = NULL;
    o->u.v_dict.order_tail = NULL;
    o->u.v_dict.buckets = (P2C_DictEntry**)p2c_calloc_checked(o->u.v_dict.bucket_count, sizeof(P2C_DictEntry*), "dict buckets");
    if (!o->u.v_dict.buckets) { p2c_gc_discard_new_object(o); return NULL; }
    return o;
}

void p2c_set_add(P2C_Object *set, P2C_Object *item) {
    if (!set || !p2c_obj_is_set(set) || !item) {
        p2c_raise(p2c_make_exception("TypeError", "set add requires a set and a value"));
        return;
    }
    if (!p2c_require_hashable(item)) return;
    size_t h = p2c_obj_hash(item) % set->u.v_dict.bucket_count;
    for (P2C_DictEntry *entry = set->u.v_dict.buckets[h]; entry; entry = entry->next) {
        if (p2c_obj_equal_raw(entry->key, item)) return;
    }
    P2C_DictEntry *entry = (P2C_DictEntry*)p2c_calloc_checked(1, sizeof(P2C_DictEntry), "dict entry");
    if (!entry) return;
    entry->key = item;
    entry->val = &P2C_None;
    entry->next = set->u.v_dict.buckets[h];
    set->u.v_dict.buckets[h] = entry;
    if (set->u.v_dict.order_tail) set->u.v_dict.order_tail->order_next = entry;
    else set->u.v_dict.order_head = entry;
    set->u.v_dict.order_tail = entry;
    set->u.v_dict.len++;
}

static bool p2c_set_remove_item(P2C_Object *set, P2C_Object *item) {
    if (!set || !p2c_obj_is_set(set) || !item || set->u.v_dict.bucket_count == 0) return false;
    if (!p2c_require_hashable(item)) return false;
    size_t h = p2c_obj_hash(item) % set->u.v_dict.bucket_count;
    P2C_DictEntry *prev = NULL;
    for (P2C_DictEntry *entry = set->u.v_dict.buckets[h]; entry; entry = entry->next) {
        if (!p2c_obj_equal_raw(entry->key, item)) { prev = entry; continue; }
        if (prev) prev->next = entry->next; else set->u.v_dict.buckets[h] = entry->next;
        P2C_DictEntry *order_prev = NULL;
        for (P2C_DictEntry *order = set->u.v_dict.order_head; order; order = order->order_next) {
            if (order != entry) { order_prev = order; continue; }
            if (order_prev) order_prev->order_next = order->order_next;
            else set->u.v_dict.order_head = order->order_next;
            if (set->u.v_dict.order_tail == order) set->u.v_dict.order_tail = order_prev;
            break;
        }
        p2c_heap_free(entry);
        set->u.v_dict.len--;
        return true;
    }
    return false;
}

static void p2c_set_clear_items(P2C_Object *set) {
    if (!set || !p2c_obj_is_set(set)) return;
    for (size_t i = 0; i < set->u.v_dict.bucket_count; i++) {
        P2C_DictEntry *entry = set->u.v_dict.buckets[i];
        while (entry) { P2C_DictEntry *next = entry->next; p2c_heap_free(entry); entry = next; }
        set->u.v_dict.buckets[i] = NULL;
    }
    set->u.v_dict.order_head = NULL;
    set->u.v_dict.order_tail = NULL;
    set->u.v_dict.len = 0;
}

P2C_Object* p2c_set_from_array(P2C_Object **items, size_t len) {
    P2C_Object *set = p2c_set_new();
    if (!set) return NULL;
    for (size_t i = 0; i < len; i++) p2c_set_add(set, items[i]);
    return set;
}

static P2C_Object* p2c_set_binary_op(P2C_Object *a, P2C_Object *b, int op) {
    if (!p2c_obj_is_set(a) || !p2c_obj_is_set(b)) {
        p2c_raise(p2c_make_exception("TypeError", "set operations require two sets"));
        return &P2C_None;
    }
    P2C_Object *out = p2c_set_new();
    if (!out) return NULL;
    if (op == 0 || op == 3) {
        for (P2C_DictEntry *e = a->u.v_dict.order_head; e; e = e->order_next) {
            if (op == 0 || !p2c_obj_is_truthy(p2c_obj_contains(b, e->key))) p2c_set_add(out, e->key);
        }
    } else {
        for (P2C_DictEntry *e = a->u.v_dict.order_head; e; e = e->order_next) {
            bool in_b = p2c_obj_is_truthy(p2c_obj_contains(b, e->key));
            if ((op == 1 && in_b) || (op == 2 && !in_b)) p2c_set_add(out, e->key);
        }
    }
    if (op == 0 || op == 3) {
        for (P2C_DictEntry *e = b->u.v_dict.order_head; e; e = e->order_next) {
            if (op == 0 || !p2c_obj_is_truthy(p2c_obj_contains(a, e->key))) p2c_set_add(out, e->key);
        }
    }
    return out;
}

P2C_Object* p2c_dict_from_pairs(P2C_Object **keys, P2C_Object **vals, size_t len) {
    P2C_Object *o = p2c_dict_new(); if (!o) return NULL;
    for (size_t i = 0; i < len; i++) p2c_dict_set(o, keys[i], vals[i]);
    return o;
}

P2C_Object* p2c_dict_fromkeys(P2C_Object *iterable, P2C_Object *value) {
    P2C_Object *out = p2c_dict_new();
    P2C_Object *keys;
    if (!out) return NULL;
    keys = p2c_builtin_list(iterable);
    if (!keys || !p2c_obj_is_list(keys)) return out;
    for (size_t i = 0; i < keys->u.v_list.len; i++) p2c_dict_set(out, keys->u.v_list.items[i], value ? value : &P2C_None);
    return out;
}

void p2c_dict_set(P2C_Object *dict, P2C_Object *key, P2C_Object *val) {
    if (!dict || !p2c_obj_is_dict(dict) || !key) return;
    if (!p2c_require_hashable(key)) return;
    size_t h = p2c_obj_hash(key) % dict->u.v_dict.bucket_count;
    P2C_DictEntry *e = dict->u.v_dict.buckets[h];
    while (e) {
        if (p2c_obj_equal_raw(e->key, key)) { e->val = val; return; }
        e = e->next;
    }
    e = (P2C_DictEntry*)p2c_calloc_checked(1, sizeof(P2C_DictEntry), "dict entry");
    if (!e) return;
    e->key = key;
    e->val = val;
    e->next = dict->u.v_dict.buckets[h];
    e->order_next = NULL;
    dict->u.v_dict.buckets[h] = e;
    if (dict->u.v_dict.order_tail) dict->u.v_dict.order_tail->order_next = e;
    else dict->u.v_dict.order_head = e;
    dict->u.v_dict.order_tail = e;
    dict->u.v_dict.len++;
}

void p2c_dict_update(P2C_Object *dict, P2C_Object *mapping) {
    if (!dict || !p2c_obj_is_dict(dict) || !mapping || !p2c_obj_is_dict(mapping)) {
        p2c_raise(p2c_make_exception("TypeError", "dictionary update requires a dictionary"));
        return;
    }
    for (P2C_DictEntry *entry = mapping->u.v_dict.order_head; entry; entry = entry->order_next) {
        p2c_dict_set(dict, entry->key, entry->val);
    }
}
P2C_Object* p2c_dict_get(P2C_Object *dict, P2C_Object *key) {
    if (!dict || !p2c_obj_is_dict(dict) || !key) return &P2C_None;
    if (!p2c_require_hashable(key)) return &P2C_None;
    size_t h = p2c_obj_hash(key) % dict->u.v_dict.bucket_count;
    P2C_DictEntry *e = dict->u.v_dict.buckets[h];
    while (e) { if (p2c_obj_equal_raw(e->key, key)) return e->val; e = e->next; }
    p2c_raise(p2c_make_exception("KeyError", "dictionary key not found"));
    return &P2C_None;
}

/* del dict[key] : キーが存在すれば削除して true、存在しなければ
 * KeyError を送出する（Pythonの仕様通り）。 */
/* srcのうち、名前がexclude[0..n_exclude)のいずれにも一致しないエントリだけを
 * 集めた新しいdictを返す。f(**d) で呼び出した先の関数が **kwargs
 * （名前付きパラメータに収まらない残りをまとめて受け取る仮引数）を持つとき、
 * 「名前付きパラメータとして個別に取り出した名前」を除いた残りをkwargsとして
 * 渡すために使う（exclude が空なら単純にsrcの複製になる）。 */
P2C_Object* p2c_dict_exclude_keys(P2C_Object *src, const char **exclude, size_t n_exclude) {
    P2C_Object *out = p2c_dict_new();
    if (!out || !src || !p2c_obj_is_dict(src)) return out;
    for (P2C_DictEntry *e = src->u.v_dict.order_head; e; e = e->order_next) {
        const char *kstr = p2c_obj_is_str(e->key) ? e->key->u.v_str.data : NULL;
        bool skip = false;
        if (kstr) {
            for (size_t j = 0; j < n_exclude; j++) {
                if (exclude[j] && strcmp(kstr, exclude[j]) == 0) { skip = true; break; }
            }
        }
        if (!skip) p2c_dict_set(out, e->key, e->val);
    }
    return out;
}
void p2c_dict_remove(P2C_Object *dict, P2C_Object *key) {
    if (!dict || !p2c_obj_is_dict(dict) || !key) return;
    if (!p2c_require_hashable(key)) return;
    size_t h = p2c_obj_hash(key) % dict->u.v_dict.bucket_count;
    P2C_DictEntry *e = dict->u.v_dict.buckets[h];
    P2C_DictEntry *prev = NULL;
    while (e) {
        if (p2c_obj_equal_raw(e->key, key)) {
            if (prev) prev->next = e->next; else dict->u.v_dict.buckets[h] = e->next;
            P2C_DictEntry *order_prev = NULL;
            P2C_DictEntry *order_cur = dict->u.v_dict.order_head;
            while (order_cur && order_cur != e) {
                order_prev = order_cur;
                order_cur = order_cur->order_next;
            }
            if (order_cur) {
                if (order_prev) order_prev->order_next = order_cur->order_next;
                else dict->u.v_dict.order_head = order_cur->order_next;
                if (dict->u.v_dict.order_tail == order_cur) dict->u.v_dict.order_tail = order_prev;
            }
            p2c_heap_free(e);
            dict->u.v_dict.len--;
            return;
        }
        prev = e; e = e->next;
    }
    p2c_raise(p2c_make_exception("KeyError", "dictionary key not found"));
}

/* **d アンパック用: キーが存在すれば値を返し、なければ default_val を返す (KeyError を送出しない) */
P2C_Object* p2c_dict_get_with_default(P2C_Object *dict, P2C_Object *key, P2C_Object *default_val) {
    if (!dict || !p2c_obj_is_dict(dict) || !key) return default_val ? default_val : &P2C_None;
    if (!p2c_require_hashable(key)) return &P2C_None;
    if (!dict->u.v_dict.buckets || dict->u.v_dict.bucket_count == 0) return default_val ? default_val : &P2C_None;
    size_t h = p2c_obj_hash(key) % dict->u.v_dict.bucket_count;
    P2C_DictEntry *e = dict->u.v_dict.buckets[h];
    while (e) { if (p2c_obj_equal_raw(e->key, key)) return e->val; e = e->next; }
    return default_val ? default_val : &P2C_None;
}
size_t p2c_dict_len(P2C_Object *dict) { return (dict && p2c_obj_is_dict(dict)) ? dict->u.v_dict.len : 0; }

P2C_Object* p2c_tuple_new(size_t len) {
    P2C_Object *o = p2c_obj_new(&P2C_Class_Tuple);
    if (!o) return NULL;
    o->u.v_tuple.items = (P2C_Object**)p2c_calloc_checked(len ? len : 1, sizeof(P2C_Object*), "tuple slots");
    if (!o->u.v_tuple.items) { p2c_gc_discard_new_object(o); return NULL; }
    o->u.v_tuple.len = len; return o;
}
P2C_Object* p2c_tuple_from_array(P2C_Object **items, size_t len) {
    P2C_Object *o = p2c_tuple_new(len); if (!o) return NULL;
    for (size_t i = 0; i < len; i++) o->u.v_tuple.items[i] = items[i];
    return o;
}
void p2c_tuple_set(P2C_Object *tuple, size_t idx, P2C_Object *val) { if (tuple && p2c_obj_is_tuple(tuple) && idx < tuple->u.v_tuple.len) tuple->u.v_tuple.items[idx] = val; }
P2C_Object* p2c_tuple_get(P2C_Object *tuple, size_t idx) {
    if (!tuple || !p2c_obj_is_tuple(tuple) || idx >= tuple->u.v_tuple.len) { p2c_raise(p2c_make_exception("IndexError", "tuple index out of range")); return &P2C_None; }
    return tuple->u.v_tuple.items[idx];
}
size_t p2c_tuple_len(P2C_Object *tuple) { return (tuple && p2c_obj_is_tuple(tuple)) ? tuple->u.v_tuple.len : 0; }

P2C_Object* p2c_str_concat(P2C_Object *a, P2C_Object *b) {
    const char *sa = p2c_obj_as_str(a), *sb = p2c_obj_as_str(b);
    size_t la = strlen(sa), lb = strlen(sb);
    P2C_Object *o = p2c_obj_new(&P2C_Class_Str);
    if (!o) return NULL;
    o->u.v_str.data = (char*)p2c_malloc_checked(la + lb + 1, "str concat");
    if (!o->u.v_str.data) { p2c_gc_discard_new_object(o); return NULL; }
    memcpy(o->u.v_str.data, sa, la); memcpy(o->u.v_str.data + la, sb, lb); o->u.v_str.data[la + lb] = '\0';
    o->u.v_str.len = la + lb; return o;
}
size_t p2c_obj_str_len(P2C_Object *s) { return (s && p2c_obj_is_str(s)) ? s->u.v_str.len : 0; }

/* Python の str.format() 実装。
 * {} (自動インデックス), {0}/{1} (位置), {name:fmt} のフォーマット仕様に対応。
 * シンプルな動的バッファで実装し、内部のP2C_Stringビルダーには依存しない。 */
P2C_Object* p2c_str_format_py(const char *fmt, P2C_Object **args, size_t nargs) {
    if (!fmt) return p2c_obj_from_str("");
    /* 出力バッファ: 適宜倍増する */
    size_t cap = strlen(fmt) * 3 + 64;
    char *out = (char*)p2c_malloc_checked(cap, "text builder");
    if (!out) return p2c_obj_from_str("");
    size_t out_len = 0;
    size_t auto_idx = 0;

    /* バッファへ文字列を追記するラムダ相当のマクロ */
#define FBUF_APPEND(str) do { \
    const char *_s = (str); size_t _l = strlen(_s); \
    while (out_len + _l + 1 >= cap) { cap *= 2; char *_r = (char*)p2c_realloc_checked(out, cap, "text builder growth"); if (!_r) { p2c_heap_free(out); return p2c_obj_from_str(""); } out = _r; } \
    memcpy(out + out_len, _s, _l); out_len += _l; out[out_len] = '\0'; } while(0)
#define FBUF_APPENDC(c) do { \
    if (out_len + 2 >= cap) { cap *= 2; char *_r = (char*)p2c_realloc_checked(out, cap, "text builder growth"); if (!_r) { p2c_heap_free(out); return p2c_obj_from_str(""); } out = _r; } \
    out[out_len++] = (c); out[out_len] = '\0'; } while(0)

    const char *p = fmt;
    while (*p) {
        if (*p == '{') {
            p++;
            if (*p == '{') { FBUF_APPENDC('{'); p++; continue; }
            char spec_buf[256] = {0}; size_t spec_len = 0;
            while (*p && *p != '}' && spec_len < 255) spec_buf[spec_len++] = *p++;
            if (*p == '}') p++;
            spec_buf[spec_len] = '\0';
            /* フォーマット仕様 ':' で分割 */
            char *colon = strchr(spec_buf, ':');
            const char *fmt_spec = colon ? colon + 1 : "";
            if (colon) *colon = '\0';
            /* インデックス決定 */
            size_t idx = auto_idx++;
            if (spec_buf[0] >= '0' && spec_buf[0] <= '9') idx = (size_t)atoi(spec_buf);
            P2C_Object *arg = (idx < nargs) ? args[idx] : &P2C_None;
            char val_buf[256] = {0};
            if (fmt_spec[0]) {
                /* C printf仕様に変換: %<spec_without_last><spec_char>
                 * fmt_spec は spec_buf（最大255文字）由来のため、"%" + fmt_spec +
                 * 型文字 + '\0' が確実に収まるサイズを確保する
                 * （固定64バイトだと極端に長い書式指定でGCCのformat-truncation
                 * 警告(-Werror)が出るのに加え、実際に切り詰められうる）。 */
                char c_fmt[sizeof(spec_buf) + 16];
                char spec_char = fmt_spec[strlen(fmt_spec)-1];
                if (spec_char == 'd' || spec_char == 'i') {
                    snprintf(c_fmt, sizeof(c_fmt), "%%%s", fmt_spec);
                    snprintf(val_buf, sizeof(val_buf), c_fmt, (long long)p2c_obj_as_int(arg));
                } else if (spec_char == 'f' || spec_char == 'e' || spec_char == 'g' ||
                           spec_char == 'E' || spec_char == 'G') {
                    snprintf(c_fmt, sizeof(c_fmt), "%%%s", fmt_spec);
                    snprintf(val_buf, sizeof(val_buf), c_fmt, p2c_obj_as_float(arg));
                } else if (spec_char == 's') {
                    snprintf(c_fmt, sizeof(c_fmt), "%%%ss", fmt_spec);
                    const char *sv = p2c_obj_as_str(arg);
                    snprintf(val_buf, sizeof(val_buf), c_fmt, sv ? sv : "None");
                } else {
                    /* フォールバック */
                    if (p2c_obj_is_int(arg)) { snprintf(c_fmt, sizeof(c_fmt), "%%%sd", fmt_spec); snprintf(val_buf, sizeof(val_buf), c_fmt, (long long)p2c_obj_as_int(arg)); }
                    else if (p2c_obj_is_float(arg)) { snprintf(c_fmt, sizeof(c_fmt), "%%%sg", fmt_spec); snprintf(val_buf, sizeof(val_buf), c_fmt, p2c_obj_as_float(arg)); }
                    else { const char *sv = p2c_obj_as_str(arg); snprintf(val_buf, sizeof(val_buf), "%s", sv ? sv : "None"); }
                }
                FBUF_APPEND(val_buf);
            } else {
                /* 書式なし: str() 相当 - p2c_obj_str() で int/float も正しく変換 */
                P2C_Object *sval = p2c_obj_str(arg);
                const char *sv = (sval && p2c_obj_is_str(sval) && sval->u.v_str.data) ? sval->u.v_str.data : p2c_obj_as_str(arg);
                FBUF_APPEND(sv ? sv : "None");
            }
        } else if (*p == '}' && *(p+1) == '}') {
            FBUF_APPENDC('}'); p += 2;
        } else {
            FBUF_APPENDC(*p++);
        }
    }
#undef FBUF_APPEND
#undef FBUF_APPENDC
    P2C_Object *result = p2c_obj_from_str(out);
    p2c_heap_free(out);
    return result;
}

P2C_Object* p2c_str_format(const char *fmt, ...) {
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
    va_list args, args2; va_start(args, fmt); va_copy(args2, args);
    int n = vsnprintf(NULL, 0, fmt, args); va_end(args);
    if (n < 0) { va_end(args2); return p2c_obj_from_str(""); }
    char *buf = (char*)p2c_malloc_checked((size_t)n + 1, "format buffer"); if (!buf) { va_end(args2); return p2c_obj_from_str(""); }
    vsnprintf(buf, (size_t)n + 1, fmt, args2); va_end(args2);
    P2C_Object *out = p2c_obj_from_str(buf); p2c_heap_free(buf); return out;
#else
    (void)fmt; return p2c_obj_from_str("");
#endif
}

P2C_Object* p2c_function_new(const char *name, P2C_CallableFn func) {
    P2C_Object *o = p2c_obj_new(&P2C_Class_Function); if (!o) return NULL;
    o->u.v_function.name = p2c_strdup_local(name ? name : "<fn>");
    if (!o->u.v_function.name) { p2c_gc_discard_new_object(o); return NULL; }
    o->u.v_function.func = func;
    o->u.v_function.closure_func = NULL;
    o->u.v_function.env = NULL;
    return o;
}
P2C_Object* p2c_closure_new(const char *name, P2C_ClosureFn func, P2C_Object *env) {
    P2C_Object *o = p2c_obj_new(&P2C_Class_Function); if (!o) return NULL;
    o->u.v_function.name = p2c_strdup_local(name ? name : "<closure>");
    if (!o->u.v_function.name) { p2c_gc_discard_new_object(o); return NULL; }
    o->u.v_function.func = NULL;
    o->u.v_function.closure_func = func;
    o->u.v_function.env = env ? env : p2c_dict_new();
    if (!o->u.v_function.env) {
        p2c_heap_free(o->u.v_function.name);
        p2c_gc_discard_new_object(o);
        p2c_raise(p2c_make_exception("MemoryError", "could not allocate closure environment"));
        return &P2C_None;
    }
    return o;
}
P2C_Object* p2c_cell_new(P2C_Object *value) {
    P2C_Object *cell = p2c_obj_new(&P2C_Class_Cell);
    if (!cell) return &P2C_None;
    cell->u.v_cell.value = value ? value : &P2C_None;
    return cell;
}
P2C_Object* p2c_cell_get(P2C_Object *cell) {
    if (!cell || !cell->cls || cell->cls->type_tag != OBJ_CELL) {
        p2c_raise(p2c_make_exception("TypeError", "object is not a closure cell"));
        return &P2C_None;
    }
    return cell->u.v_cell.value ? cell->u.v_cell.value : &P2C_None;
}
void p2c_cell_set(P2C_Object *cell, P2C_Object *value) {
    if (!cell || !cell->cls || cell->cls->type_tag != OBJ_CELL) {
        p2c_raise(p2c_make_exception("TypeError", "object is not a closure cell"));
        return;
    }
    cell->u.v_cell.value = value ? value : &P2C_None;
}
static void p2c_register_class(const char *name, P2C_Object *cls_obj);

P2C_Object* p2c_class_new(const char *name, P2C_CallableFn ctor, P2C_MethodDef *methods, const char *base_name) {
    P2C_Object *o = p2c_obj_new(&P2C_Class_ClassObject); if (!o) return NULL;
    o->u.v_class.name = p2c_strdup_local(name ? name : "<class>");
    o->u.v_class.ctor = ctor;
    o->u.v_class.methods = methods;
    o->u.v_class.attrs = new_attr_map();
    o->u.v_class.base_name = p2c_strdup_local(base_name ? base_name : "");
    /* MROキャッシュは初回のメソッド・属性解決時に計算する（未計算はNULL）。 */
    o->u.v_class.mro = NULL;
    if (!o->u.v_class.name || !o->u.v_class.attrs || !o->u.v_class.base_name) {
        p2c_heap_free(o->u.v_class.name);
        p2c_heap_free(o->u.v_class.base_name);
        free_map_shallow(o->u.v_class.attrs);
        p2c_gc_discard_new_object(o);
        return NULL;
    }
    p2c_register_class(o->u.v_class.name, o);
    return o;
}
P2C_Object* p2c_instance_new(P2C_Object *klass) {
    P2C_Object *o = p2c_obj_new(&P2C_Class_Instance); if (!o) return NULL;
    o->u.v_instance.klass = klass;
    o->u.v_instance.attrs = new_attr_map();
    if (!o->u.v_instance.attrs) { p2c_gc_discard_new_object(o); return NULL; }
    return o;
}
P2C_Object* p2c_module_new(const char *name) {
    P2C_Object *o = p2c_obj_new(&P2C_Class_Module); if (!o) return NULL;
    o->u.v_module.name = p2c_strdup_local(name ? name : "<module>");
    o->u.v_module.attrs = new_attr_map();
    if (!o->u.v_module.name || !o->u.v_module.attrs) {
        p2c_heap_free(o->u.v_module.name);
        free_map_shallow(o->u.v_module.attrs);
        p2c_gc_discard_new_object(o);
        return NULL;
    }
    return o;
}
void p2c_module_set_attr(P2C_Object *module, const char *name, P2C_Object *val) {
    if (module && module->cls && module->cls->type_tag == OBJ_MODULE) attr_map_set(module->u.v_module.attrs, name, val);
}
void p2c_register_module(P2C_Object *module) {
    if (!module || !module->u.v_module.name) return;
    if (!g_module_registry) g_module_registry = new_attr_map();
    if (!g_module_registry) return;
    attr_map_set(g_module_registry, module->u.v_module.name, module);
}
P2C_Object* p2c_import_module(const char *name) {
    if (!g_module_registry) g_module_registry = new_attr_map();
    P2C_Object *module = attr_map_get(g_module_registry, name);
    if (module) return module;
    p2c_raise(p2c_make_exception("ImportError", name ? name : "<module>"));
    return &P2C_None;
}
P2C_Object* p2c_call(P2C_Object *callable, P2C_Object **args, size_t nargs) {
    if (!callable || !callable->cls) { p2c_raise(p2c_make_exception("TypeError", "object is not callable")); return &P2C_None; }
    switch (callable->cls->type_tag) {
        case OBJ_FUNCTION:
            if (callable->u.v_function.closure_func) return callable->u.v_function.closure_func(callable->u.v_function.env, args, nargs);
            return callable->u.v_function.func ? callable->u.v_function.func(args, nargs) : &P2C_None;
        case OBJ_CLASS:
            return callable->u.v_class.ctor ? callable->u.v_class.ctor(args, nargs) : &P2C_None;
        default:
            p2c_raise(p2c_make_exception("TypeError", "object is not callable"));
            return &P2C_None;
    }
}
/* この後方（sorted/集合順序付け）で使う前方宣言。
 * p2c_builtin_sorted の宣言は runtime.h にある。 */
static P2C_Object** p2c_iter_items(P2C_Object *obj, size_t *out_n, bool *out_owned);

/* クラス名 -> クラスオブジェクトの簡易レジストリ。
 * 継承(base_name)は名前の文字列でしか保持されていないため、
 * メソッド解決時に基底クラスのクラスオブジェクトを辿れるように登録しておく。
 * (以前はこの仕組みがなく、サブクラスが自分でオーバーライドしていない
 * メソッドを呼び出すと解決できずクラッシュしていた。) */
#define P2C_MAX_CLASS_REGISTRY 256
static const char *g_class_registry_names[P2C_MAX_CLASS_REGISTRY];
static P2C_Object *g_class_registry_objs[P2C_MAX_CLASS_REGISTRY];
static int g_class_registry_count = 0;

static void p2c_register_class(const char *name, P2C_Object *cls_obj) {
    if (!name || g_class_registry_count >= P2C_MAX_CLASS_REGISTRY) return;
    g_class_registry_names[g_class_registry_count] = name;
    g_class_registry_objs[g_class_registry_count] = cls_obj;
    g_class_registry_count++;
}

static void p2c_reset_class_registry(void) {
    memset(g_class_registry_names, 0, sizeof(g_class_registry_names));
    memset(g_class_registry_objs, 0, sizeof(g_class_registry_objs));
    g_class_registry_count = 0;
}

static P2C_Object* p2c_find_class_by_name(const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < g_class_registry_count; i++) {
        if (strcmp(g_class_registry_names[i], name) == 0) return g_class_registry_objs[i];
    }
    return NULL;
}

/* ---- クラス階層のC3線形化（PythonのMRO） ------------------------------
 * 継承関係は名前文字列（"Base" や "Left,Right"）で保持しているため、名前から
 * p2c_find_class_by_name() でクラスオブジェクトを引き、Pythonと同じC3線形化
 *     L(C) = C + merge(L(B1), ..., L(Bn), [B1, ..., Bn])
 * を計算する。ダイヤモンド継承
 *     class A:        def who(self): ...
 *     class B(A):     pass
 *     class C(A):     def who(self): ...
 *     class D(B, C):  pass
 * ではPythonは C.who を選ぶ（MROは D, B, C, A）。以前は「左の基底から順に
 * 深さ優先で再帰する」近似だったため A.who を選び、CPythonと静かに異なる
 * 結果を返していた。
 *
 * 計算結果はクラスごとに一度だけ求め、v_class.mro へ「自分以外の名前を解決順に
 * カンマで連結した文字列」としてキャッシュする（基底関係はクラス生成時に確定し、
 * 以後変化しないため再計算は不要）。
 *
 * 破綻した階層（循環、深さ・幅・アリーナの上限超過、mergeの失敗）では従来と
 * 同じ深さ優先の訪問順をキャッシュする。このときの解決結果は以前の実装と
 * 一致する（安全側へのフォールバック）。
 */
#define P2C_MRO_MAX_NAMES 32
#define P2C_MRO_MAX_BASES 8
#define P2C_MRO_MAX_DEPTH 12
/* アリーナはMRO計算中だけ使う一時領域。組込み（カーネルスタックが数KB）でも
 * 収まるよう、各再帰フレームの作業配列は小さく、アリーナも4KB未満に抑える。 */
#define P2C_MRO_ARENA_BYTES 3072

typedef struct {
    const char *names[P2C_MRO_MAX_NAMES];
    size_t count;
} P2C_NameList;

typedef struct {
    char *base;
    size_t used;
} P2C_MroArena;

/* MRO文字列（"B,C,A"形式）をコピーせずに走査するための範囲。 */
typedef struct {
    const char *ptr;
    size_t len;
} P2C_NameSpan;

/* クラス名はクラスオブジェクトやbase_nameに紐づく文字列を指すだけでよいが、
 * strtok_rが書き換える一時バッファは関数を抜けると消えるため、線形化の間だけ
 * 有効なアリーナへコピーして寿命を揃える。 */
static const char* p2c_mro_intern(P2C_MroArena *arena, const char *text, size_t len) {
    if (!arena || !text) return NULL;
    if (len + 1u > (size_t)P2C_MRO_ARENA_BYTES - arena->used) return NULL;
    char *slot = arena->base + arena->used;
    memcpy(slot, text, len);
    slot[len] = '\0';
    arena->used += len + 1u;
    return slot;
}

static bool p2c_name_list_add(P2C_NameList *list, const char *name) {
    if (!list || !name || !name[0]) return false;
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->names[i], name) == 0) return true;
    }
    if (list->count >= P2C_MRO_MAX_NAMES) return false;
    list->names[list->count] = name;
    list->count++;
    return true;
}

static bool p2c_name_list_has_from(const P2C_NameList *list, size_t from, const char *name) {
    for (size_t i = from; i < list->count; i++) {
        if (strcmp(list->names[i], name) == 0) return true;
    }
    return false;
}

/* C3のmerge。lists[0..nlists-1]の先頭から、他のどのリストの末尾にも現れない
 * 名前を1つずつ取り出してoutへ移す。取り出せる名前が無ければ階層が一貫して
 * いない（循環など）ためfalseを返す。 */
static bool p2c_c3_merge(P2C_NameList *const *lists, size_t nlists, P2C_NameList *out) {
    size_t remaining = 0;
    for (size_t i = 0; i < nlists; i++) remaining += lists[i]->count;
    while (remaining > 0) {
        bool took = false;
        for (size_t i = 0; i < nlists && !took; i++) {
            if (lists[i]->count == 0) continue;
            const char *candidate = lists[i]->names[0];
            bool blocked = false;
            for (size_t j = 0; j < nlists && !blocked; j++) {
                if (j == i || lists[j]->count == 0) continue;
                if (p2c_name_list_has_from(lists[j], 1, candidate)) blocked = true;
            }
            if (blocked) continue;
            if (!p2c_name_list_add(out, candidate)) return false;
            for (size_t j = 0; j < nlists; j++) {
                if (lists[j]->count > 0 && strcmp(lists[j]->names[0], candidate) == 0) {
                    memmove(&lists[j]->names[0], &lists[j]->names[1],
                            (lists[j]->count - 1) * sizeof(const char*));
                    lists[j]->count--;
                }
            }
            remaining = 0;
            for (size_t j = 0; j < nlists; j++) remaining += lists[j]->count;
            took = true;
        }
        if (!took) return false;
    }
    return true;
}

/* outの先頭には自分自身の名前が入る。 */
static bool p2c_c3_linearize(P2C_Object *cls_obj, P2C_NameList *out, P2C_MroArena *arena, int depth) {
    if (!cls_obj || !out || !arena || depth > P2C_MRO_MAX_DEPTH) return false;
    if (!p2c_name_list_add(out, cls_obj->u.v_class.name)) return false;
    const char *bases = cls_obj->u.v_class.base_name;
    if (!bases || !bases[0]) return true;
    char buf[512];
    size_t blen = strlen(bases);
    if (blen >= sizeof(buf)) return false;
    memcpy(buf, bases, blen + 1u);
    P2C_NameList direct;
    P2C_NameList sub_lists[P2C_MRO_MAX_BASES];
    P2C_NameList *all_lists[P2C_MRO_MAX_BASES + 1];
    size_t nsub = 0;
    direct.count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        /* 直接の基底が上限を超える階層はC3を諦めて従来の深さ優先へ委ねる
         * （スタック使用量を有界に保つため）。 */
        if (nsub >= P2C_MRO_MAX_BASES) return false;
        const char *interned = p2c_mro_intern(arena, tok, strlen(tok));
        if (!interned) return false;
        if (!p2c_name_list_add(&direct, interned)) return false;
        sub_lists[nsub].count = 0;
        /* 未登録の基底（組み込み例外名など）は名前だけを1要素リストとして残し、
         * MROには含める（解決順の比較には名前で十分なため）。 */
        P2C_Object *base_cls = p2c_find_class_by_name(interned);
        if (base_cls && base_cls != cls_obj) {
            if (!p2c_c3_linearize(base_cls, &sub_lists[nsub], arena, depth + 1)) return false;
        } else if (!p2c_name_list_add(&sub_lists[nsub], interned)) {
            return false;
        }
        all_lists[nsub] = &sub_lists[nsub];
        nsub++;
    }
    all_lists[nsub] = &direct;
    P2C_NameList tail;
    tail.count = 0;
    if (!p2c_c3_merge(all_lists, nsub + 1, &tail)) return false;
    for (size_t i = 0; i < tail.count; i++) {
        if (!p2c_name_list_add(out, tail.names[i])) return false;
    }
    return true;
}

/* C3を計算できない階層向けのフォールバック。以前の実装と同じ「左の基底から
 * 順に深さ優先で訪問した順序」を重複除去して並べる。 */
static bool p2c_mro_dfs_collect(P2C_Object *cls_obj, P2C_NameList *out, P2C_MroArena *arena, int depth) {
    if (!cls_obj || !out || !arena || depth > P2C_MRO_MAX_DEPTH) return false;
    const char *bases = cls_obj->u.v_class.base_name;
    if (!bases || !bases[0]) return true;
    char buf[512];
    size_t blen = strlen(bases);
    if (blen >= sizeof(buf)) return false;
    memcpy(buf, bases, blen + 1u);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        const char *interned = p2c_mro_intern(arena, tok, strlen(tok));
        if (!interned) return false;
        if (!p2c_name_list_add(out, interned)) return false;
        P2C_Object *base_cls = p2c_find_class_by_name(interned);
        if (base_cls && base_cls != cls_obj) {
            if (!p2c_mro_dfs_collect(base_cls, out, arena, depth + 1)) return false;
        }
    }
    return true;
}

/* list[from..]の名前をカンマで連結したヒープ文字列を作る。 */
static char* p2c_mro_join(const P2C_NameList *list, size_t from) {
    size_t total = 1u;
    for (size_t i = from; i < list->count; i++) total += strlen(list->names[i]) + 1u;
    char *joined = (char*)p2c_malloc_checked(total, "class MRO");
    if (!joined) return NULL;
    size_t pos = 0;
    for (size_t i = from; i < list->count; i++) {
        size_t len = strlen(list->names[i]);
        if (i > from) joined[pos++] = ',';
        memcpy(joined + pos, list->names[i], len);
        pos += len;
    }
    joined[pos] = '\0';
    return joined;
}

/* クラスのMRO（自分自身を除く解決順）を返す。初回だけ計算してキャッシュする。
 * 計算自体に失敗した場合も「継承なし」を表す空文字列を返し、毎回の再計算を防ぐ。 */
static const char* p2c_class_mro(P2C_Object *cls_obj) {
    if (!cls_obj || !cls_obj->cls || cls_obj->cls->type_tag != OBJ_CLASS) return NULL;
    if (cls_obj->u.v_class.mro) return cls_obj->u.v_class.mro;
    char arena_buf[P2C_MRO_ARENA_BYTES];
    P2C_MroArena arena;
    P2C_NameList order;
    arena.base = arena_buf;
    arena.used = 0;
    order.count = 0;
    if (!p2c_c3_linearize(cls_obj, &order, &arena, 0)) {
        order.count = 0;
        arena.used = 0;
        if (!p2c_mro_dfs_collect(cls_obj, &order, &arena, 0)) order.count = 0;
    }
    char *joined = p2c_mro_join(&order, 1);
    cls_obj->u.v_class.mro = joined ? joined : p2c_strdup_local("");
    return cls_obj->u.v_class.mro;
}

/* MRO文字列から次の名前を取り出す。offsetは呼び出し側が0で初期化し、名前が
 * 尽きたらfalseを返す。名前はコピーしない（呼び出し中はMRO文字列が生存する）。 */
static bool p2c_mro_next(const char *mro, size_t *offset, P2C_NameSpan *out) {
    if (!mro || !offset || !out) return false;
    size_t i = *offset;
    if (mro[i] == '\0') return false;
    size_t start = i;
    while (mro[i] != '\0' && mro[i] != ',') i++;
    out->ptr = mro + start;
    out->len = i - start;
    if (mro[i] == ',') i++;
    *offset = i;
    return out->len > 0;
}

static bool p2c_name_span_equals(const P2C_NameSpan *span, const char *name) {
    if (!span || !name) return false;
    return strlen(name) == span->len && memcmp(name, span->ptr, span->len) == 0;
}

static P2C_Object* p2c_find_class_by_span(const P2C_NameSpan *span) {
    if (!span || span->len == 0) return NULL;
    for (int i = 0; i < g_class_registry_count; i++) {
        const char *name = g_class_registry_names[i];
        if (name && p2c_name_span_equals(span, name)) return g_class_registry_objs[i];
    }
    return NULL;
}

static P2C_MethodDef* p2c_find_own_method(P2C_Object *cls_obj, const char *name) {
    if (!cls_obj || !name) return NULL;
    for (P2C_MethodDef *m = cls_obj->u.v_class.methods; m && m->name; m++) {
        if (strcmp(m->name, name) == 0) return m;
    }
    return NULL;
}

/* メソッドの解決。自クラス → MRO順の基底クラス。PythonのMROに従うため、
 * ダイヤモンド継承でも基底の選択がCPythonと一致する。 */
static P2C_MethodDef* p2c_find_method_in_chain(P2C_Object *cls_obj, const char *name) {
    if (!cls_obj || !name) return NULL;
    P2C_MethodDef *own = p2c_find_own_method(cls_obj, name);
    if (own) return own;
    const char *mro = p2c_class_mro(cls_obj);
    size_t offset = 0;
    P2C_NameSpan span;
    while (p2c_mro_next(mro, &offset, &span)) {
        P2C_Object *base_cls = p2c_find_class_by_span(&span);
        if (!base_cls) continue;
        P2C_MethodDef *found = p2c_find_own_method(base_cls, name);
        if (found) return found;
    }
    return NULL;
}

/* クラス本体の代入（メソッド以外のクラス属性）をMRO順に探す。
 * Python同様、サブクラスのインスタンスから基底クラスのクラス属性が見える。 */
static P2C_Object* p2c_class_attr_along_mro(P2C_Object *cls_obj, const char *name) {
    if (!cls_obj || !name || !cls_obj->cls || cls_obj->cls->type_tag != OBJ_CLASS) return NULL;
    if (cls_obj->u.v_class.attrs) {
        P2C_Object *val = attr_map_get(cls_obj->u.v_class.attrs, name);
        if (val) return val;
    }
    const char *mro = p2c_class_mro(cls_obj);
    size_t offset = 0;
    P2C_NameSpan span;
    while (p2c_mro_next(mro, &offset, &span)) {
        P2C_Object *base_cls = p2c_find_class_by_span(&span);
        if (!base_cls || !base_cls->u.v_class.attrs) continue;
        P2C_Object *val = attr_map_get(base_cls->u.v_class.attrs, name);
        if (val) return val;
    }
    return NULL;
}

/* obj.method を値として取り出したとき（m = obj.method、sorted(key=obj.key) など）
 * に返す「束縛メソッド」。呼び出し時は元のインスタンスをselfとしてメソッドへ
 * 転送する。環境辞書（selfとメソッド名）を持つクロージャとして実装するため、
 * 新しいOBJ型やGCルートを増やさずに済み、GCの走査対象
 * （関数オブジェクトのenv辞書）にもそのまま乗る。 */
static P2C_Object* p2c_bound_method_dispatch(P2C_Object *env, P2C_Object **args, size_t nargs) {
    if (!env || !env->cls || env->cls->type_tag != OBJ_DICT) {
        p2c_raise(p2c_make_exception("TypeError", "invalid bound method receiver"));
        return &P2C_None;
    }
    P2C_Object *bound_self = p2c_dict_get_with_default(env, p2c_obj_from_str("self"), &P2C_None);
    P2C_Object *bound_name = p2c_dict_get_with_default(env, p2c_obj_from_str("name"), &P2C_None);
    if (!bound_self || bound_self == &P2C_None || !p2c_obj_is_str(bound_name)) {
        p2c_raise(p2c_make_exception("TypeError", "invalid bound method"));
        return &P2C_None;
    }
    return p2c_call_attr(bound_self, p2c_obj_as_str(bound_name), args, nargs);
}

static P2C_Object* p2c_bound_method_new(P2C_Object *self_obj, const char *name) {
    P2C_Object *env = p2c_dict_new();
    if (!env) return &P2C_None;
    p2c_dict_set(env, p2c_obj_from_str("self"), self_obj);
    p2c_dict_set(env, p2c_obj_from_str("name"), p2c_obj_from_str(name));
    return p2c_closure_new(name, p2c_bound_method_dispatch, env);
}

/* super().method(...) の解決。Pythonでは「インスタンスの型のMRO上で、そのメソッドを
 * 定義しているクラスの次」から探索する。defining_class（メソッド本体を書いたクラス）
 * を受け取り、selfの実際の型のMROで defining_class の次から name を持つメソッドを
 * 探して self を束縛して呼ぶ。コード生成時に基底チェーンを静的に辿る近似と違い、
 * 多重継承のダイヤモンド（class D(B, C)、B(A)、C(A)）でもCPythonと同じ実装が選ばれる。 */
P2C_Object* p2c_super_call_attr(P2C_Object *self, P2C_Object *defining_class, const char *name, P2C_Object **args, size_t nargs) {
    if (!self || !self->cls || self->cls->type_tag != OBJ_INSTANCE || !name) {
        p2c_raise(p2c_make_exception("TypeError", "super() requires an instance method receiver"));
        return &P2C_None;
    }
    P2C_Object *klass = self->u.v_instance.klass;
    const char *def_name = (defining_class && defining_class->cls && defining_class->cls->type_tag == OBJ_CLASS)
        ? defining_class->u.v_class.name : NULL;
    if (!def_name) {
        /* 呼び出し元のクラスが分からない場合は、自クラスの次という情報が無いため、
         * インスタンスのクラス自身（＝最も自然な既定）から探索する。 */
        P2C_MethodDef *own = p2c_find_own_method(klass, name);
        if (own) return own->func(self, args, nargs);
    }
    const char *mro = p2c_class_mro(klass);
    size_t offset = 0;
    P2C_NameSpan span;
    /* MROリストは「自分自身を除く」並びなので、defining_classがインスタンスの
     * クラス自身なら先頭から、そうでなければMRO上でdefining_classの次から探索する
     * （Pythonの super() はメソッドを定義しているクラス自身を飛ばす）。 */
    bool skipping = def_name != NULL &&
        !(klass->u.v_class.name && strcmp(klass->u.v_class.name, def_name) == 0);
    while (p2c_mro_next(mro, &offset, &span)) {
        if (skipping) {
            if (p2c_name_span_equals(&span, def_name)) skipping = false;
            continue;
        }
        P2C_Object *base_cls = p2c_find_class_by_span(&span);
        if (!base_cls) continue;
        P2C_MethodDef *found = p2c_find_own_method(base_cls, name);
        if (found) return found->func(self, args, nargs);
    }
    p2c_raise(p2c_make_exception("AttributeError", name));
    return &P2C_None;
}

/* isinstance 用: クラス自身またはMRO上の基底クラス名に一致するか。
 * MRO文字列はC3線形化（失敗時は深さ優先の訪問順）なので、名前の一致判定は
 * 従来の再帰的な探索と同じか、より多くの基底名（未登録の基底名もMROに残る）を
 * 見る。Pythonのisinstanceの判定に近づく方向の変更で、結果は広がるのみ。 */
static bool p2c_class_chain_has_name(P2C_Object *cls_obj, const char *target_name) {
    if (!cls_obj || !target_name || !target_name[0]) return false;
    if (cls_obj->u.v_class.name && strcmp(cls_obj->u.v_class.name, target_name) == 0) return true;
    const char *mro = p2c_class_mro(cls_obj);
    size_t offset = 0;
    P2C_NameSpan span;
    while (p2c_mro_next(mro, &offset, &span)) {
        if (p2c_name_span_equals(&span, target_name)) return true;
    }
    return false;
}

bool p2c_isinstance_of_class(P2C_Object *obj, const char *class_name) {
    if (!obj || !obj->cls || !class_name) return false;
    if (strcmp(class_name, "object") == 0) return true;
    switch (obj->cls->type_tag) {
        case OBJ_NONE: return strcmp(class_name, "NoneType") == 0;
        case OBJ_BOOL: return strcmp(class_name, "bool") == 0 || strcmp(class_name, "int") == 0;
        case OBJ_INT: return strcmp(class_name, "int") == 0;
        case OBJ_FLOAT: return strcmp(class_name, "float") == 0;
        case OBJ_STR: return strcmp(class_name, "str") == 0;
        case OBJ_LIST: return strcmp(class_name, "list") == 0;
        case OBJ_DICT: return strcmp(class_name, "dict") == 0;
        case OBJ_SET: return strcmp(class_name, "set") == 0;
        case OBJ_TUPLE: return strcmp(class_name, "tuple") == 0;
        case OBJ_FUNCTION: return strcmp(class_name, "function") == 0;
        case OBJ_INSTANCE:
            return obj->u.v_instance.klass && p2c_class_chain_has_name(obj->u.v_instance.klass, class_name);
        default:
            return false;
    }
}

bool p2c_isinstance_of_object(P2C_Object *obj, P2C_Object *class_obj) {
    if (!class_obj || !class_obj->cls || class_obj->cls->type_tag != OBJ_CLASS) return false;
    /* クラスオブジェクトの同一性で判定できる場合はそれを使う。名前ベースの
     * 判定はフォールバックとして残す（組み込み型や名前で保持された継承関係用）。 */
    if (obj && obj->cls && obj->cls->type_tag == OBJ_INSTANCE && obj->u.v_instance.klass == class_obj) return true;
    return p2c_isinstance_of_class(obj, class_obj->u.v_class.name);
}

bool p2c_has_method(P2C_Object *obj, const char *name) {
    if (!obj || !obj->cls || obj->cls->type_tag != OBJ_INSTANCE || !obj->u.v_instance.klass || !name) return false;
    return p2c_find_method_in_chain(obj->u.v_instance.klass, name) != NULL;
}

P2C_Object* p2c_builtin_type(P2C_Object *obj) {
    const char *tn = "object";
    if (!obj || !obj->cls) tn = "NoneType";
    else switch (obj->cls->type_tag) {
        case OBJ_NONE: tn = "NoneType"; break;
        case OBJ_INT: tn = "int"; break;
        case OBJ_ELLIPSIS: tn = "ellipsis"; break;
        case OBJ_FLOAT: tn = "float"; break;
        case OBJ_STR: tn = "str"; break;
        case OBJ_BOOL: tn = "bool"; break;
        case OBJ_LIST: tn = "list"; break;
        case OBJ_TUPLE: tn = "tuple"; break;
        case OBJ_DICT: tn = "dict"; break;
        case OBJ_SET: tn = "set"; break;
        case OBJ_INSTANCE: tn = (obj->u.v_instance.klass && obj->u.v_instance.klass->u.v_class.name) ? obj->u.v_instance.klass->u.v_class.name : "object"; break;
        case OBJ_EXCEPTION: tn = obj->u.v_exception.type_name ? obj->u.v_exception.type_name : "Exception"; break;
        case OBJ_CLASS: tn = "type"; break;
        case OBJ_MODULE: tn = "module"; break;
        default: break;
    }
    /* 実際のtypeオブジェクトは表現していないため、print(type(x))で表示した際に
     * CPythonと同じ見た目 (<class 'int'> 等) になる文字列を返す近似実装。
     * type(x) == int のような型比較には対応していない。 */
    char buf[128];
    snprintf(buf, sizeof(buf), "<class '%s'>", tn);
    return p2c_obj_from_str(buf);
}

/* iter(obj): sequence(list/tuple/str/dict、および __len__+__getitem__ を
 * 実装するインスタンス)向けにはインデックスを進めていくイテレータを、
 * __iter__ を実装するインスタンス向けにはその戻り値をラップしたイテレータを
 * 作る。戻り値がすでにp2c_builtin_iterの返す反復子自身であればそのまま返す
 * （二重ラップを避ける）。 */
P2C_Object* p2c_builtin_iter(P2C_Object *obj) {
    if (!obj || !obj->cls) { p2c_raise(p2c_make_exception("TypeError", "object is not iterable")); return &P2C_None; }
    if (obj->cls->type_tag == OBJ_ITERATOR) return obj; /* 既に反復子ならそのまま */
    if (obj->cls->type_tag == OBJ_INSTANCE && p2c_has_method(obj, "__iter__")) {
        P2C_Object *result = p2c_call_attr(obj, "__iter__", NULL, 0);
        if (result && result->cls && result->cls->type_tag == OBJ_ITERATOR) return result;
        /* __iter__ が(自分自身を含め)独自の__next__実装を持つオブジェクトを
         * 返した場合、それをカスタムイテレータとしてラップする。 */
        P2C_Object *it = p2c_obj_new(&P2C_Class_Iterator);
        if (!it) return &P2C_None;
        it->u.v_iterator.seq = NULL;
        it->u.v_iterator.pos = 0;
        it->u.v_iterator.custom = result;
        return it;
    }
    switch (obj->cls->type_tag) {
        case OBJ_LIST: case OBJ_TUPLE: case OBJ_STR: case OBJ_DICT: case OBJ_SET: case OBJ_INSTANCE: {
            P2C_Object *it = p2c_obj_new(&P2C_Class_Iterator);
            if (!it) return &P2C_None;
            it->u.v_iterator.seq = obj;
            it->u.v_iterator.pos = 0;
            it->u.v_iterator.custom = NULL;
            return it;
        }
        default:
            p2c_raise(p2c_make_exception("TypeError", "object is not iterable"));
            return &P2C_None;
    }
}
/* next(it): 反復子を1つ進める。末尾に達したらStopIterationを送出する
 * （呼び出し側がtry/exceptで捕捉することを想定。for文自体は既存の通り
 * p2c_len+p2c_subscript_getベースで動くため、next()を直接使うのは
 * ジェネレータ的なコードを手動でイテレートするケース）。 */
P2C_Object* p2c_async_iter(P2C_Object *obj) {
    if (!obj || !p2c_has_method(obj, "__aiter__")) {
        p2c_raise(p2c_make_exception("TypeError", "object is not an asynchronous iterable"));
        return &P2C_None;
    }
    return p2c_call_attr(obj, "__aiter__", NULL, 0);
}

static P2C_Object* p2c_completed_coroutine_step(P2C_Object *generator) {
    return p2c_generator_finish(generator, p2c_generator_local_get(generator, "__p2c_completed_value"));
}

static P2C_Object* p2c_async_completed(P2C_Object *value, P2C_Object *exception) {
    P2C_Object *coroutine = p2c_generator_new(p2c_completed_coroutine_step, true);
    if (coroutine == &P2C_None) return coroutine;
    if (exception) {
        (void)p2c_generator_finish_exception(coroutine, exception);
    } else {
        p2c_generator_local_set(coroutine, "__p2c_completed_value", value);
        (void)p2c_generator_finish(coroutine, value);
    }
    return coroutine;
}

P2C_Object* p2c_async_call_attr(P2C_Object *obj, const char *name, P2C_Object **args, size_t nargs) {
    P2C_ExceptFrame frame;
    P2C_Object *volatile result = NULL;
    if (!obj || !name || !p2c_has_method(obj, name)) {
        p2c_raise(p2c_make_exception("TypeError", "object does not implement required asynchronous protocol method"));
        return &P2C_None;
    }
    frame.prev = p2c_exc_stack;
    frame.exc = NULL;
    p2c_exc_stack = &frame;
    if (P2C_SETJMP(frame.env) == 0) {
        result = p2c_call_attr(obj, name, args, nargs);
        p2c_exc_stack = frame.prev;
        if (result && result->cls && result->cls->type_tag == OBJ_ITERATOR && result->u.v_iterator.step && result->u.v_iterator.is_coroutine) return (P2C_Object*)result;
        return p2c_async_completed((P2C_Object*)result, NULL);
    }
    {
        P2C_Object *exception = frame.exc;
        p2c_exc_stack = frame.prev;
        return p2c_async_completed(NULL, exception);
    }
}

P2C_Object* p2c_async_next(P2C_Object *iter) {
    return p2c_async_call_attr(iter, "__anext__", NULL, 0);
}

P2C_Object* p2c_builtin_next(P2C_Object *it) {
    if (!it || !it->cls || it->cls->type_tag != OBJ_ITERATOR) {
        p2c_raise(p2c_make_exception("TypeError", "argument is not an iterator"));
        return &P2C_None;
    }
    if (it->u.v_iterator.step) {
        if (it->u.v_iterator.is_coroutine) {
            p2c_raise(p2c_make_exception("TypeError", "coroutine must be awaited"));
            return &P2C_None;
        }
        if (it->u.v_iterator.done) {
            p2c_raise(p2c_make_exception("StopIteration", ""));
            return &P2C_None;
        }
        if (it->u.v_iterator.running) {
            p2c_raise(p2c_make_exception("ValueError", "generator already executing"));
            return &P2C_None;
        }
        it->u.v_iterator.running = true;
        it->u.v_iterator.yielded = false;
        P2C_Object *value = it->u.v_iterator.step(it);
        it->u.v_iterator.running = false;
        if (it->u.v_iterator.done) {
            p2c_raise(p2c_make_exception("StopIteration", ""));
            return &P2C_None;
        }
        if (it->u.v_iterator.yielded) return value ? value : &P2C_None;
        p2c_raise(p2c_make_exception("RuntimeError", "generator step did not yield or finish"));
        return &P2C_None;
    }
    if (it->u.v_iterator.custom) {
        if (!p2c_has_method(it->u.v_iterator.custom, "__next__")) {
            p2c_raise(p2c_make_exception("TypeError", "iterator object has no __next__"));
            return &P2C_None;
        }
        return p2c_call_attr(it->u.v_iterator.custom, "__next__", NULL, 0);
    }
    P2C_Object *seq = it->u.v_iterator.seq;
    int64_t len = p2c_len(seq);
    if (it->u.v_iterator.pos >= len) {
        p2c_raise(p2c_make_exception("StopIteration", ""));
        return &P2C_None;
    }
    P2C_Object *val = p2c_iter_at(seq, it->u.v_iterator.pos);
    it->u.v_iterator.pos++;
    return val;
}

#ifndef P2C_ASYNC_QUEUE_CAPACITY
#define P2C_ASYNC_QUEUE_CAPACITY 256
#endif

static P2C_Object *g_async_queue[P2C_ASYNC_QUEUE_CAPACITY];
static size_t g_async_queue_head = 0;
static size_t g_async_queue_tail = 0;
static size_t g_async_queue_count = 0;

static void p2c_reset_async_queue(void) {
    memset(g_async_queue, 0, sizeof(g_async_queue));
    g_async_queue_head = 0;
    g_async_queue_tail = 0;
    g_async_queue_count = 0;
}

static bool p2c_is_generator_object(P2C_Object *obj) {
    return obj && obj->cls && obj->cls->type_tag == OBJ_ITERATOR && obj->u.v_iterator.step != NULL;
}

P2C_Object* p2c_generator_new(P2C_GeneratorStepFn step, bool is_coroutine) {
    if (!step) {
        p2c_raise(p2c_make_exception("TypeError", "generator step is required"));
        return &P2C_None;
    }
    P2C_Object *generator = p2c_obj_new(&P2C_Class_Iterator);
    if (!generator) return &P2C_None;
    generator->u.v_iterator.step = step;
    generator->u.v_iterator.locals = p2c_dict_new();
    generator->u.v_iterator.is_coroutine = is_coroutine;
    generator->u.v_iterator.state = 0;
    if (!generator->u.v_iterator.locals) {
        p2c_raise(p2c_make_exception("MemoryError", "could not allocate generator locals"));
        return &P2C_None;
    }
    return generator;
}

P2C_Object* p2c_generator_new_with_local(P2C_GeneratorStepFn step, bool is_coroutine, const char *name, P2C_Object *value) {
    P2C_Object *generator = p2c_generator_new(step, is_coroutine);
    if (generator != &P2C_None && name) p2c_generator_local_set(generator, name, value);
    return generator;
}

P2C_Object* p2c_generator_new_with_locals(P2C_GeneratorStepFn step, bool is_coroutine, const char * const *names, P2C_Object * const *values, size_t count) {
    P2C_Object *generator = p2c_generator_new(step, is_coroutine);
    if (generator == &P2C_None) return generator;
    for (size_t i = 0; i < count; i++) {
        if (names && names[i]) p2c_generator_local_set(generator, names[i], values ? values[i] : &P2C_None);
    }
    return generator;
}

uint32_t p2c_generator_state(P2C_Object *generator) {
    return p2c_is_generator_object(generator) ? generator->u.v_iterator.state : 0;
}

void p2c_generator_set_state(P2C_Object *generator, uint32_t state) {
    if (p2c_is_generator_object(generator)) generator->u.v_iterator.state = state;
}

P2C_Object* p2c_generator_local_get(P2C_Object *generator, const char *name) {
    if (!p2c_is_generator_object(generator) || !name) {
        p2c_raise(p2c_make_exception("TypeError", "invalid generator local access"));
        return &P2C_None;
    }
    return p2c_dict_get_with_default(generator->u.v_iterator.locals, p2c_obj_from_str(name), &P2C_None);
}

void p2c_generator_local_set(P2C_Object *generator, const char *name, P2C_Object *value) {
    if (!p2c_is_generator_object(generator) || !name) {
        p2c_raise(p2c_make_exception("TypeError", "invalid generator local assignment"));
        return;
    }
    p2c_dict_set(generator->u.v_iterator.locals, p2c_obj_from_str(name), value ? value : &P2C_None);
}

P2C_Object* p2c_generator_yield(P2C_Object *generator, P2C_Object *value, uint32_t next_state) {
    if (!p2c_is_generator_object(generator) || generator->u.v_iterator.is_coroutine) {
        p2c_raise(p2c_make_exception("TypeError", "yield is only valid in a generator"));
        return &P2C_None;
    }
    generator->u.v_iterator.state = next_state;
    generator->u.v_iterator.yielded = true;
    return value ? value : &P2C_None;
}

P2C_Object* p2c_generator_finish(P2C_Object *generator, P2C_Object *value) {
    if (!p2c_is_generator_object(generator)) {
        p2c_raise(p2c_make_exception("TypeError", "invalid generator completion"));
        return &P2C_None;
    }
    generator->u.v_iterator.result = value ? value : &P2C_None;
    generator->u.v_iterator.exception = NULL;
    generator->u.v_iterator.awaiting = NULL;
    generator->u.v_iterator.done = true;
    generator->u.v_iterator.yielded = false;
    return generator->u.v_iterator.result;
}

P2C_Object* p2c_generator_finish_exception(P2C_Object *generator, P2C_Object *exception) {
    if (!p2c_is_generator_object(generator)) {
        p2c_raise(p2c_make_exception("TypeError", "invalid generator exceptional completion"));
        return &P2C_None;
    }
    generator->u.v_iterator.result = &P2C_None;
    generator->u.v_iterator.exception = exception ? exception : p2c_make_exception("RuntimeError", "unknown coroutine error");
    generator->u.v_iterator.awaiting = NULL;
    generator->u.v_iterator.done = true;
    generator->u.v_iterator.yielded = false;
    return &P2C_None;
}

P2C_Object* p2c_generator_result(P2C_Object *generator) {
    if (!p2c_is_generator_object(generator)) {
        p2c_raise(p2c_make_exception("TypeError", "object is not a generator"));
        return &P2C_None;
    }
    return generator->u.v_iterator.result ? generator->u.v_iterator.result : &P2C_None;
}

P2C_Object* p2c_generator_await(P2C_Object *generator, P2C_Object *awaitable, uint32_t next_state) {
    if (!p2c_is_generator_object(generator) || !generator->u.v_iterator.is_coroutine) {
        p2c_raise(p2c_make_exception("TypeError", "await is only valid in a coroutine"));
        return &P2C_None;
    }
    if (!p2c_is_generator_object(awaitable) || !awaitable->u.v_iterator.is_coroutine) {
        p2c_raise(p2c_make_exception("TypeError", "object is not awaitable"));
        return &P2C_None;
    }
    generator->u.v_iterator.awaiting = awaitable;
    generator->u.v_iterator.state = next_state;
    return &P2C_None;
}

P2C_Object* p2c_generator_await_result(P2C_Object *generator) {
    if (!p2c_is_generator_object(generator) || !generator->u.v_iterator.awaiting || !generator->u.v_iterator.awaiting->u.v_iterator.done) {
        p2c_raise(p2c_make_exception("RuntimeError", "await result is not ready"));
        return &P2C_None;
    }
    P2C_Object *awaited = generator->u.v_iterator.awaiting;
    P2C_Object *result = p2c_generator_result(awaited);
    generator->u.v_iterator.awaiting = NULL;
    if (awaited->u.v_iterator.exception) p2c_raise(awaited->u.v_iterator.exception);
    return result;
}

bool p2c_generator_is_done(P2C_Object *generator) {
    return p2c_is_generator_object(generator) && generator->u.v_iterator.done;
}

void p2c_async_schedule(P2C_Object *coroutine) {
    if (!p2c_is_generator_object(coroutine) || !coroutine->u.v_iterator.is_coroutine) {
        p2c_raise(p2c_make_exception("TypeError", "object is not a coroutine"));
        return;
    }
    if (coroutine->u.v_iterator.done || coroutine->u.v_iterator.queued) return;
    if (g_async_queue_count == P2C_ASYNC_QUEUE_CAPACITY) {
        p2c_raise(p2c_make_exception("RuntimeError", "async queue capacity exceeded"));
        return;
    }
    p2c_obj_incref(coroutine);
    coroutine->u.v_iterator.queued = true;
    g_async_queue[g_async_queue_tail] = coroutine;
    g_async_queue_tail = (g_async_queue_tail + 1) % P2C_ASYNC_QUEUE_CAPACITY;
    g_async_queue_count++;
}

size_t p2c_async_pending_count(void) {
    return g_async_queue_count;
}

P2C_Object* p2c_async_run(P2C_Object *coroutine) {
    if (!p2c_is_generator_object(coroutine) || !coroutine->u.v_iterator.is_coroutine) {
        p2c_raise(p2c_make_exception("TypeError", "async_run expects a coroutine"));
        return &P2C_None;
    }
    p2c_obj_incref(coroutine);
    p2c_async_schedule(coroutine);
    while (!coroutine->u.v_iterator.done && g_async_queue_count > 0) {
        P2C_Object *current = g_async_queue[g_async_queue_head];
        g_async_queue[g_async_queue_head] = NULL;
        g_async_queue_head = (g_async_queue_head + 1) % P2C_ASYNC_QUEUE_CAPACITY;
        g_async_queue_count--;
        current->u.v_iterator.queued = false;
        if (!current->u.v_iterator.done) {
            P2C_Object *waiting = current->u.v_iterator.awaiting;
            if (waiting && !waiting->u.v_iterator.done) {
                p2c_async_schedule(waiting);
                p2c_async_schedule(current);
            } else {
                P2C_ExceptFrame frame;
                frame.prev = p2c_exc_stack;
                frame.exc = NULL;
                p2c_exc_stack = &frame;
                current->u.v_iterator.running = true;
                current->u.v_iterator.yielded = false;
                if (P2C_SETJMP(frame.env) == 0) {
                    (void)current->u.v_iterator.step(current);
                    p2c_exc_stack = frame.prev;
                } else {
                    P2C_Object *exception = frame.exc;
                    p2c_exc_stack = frame.prev;
                    p2c_generator_finish_exception(current, exception);
                }
                current->u.v_iterator.running = false;
                if (!current->u.v_iterator.done) {
                    if (current->u.v_iterator.awaiting) {
                        p2c_async_schedule(current->u.v_iterator.awaiting);
                        p2c_async_schedule(current);
                    } else {
                        p2c_raise(p2c_make_exception("RuntimeError", "coroutine step did not await or finish"));
                    }
                }
            }
        }
        p2c_obj_decref(current);
    }
    if (!coroutine->u.v_iterator.done) {
        p2c_obj_decref(coroutine);
        p2c_raise(p2c_make_exception("RuntimeError", "event loop stopped before coroutine completion"));
        return &P2C_None;
    }
    P2C_Object *result = p2c_generator_result(coroutine);
    P2C_Object *exception = coroutine->u.v_iterator.exception;
    p2c_obj_decref(coroutine);
    if (exception) p2c_raise(exception);
    return result;
}

static bool p2c_obj_index_value(P2C_Object *obj, int64_t *out) {
    if (p2c_obj_is_int(obj) || p2c_obj_is_bool(obj)) {
        *out = p2c_obj_as_int(obj);
        return true;
    }
    p2c_raise(p2c_make_exception("TypeError", "indices must be integers"));
    return false;
}

static bool p2c_normalize_search_bounds(size_t len, P2C_Object *start_obj, P2C_Object *stop_obj, int64_t *start_out, int64_t *stop_out) {
    int64_t start = 0;
    int64_t stop = (int64_t)len;
    if (start_obj && !p2c_obj_is_none(start_obj) && !p2c_obj_index_value(start_obj, &start)) return false;
    if (stop_obj && !p2c_obj_is_none(stop_obj) && !p2c_obj_index_value(stop_obj, &stop)) return false;
    if (start < 0) start += (int64_t)len;
    if (stop < 0) stop += (int64_t)len;
    if (start < 0) start = 0;
    if (stop < 0) stop = 0;
    if (start > (int64_t)len) start = (int64_t)len;
    if (stop > (int64_t)len) stop = (int64_t)len;
    *start_out = start;
    *stop_out = stop;
    return true;
}

static int64_t p2c_str_find_range(const char *text, int64_t start, int64_t stop, const char *needle, bool reverse) {
    size_t needle_len = strlen(needle);
    if (start > stop) return -1;
    if (needle_len == 0) return reverse ? stop : start;
    if (needle_len > (size_t)(stop - start)) return -1;
    if (!reverse) {
        for (int64_t i = start; i + (int64_t)needle_len <= stop; i++) {
            if (strncmp(text + i, needle, needle_len) == 0) return i;
        }
    } else {
        for (int64_t i = stop - (int64_t)needle_len;; i--) {
            if (strncmp(text + i, needle, needle_len) == 0) return i;
            if (i == start) break;
        }
    }
    return -1;
}

static bool p2c_str_edge_matches(const char *text, int64_t start, int64_t stop, P2C_Object *patterns, bool suffix) {
    if (p2c_obj_is_str(patterns)) {
        const char *pattern = p2c_obj_as_str(patterns);
        size_t pattern_len = strlen(pattern);
        if (start > stop || pattern_len > (size_t)(stop - start)) return false;
        return suffix ? strncmp(text + stop - (int64_t)pattern_len, pattern, pattern_len) == 0
                      : strncmp(text + start, pattern, pattern_len) == 0;
    }
    if (p2c_obj_is_tuple(patterns)) {
        for (size_t i = 0; i < patterns->u.v_tuple.len; i++) {
            P2C_Object *pattern = patterns->u.v_tuple.items[i];
            if (!p2c_obj_is_str(pattern)) {
                p2c_raise(p2c_make_exception("TypeError", "tuple for startswith/endswith must only contain str"));
                return false;
            }
            if (p2c_str_edge_matches(text, start, stop, pattern, suffix)) return true;
        }
        return false;
    }
    p2c_raise(p2c_make_exception("TypeError", "startswith/endswith first arg must be str or tuple of str"));
    return false;
}

P2C_Object* p2c_call_attr(P2C_Object *obj, const char *name, P2C_Object **args, size_t nargs) {
    if (obj && obj->cls && obj->cls->type_tag == OBJ_CLASS) {
        /* ClassName.method(self, ...) の形（明示的な基底クラスメソッド呼び出し、
         * 例: Shape.__init__(self, ...)）。この場合は obj 自体がクラスオブジェクトで、
         * args[0] が self に相当する。 */
        if (nargs >= 1) {
            P2C_MethodDef *found = p2c_find_method_in_chain(obj, name);
            if (found) return found->func(args[0], args + 1, nargs - 1);
        }
    }
    if (obj && obj->cls && obj->cls->type_tag == OBJ_INSTANCE && obj->u.v_instance.klass) {
        /* 自クラス、そこで見つからなければ基底クラス（複数可）を辿って
         * メソッドを探す（Python同様のメソッド解決順序: サブクラス→基底クラス）。 */
        P2C_MethodDef *found = p2c_find_method_in_chain(obj->u.v_instance.klass, name);
        if (found) return found->func(obj, args, nargs);
    }
    /* list / str の組み込みメソッド。
     * これらはP2C_Object上の属性としては存在しないため、
     * p2c_getattr経由のcallに任せると必ず失敗する（TypeError: not callable）。
     * ここで直接ディスパッチする。 */
    if (obj && obj->cls) {
        if (obj->cls->type_tag == OBJ_LIST) {
            if (strcmp(name, "append") == 0) { if (nargs >= 1) p2c_list_append(obj, args[0]); return &P2C_None; }
            if (strcmp(name, "pop") == 0) {
                if (nargs >= 1) {
                    int64_t idx = p2c_obj_as_int(args[0]);
                    int64_t len = (int64_t)obj->u.v_list.len;
                    if (idx < 0) idx += len;
                    if (idx < 0 || idx >= len) { p2c_raise(p2c_make_exception("IndexError", "pop index out of range")); return &P2C_None; }
                    P2C_Object *val = obj->u.v_list.items[idx];
                    for (int64_t i = idx; i < len - 1; i++) obj->u.v_list.items[i] = obj->u.v_list.items[i+1];
                    obj->u.v_list.len--;
                    return val;
                }
                return p2c_list_pop(obj);
            }
            if (strcmp(name, "insert") == 0 && nargs >= 2) {
                int64_t idx = p2c_obj_as_int(args[0]);
                int64_t len = (int64_t)obj->u.v_list.len;
                if (idx < 0) idx += len;
                if (idx < 0) idx = 0;
                if (idx > len) idx = len;
                p2c_list_append(obj, args[1]); /* 末尾に伸ばしてから後ろへずらす */
                for (int64_t i = (int64_t)obj->u.v_list.len - 1; i > idx; i--) obj->u.v_list.items[i] = obj->u.v_list.items[i-1];
                obj->u.v_list.items[idx] = args[1];
                return &P2C_None;
            }
            if (strcmp(name, "remove") == 0 && nargs >= 1) {
                for (size_t i = 0; i < obj->u.v_list.len; i++) {
                    if (p2c_obj_equal_raw(obj->u.v_list.items[i], args[0])) {
                        for (size_t j = i; j + 1 < obj->u.v_list.len; j++) obj->u.v_list.items[j] = obj->u.v_list.items[j+1];
                        obj->u.v_list.len--;
                        return &P2C_None;
                    }
                }
                p2c_raise(p2c_make_exception("ValueError", "list.remove(x): x not in list"));
                return &P2C_None;
            }
            if (strcmp(name, "count") == 0 && nargs >= 1) {
                int64_t c = 0;
                for (size_t i = 0; i < obj->u.v_list.len; i++) if (p2c_obj_equal_raw(obj->u.v_list.items[i], args[0])) c++;
                return p2c_obj_from_int(c);
            }
            if (strcmp(name, "index") == 0 && nargs >= 1) {
                int64_t len = (int64_t)obj->u.v_list.len;
                int64_t start = nargs >= 2 ? p2c_obj_as_int(args[1]) : 0;
                int64_t stop = nargs >= 3 ? p2c_obj_as_int(args[2]) : len;
                if (start < 0) start += len;
                if (stop < 0) stop += len;
                if (start < 0) start = 0;
                if (stop < 0) stop = 0;
                if (start > len) start = len;
                if (stop > len) stop = len;
                for (int64_t i = start; i < stop; i++) {
                    if (p2c_obj_equal_raw(obj->u.v_list.items[i], args[0])) return p2c_obj_from_int(i);
                }
                p2c_raise(p2c_make_exception("ValueError", "x not in list"));
                return &P2C_None;
            }
            if (strcmp(name, "extend") == 0 && nargs >= 1) {
                size_t n = 0; bool owned = false; P2C_Object **items = p2c_iter_items(args[0], &n, &owned);
                for (size_t i = 0; i < n; i++) p2c_list_append(obj, items[i]);
                if (owned) p2c_heap_free(items);
                return &P2C_None;
            }
            if (strcmp(name, "clear") == 0) { obj->u.v_list.len = 0; return &P2C_None; }
            if (strcmp(name, "copy") == 0 && nargs == 0) return p2c_builtin_list(obj);
            if (strcmp(name, "reverse") == 0) {
                size_t n = obj->u.v_list.len;
                for (size_t i = 0; i < n / 2; i++) { P2C_Object *t = obj->u.v_list.items[i]; obj->u.v_list.items[i] = obj->u.v_list.items[n-1-i]; obj->u.v_list.items[n-1-i] = t; }
                return &P2C_None;
            }
            if (strcmp(name, "sort") == 0) {
                P2C_Object *sorted = p2c_builtin_sorted(obj);
                for (size_t i = 0; i < obj->u.v_list.len; i++) obj->u.v_list.items[i] = sorted->u.v_list.items[i];
                return &P2C_None;
            }
        } else if (obj->cls->type_tag == OBJ_DICT) {
            if (strcmp(name, "get") == 0 && nargs >= 1) {
                if (!p2c_require_hashable(args[0])) return &P2C_None;
                /* p2c_dict_get はキーが無いとKeyErrorを送出するため、
                 * ここでは同じバケット探索を行い、見つからなければ
                 * （例外を出さずに）デフォルト値かNoneを返す。 */
                P2C_Object *found = NULL;
                if (obj->u.v_dict.bucket_count > 0) {
                    size_t h = p2c_obj_hash(args[0]) % obj->u.v_dict.bucket_count;
                    for (P2C_DictEntry *e = obj->u.v_dict.buckets[h]; e; e = e->next) {
                        if (p2c_obj_equal_raw(e->key, args[0])) { found = e->val; break; }
                    }
                }
                if (found) return found;
                return (nargs >= 2) ? args[1] : &P2C_None;
            }
            if (strcmp(name, "keys") == 0 || strcmp(name, "values") == 0 || strcmp(name, "items") == 0) {
                P2C_Object *out = p2c_list_new();
                bool want_keys = strcmp(name, "keys") == 0;
                bool want_items = strcmp(name, "items") == 0;
                for (P2C_DictEntry *e = obj->u.v_dict.order_head; e; e = e->order_next) {
                    if (want_items) {
                        P2C_Object *pair = p2c_obj_new(&P2C_Class_Tuple);
                        pair->u.v_tuple.items = (P2C_Object**)p2c_malloc_checked(2 * sizeof(P2C_Object*), "pair items");
                        pair->u.v_tuple.items[0] = e->key; pair->u.v_tuple.items[1] = e->val;
                        pair->u.v_tuple.len = 2;
                        p2c_list_append(out, pair);
                    } else {
                        p2c_list_append(out, want_keys ? e->key : e->val);
                    }
                }
                return out;
            }
            if (strcmp(name, "update") == 0 && nargs == 1) {
                P2C_Object *other = args[0];
                P2C_Object *mapping = (other && p2c_obj_is_dict(other)) ? other : p2c_builtin_dict(other);
                if (!mapping || !p2c_obj_is_dict(mapping)) {
                    p2c_raise(p2c_make_exception("TypeError", "update() requires a mapping or iterable of key/value pairs"));
                    return &P2C_None;
                }
                for (P2C_DictEntry *e = mapping->u.v_dict.order_head; e; e = e->order_next) p2c_dict_set(obj, e->key, e->val);
                return &P2C_None;
            }
            if (strcmp(name, "pop") == 0 && nargs >= 1) {
                if (!p2c_obj_is_hashable(args[0])) {
                    if (nargs >= 2) return args[1];
                    p2c_raise(p2c_make_exception("KeyError", "dictionary key not found"));
                    return &P2C_None;
                }
                /* d.pop(key[, default]): キーが存在すれば削除して値を返す。
                 * 無ければ default（指定されていればそれ、無ければKeyError）。 */
                P2C_Object *found = NULL;
                if (obj->u.v_dict.bucket_count > 0) {
                    size_t h = p2c_obj_hash(args[0]) % obj->u.v_dict.bucket_count;
                    for (P2C_DictEntry *e = obj->u.v_dict.buckets[h]; e; e = e->next) {
                        if (p2c_obj_equal_raw(e->key, args[0])) { found = e->val; break; }
                    }
                }
                if (found) {
                    p2c_dict_remove(obj, args[0]);
                    return found;
                }
                if (nargs >= 2) return args[1];
                p2c_raise(p2c_make_exception("KeyError", "dictionary key not found"));
                return &P2C_None;
            }
            if (strcmp(name, "setdefault") == 0 && nargs >= 1) {
                if (!p2c_require_hashable(args[0])) return &P2C_None;
                /* d.setdefault(key[, default]): キーがあればその値、
                 * 無ければ default（省略時None）を新規に設定して返す。 */
                P2C_Object *existing = NULL;
                if (obj->u.v_dict.bucket_count > 0) {
                    size_t h = p2c_obj_hash(args[0]) % obj->u.v_dict.bucket_count;
                    for (P2C_DictEntry *e = obj->u.v_dict.buckets[h]; e; e = e->next) {
                        if (p2c_obj_equal_raw(e->key, args[0])) { existing = e->val; break; }
                    }
                }
                if (existing) return existing;
                P2C_Object *def_val = (nargs >= 2) ? args[1] : &P2C_None;
                p2c_dict_set(obj, args[0], def_val);
                return def_val;
            }
            if (strcmp(name, "clear") == 0) {
                for (size_t i = 0; i < obj->u.v_dict.bucket_count; i++) {
                    P2C_DictEntry *e = obj->u.v_dict.buckets[i];
                    while (e) { P2C_DictEntry *next = e->next; p2c_heap_free(e); e = next; }
                    obj->u.v_dict.buckets[i] = NULL;
                }
                obj->u.v_dict.order_head = NULL;
                obj->u.v_dict.order_tail = NULL;
                obj->u.v_dict.len = 0;
                return &P2C_None;
            }
            if (strcmp(name, "copy") == 0 && nargs == 0) {
                P2C_Object *out = p2c_dict_new();
                for (P2C_DictEntry *entry = obj->u.v_dict.order_head; entry; entry = entry->order_next) p2c_dict_set(out, entry->key, entry->val);
                return out;
            }
            if (strcmp(name, "popitem") == 0 && nargs == 0) {
                P2C_DictEntry *entry = obj->u.v_dict.order_tail;
                if (!entry) { p2c_raise(p2c_make_exception("KeyError", "popitem(): dictionary is empty")); return &P2C_None; }
                P2C_Object *pair = p2c_obj_new(&P2C_Class_Tuple);
                if (!pair) return &P2C_None;
                pair->u.v_tuple.items = (P2C_Object**)p2c_malloc_checked(2 * sizeof(P2C_Object*), "pair items");
                if (!pair->u.v_tuple.items) return &P2C_None;
                pair->u.v_tuple.items[0] = entry->key;
                pair->u.v_tuple.items[1] = entry->val;
                pair->u.v_tuple.len = 2;
                p2c_dict_remove(obj, entry->key);
                return pair;
            }
        } else if (obj->cls->type_tag == OBJ_SET) {
            if (strcmp(name, "add") == 0 && nargs == 1) {
                p2c_set_add(obj, args[0]);
                return &P2C_None;
            }
            if (strcmp(name, "discard") == 0 && nargs == 1) {
                p2c_set_remove_item(obj, args[0]);
                return &P2C_None;
            }
            if (strcmp(name, "remove") == 0 && nargs == 1) {
                if (p2c_set_remove_item(obj, args[0])) return &P2C_None;
                p2c_raise(p2c_make_exception("KeyError", "set element not found"));
                return &P2C_None;
            }
            if (strcmp(name, "clear") == 0 && nargs == 0) {
                p2c_set_clear_items(obj);
                return &P2C_None;
            }
            if (strcmp(name, "copy") == 0 && nargs == 0) return p2c_builtin_set(obj);
            if (strcmp(name, "union") == 0) {
                P2C_Object *out = p2c_builtin_set(obj);
                for (size_t i = 0; i < nargs; i++) out = p2c_obj_bitor(out, p2c_builtin_set(args[i]));
                return out;
            }
            if (strcmp(name, "intersection") == 0) {
                P2C_Object *out = p2c_builtin_set(obj);
                for (size_t i = 0; i < nargs; i++) out = p2c_obj_bitand(out, p2c_builtin_set(args[i]));
                return out;
            }
            if (strcmp(name, "difference") == 0) {
                P2C_Object *out = p2c_builtin_set(obj);
                for (size_t i = 0; i < nargs; i++) out = p2c_obj_sub(out, p2c_builtin_set(args[i]));
                return out;
            }
            if (strcmp(name, "symmetric_difference") == 0 && nargs == 1) return p2c_obj_bitxor(obj, p2c_builtin_set(args[0]));
            if (strcmp(name, "update") == 0 || strcmp(name, "intersection_update") == 0 || strcmp(name, "difference_update") == 0 || strcmp(name, "symmetric_difference_update") == 0) {
                P2C_Object *out = p2c_builtin_set(obj);
                for (size_t i = 0; i < nargs; i++) {
                    P2C_Object *other = p2c_builtin_set(args[i]);
                    if (strcmp(name, "update") == 0) out = p2c_obj_bitor(out, other);
                    else if (strcmp(name, "intersection_update") == 0) out = p2c_obj_bitand(out, other);
                    else if (strcmp(name, "difference_update") == 0) out = p2c_obj_sub(out, other);
                    else out = p2c_obj_bitxor(out, other);
                }
                p2c_set_clear_items(obj);
                for (P2C_DictEntry *entry = out->u.v_dict.order_head; entry; entry = entry->order_next) p2c_set_add(obj, entry->key);
                return &P2C_None;
            }
            if (strcmp(name, "isdisjoint") == 0 && nargs == 1) {
                P2C_Object *other = p2c_builtin_set(args[0]);
                for (P2C_DictEntry *entry = obj->u.v_dict.order_head; entry; entry = entry->order_next) if (p2c_obj_is_truthy(p2c_obj_contains(other, entry->key))) return &P2C_False;
                return &P2C_True;
            }
            if (strcmp(name, "issubset") == 0 && nargs == 1) {
                P2C_Object *other = p2c_builtin_set(args[0]);
                for (P2C_DictEntry *entry = obj->u.v_dict.order_head; entry; entry = entry->order_next) if (!p2c_obj_is_truthy(p2c_obj_contains(other, entry->key))) return &P2C_False;
                return &P2C_True;
            }
            if (strcmp(name, "issuperset") == 0 && nargs == 1) {
                P2C_Object *other = p2c_builtin_set(args[0]);
                for (P2C_DictEntry *entry = other->u.v_dict.order_head; entry; entry = entry->order_next) if (!p2c_obj_is_truthy(p2c_obj_contains(obj, entry->key))) return &P2C_False;
                return &P2C_True;
            }
            if (strcmp(name, "pop") == 0 && nargs == 0) {
                P2C_DictEntry *entry = obj->u.v_dict.order_head;
                P2C_Object *item;
                if (!entry) { p2c_raise(p2c_make_exception("KeyError", "pop from an empty set")); return &P2C_None; }
                item = entry->key;
                p2c_set_remove_item(obj, item);
                return item;
            }
        } else if (obj->cls->type_tag == OBJ_STR) {
            const char *s = p2c_obj_as_str(obj);
            if (strcmp(name, "upper") == 0 || strcmp(name, "lower") == 0 || strcmp(name, "casefold") == 0) {
                size_t len = strlen(s);
                char *buf = (char*)p2c_malloc_checked(len + 1, "string copy");
                if (!buf) return &P2C_None;
                for (size_t i = 0; i < len; i++) buf[i] = (strcmp(name, "upper") == 0) ? (char)toupper((unsigned char)s[i]) : (char)tolower((unsigned char)s[i]);
                buf[len] = '\0';
                P2C_Object *out = p2c_obj_from_str(buf);
                p2c_heap_free(buf);
                return out;
            }
            if (strcmp(name, "capitalize") == 0 || strcmp(name, "swapcase") == 0) {
                size_t len = strlen(s);
                char *buf = (char*)p2c_malloc_checked(len + 1, "string copy");
                if (!buf) return &P2C_None;
                for (size_t i = 0; i < len; i++) {
                    unsigned char ch = (unsigned char)s[i];
                    if (strcmp(name, "capitalize") == 0) buf[i] = (i == 0) ? (char)toupper(ch) : (char)tolower(ch);
                    else if (ch >= (unsigned char)'a' && ch <= (unsigned char)'z') buf[i] = (char)toupper(ch);
                    else if (ch >= (unsigned char)'A' && ch <= (unsigned char)'Z') buf[i] = (char)tolower(ch);
                    else buf[i] = (char)ch;
                }
                buf[len] = '\0';
                P2C_Object *out = p2c_obj_from_str(buf);
                p2c_heap_free(buf);
                return out;
            }
            if (strcmp(name, "strip") == 0 && nargs == 0) {
                size_t len = strlen(s);
                size_t start = 0, end = len;
                while (start < end && isspace((unsigned char)s[start])) start++;
                while (end > start && isspace((unsigned char)s[end - 1])) end--;
                char *buf = (char*)p2c_malloc_checked(end - start + 1, "string slice");
                if (!buf) return &P2C_None;
                memcpy(buf, s + start, end - start);
                buf[end - start] = '\0';
                P2C_Object *out = p2c_obj_from_str(buf);
                p2c_heap_free(buf);
                return out;
            }
            if (strcmp(name, "split") == 0) {
                const char *sep = (nargs >= 1 && args[0] != &P2C_None) ? p2c_obj_as_str(args[0]) : NULL;
                int64_t maxsplit = nargs >= 2 ? p2c_obj_as_int(args[1]) : -1;
                P2C_Object *out = p2c_list_new();
                size_t len = strlen(s);
                if (!sep) {
                    size_t i = 0;
                    while (i < len && isspace((unsigned char)s[i])) i++;
                    if (i >= len) return out;
                    if (maxsplit == 0) {
                        p2c_list_append(out, p2c_obj_from_str_n(s + i, len - i));
                        return out;
                    }
                    int64_t splits = 0;
                    while (i < len) {
                        size_t start = i;
                        while (i < len && !isspace((unsigned char)s[i])) i++;
                        p2c_list_append(out, p2c_obj_from_str_n(s + start, i - start));
                        while (i < len && isspace((unsigned char)s[i])) i++;
                        if (i >= len) break;
                        splits++;
                        if (maxsplit >= 0 && splits >= maxsplit) {
                            p2c_list_append(out, p2c_obj_from_str_n(s + i, len - i));
                            break;
                        }
                    }
                } else {
                    size_t seplen = strlen(sep);
                    if (seplen == 0) { p2c_raise(p2c_make_exception("ValueError", "empty separator")); return &P2C_None; }
                    size_t start = 0, pos = 0;
                    int64_t splits = 0;
                    while (pos + seplen <= len && (maxsplit < 0 || splits < maxsplit)) {
                        if (strncmp(s + pos, sep, seplen) == 0) {
                            p2c_list_append(out, p2c_obj_from_str_n(s + start, pos - start));
                            pos += seplen;
                            start = pos;
                            splits++;
                        } else {
                            pos++;
                        }
                    }
                    p2c_list_append(out, p2c_obj_from_str_n(s + start, len - start));
                }
                return out;
            }
            if (strcmp(name, "join") == 0 && nargs >= 1) {
                size_t n = 0; bool owned = false; P2C_Object **items = p2c_iter_items(args[0], &n, &owned);
                P2C_String *buf = p2c_str_new(NULL);
                for (size_t i = 0; i < n; i++) { if (i) p2c_str_append(buf, s); p2c_str_append(buf, p2c_obj_as_str(items[i])); }
                P2C_Object *out = p2c_obj_from_str(p2c_str_cstr(buf));
                p2c_str_free(buf);
                if (owned) p2c_heap_free(items);
                return out;
            }
            if (strcmp(name, "replace") == 0 && nargs >= 2) {
                const char *old = p2c_obj_as_str(args[0]);
                const char *rep = p2c_obj_as_str(args[1]);
                size_t oldlen = strlen(old), len = strlen(s);
                int64_t maxcount = nargs >= 3 ? p2c_obj_as_int(args[2]) : -1;
                int64_t replaced = 0;
                P2C_String *buf = p2c_str_new(NULL);
                if (oldlen == 0) {
                    for (size_t i = 0; i <= len; i++) {
                        if (maxcount < 0 || replaced < maxcount) {
                            p2c_str_append(buf, rep);
                            replaced++;
                        }
                        if (i < len) {
                            char c1[2] = { s[i], '\0' };
                            p2c_str_append(buf, c1);
                        }
                    }
                } else {
                    for (size_t i = 0; i < len; ) {
                        if (i + oldlen <= len && strncmp(s + i, old, oldlen) == 0 && (maxcount < 0 || replaced < maxcount)) {
                            p2c_str_append(buf, rep);
                            i += oldlen;
                            replaced++;
                        } else {
                            char c1[2] = { s[i], '\0' };
                            p2c_str_append(buf, c1);
                            i++;
                        }
                    }
                }
                P2C_Object *out = p2c_obj_from_str(p2c_str_cstr(buf));
                p2c_str_free(buf);
                return out;
            }
            if ((strcmp(name, "partition") == 0 || strcmp(name, "rpartition") == 0) && nargs == 1) {
                const char *sep = p2c_obj_as_str(args[0]);
                size_t slen = strlen(s), seplen = strlen(sep);
                bool reverse = strcmp(name, "rpartition") == 0;
                P2C_Object *items[3];
                if (seplen == 0) {
                    p2c_raise(p2c_make_exception("ValueError", "empty separator"));
                    return &P2C_None;
                }
                int64_t found = p2c_str_find_range(s, 0, (int64_t)slen, sep, reverse);
                if (found < 0) {
                    items[0] = reverse ? p2c_obj_from_str("") : p2c_obj_from_str(s);
                    items[1] = p2c_obj_from_str("");
                    items[2] = reverse ? p2c_obj_from_str(s) : p2c_obj_from_str("");
                } else {
                    items[0] = p2c_obj_from_str_n(s, (size_t)found);
                    items[1] = p2c_obj_from_str_n(s + found, seplen);
                    items[2] = p2c_obj_from_str(s + found + (int64_t)seplen);
                }
                return p2c_tuple_from_array(items, 3);
            }
            if ((strcmp(name, "find") == 0 || strcmp(name, "index") == 0 || strcmp(name, "rfind") == 0 || strcmp(name, "rindex") == 0) && nargs >= 1) {
                const char *needle = p2c_obj_as_str(args[0]);
                int64_t start, stop;
                bool reverse = strcmp(name, "rfind") == 0 || strcmp(name, "rindex") == 0;
                bool raises_if_missing = strcmp(name, "index") == 0 || strcmp(name, "rindex") == 0;
                P2C_Object *start_obj = nargs >= 2 ? args[1] : NULL;
                P2C_Object *stop_obj = nargs >= 3 ? args[2] : NULL;
                if (!p2c_normalize_search_bounds(strlen(s), start_obj, stop_obj, &start, &stop)) return &P2C_None;
                int64_t found = p2c_str_find_range(s, start, stop, needle, reverse);
                if (found < 0 && raises_if_missing) {
                    p2c_raise(p2c_make_exception("ValueError", "substring not found"));
                    return &P2C_None;
                }
                return p2c_obj_from_int(found);
            }
            if ((strcmp(name, "startswith") == 0 || strcmp(name, "endswith") == 0) && nargs >= 1) {
                int64_t start, stop;
                P2C_Object *start_obj = nargs >= 2 ? args[1] : NULL;
                P2C_Object *stop_obj = nargs >= 3 ? args[2] : NULL;
                if (!p2c_normalize_search_bounds(strlen(s), start_obj, stop_obj, &start, &stop)) return &P2C_None;
                return p2c_obj_from_bool(p2c_str_edge_matches(s, start, stop, args[0], strcmp(name, "endswith") == 0));
            }
            if (strcmp(name, "removeprefix") == 0 && nargs == 1) {
                const char *pre = p2c_obj_as_str(args[0]);
                size_t plen = strlen(pre);
                return (strncmp(s, pre, plen) == 0) ? p2c_obj_from_str(s + plen) : p2c_obj_from_str(s);
            }
            if (strcmp(name, "removesuffix") == 0 && nargs == 1) {
                const char *suf = p2c_obj_as_str(args[0]);
                size_t slen = strlen(s), suflen = strlen(suf);
                return (suflen <= slen && strcmp(s + slen - suflen, suf) == 0) ? p2c_obj_from_str_n(s, slen - suflen) : p2c_obj_from_str(s);
            }
            /* str.format(*args, **kwargs) - {} / {0} / {name} 置換 */
            if (strcmp(name, "format") == 0) {
                return p2c_str_format_py(s, args, nargs);
            }
            /* str.title() */
            if (strcmp(name, "title") == 0) {
                size_t slen = strlen(s);
                char *buf = (char*)p2c_malloc_checked(slen + 1, "string replace"); if (!buf) return obj;
                bool cap_next = true;
                for (size_t i = 0; i < slen; i++) {
                    unsigned char c = (unsigned char)s[i];
                    if (c == ' ' || c == '\t' || c == '\n') { buf[i] = (char)c; cap_next = true; }
                    else if (cap_next) { buf[i] = (char)toupper(c); cap_next = false; }
                    else { buf[i] = (char)tolower(c); }
                }
                buf[slen] = '\0';
                P2C_Object *r = p2c_obj_from_str(buf); p2c_heap_free(buf); return r;
            }
            /* str.center(width[, fill]) */
            if (strcmp(name, "center") == 0 && nargs >= 1) {
                int64_t w = p2c_obj_as_int(args[0]);
                char fill = (nargs >= 2) ? p2c_obj_as_str(args[1])[0] : ' ';
                size_t slen = strlen(s);
                if ((size_t)w <= slen) return obj;
                size_t pad = (size_t)w - slen;
                size_t lpad = pad / 2, rpad = pad - lpad;
                char *buf = (char*)p2c_malloc_checked((size_t)w + 1, "string pad"); if (!buf) return obj;
                for (size_t i = 0; i < lpad; i++) buf[i] = fill;
                memcpy(buf + lpad, s, slen);
                for (size_t i = 0; i < rpad; i++) buf[lpad + slen + i] = fill;
                buf[w] = '\0';
                P2C_Object *r = p2c_obj_from_str(buf); p2c_heap_free(buf); return r;
            }
            /* str.ljust(width[, fill]) */
            if (strcmp(name, "ljust") == 0 && nargs >= 1) {
                int64_t w = p2c_obj_as_int(args[0]);
                char fill = (nargs >= 2) ? p2c_obj_as_str(args[1])[0] : ' ';
                size_t slen = strlen(s);
                if ((size_t)w <= slen) return obj;
                char *buf = (char*)p2c_malloc_checked((size_t)w + 1, "string pad"); if (!buf) return obj;
                memcpy(buf, s, slen);
                for (size_t i = slen; i < (size_t)w; i++) buf[i] = fill;
                buf[w] = '\0';
                P2C_Object *r = p2c_obj_from_str(buf); p2c_heap_free(buf); return r;
            }
            /* str.rjust(width[, fill]) */
            if (strcmp(name, "rjust") == 0 && nargs >= 1) {
                int64_t w = p2c_obj_as_int(args[0]);
                char fill = (nargs >= 2) ? p2c_obj_as_str(args[1])[0] : ' ';
                size_t slen = strlen(s);
                if ((size_t)w <= slen) return obj;
                size_t pad = (size_t)w - slen;
                char *buf = (char*)p2c_malloc_checked((size_t)w + 1, "string pad"); if (!buf) return obj;
                for (size_t i = 0; i < pad; i++) buf[i] = fill;
                memcpy(buf + pad, s, slen); buf[w] = '\0';
                P2C_Object *r = p2c_obj_from_str(buf); p2c_heap_free(buf); return r;
            }
            /* str.zfill(width) */
            if (strcmp(name, "zfill") == 0 && nargs >= 1) {
                int64_t w = p2c_obj_as_int(args[0]);
                size_t slen = strlen(s);
                if ((size_t)w <= slen) return obj;
                size_t pad = (size_t)w - slen;
                char *buf = (char*)p2c_malloc_checked((size_t)w + 1, "string pad"); if (!buf) return obj;
                size_t off = 0;
                if (slen > 0 && (s[0] == '+' || s[0] == '-')) { buf[off++] = s[0]; }
                for (size_t i = 0; i < pad; i++) buf[off + i] = '0';
                memcpy(buf + off + pad, s + off, slen - off); buf[w] = '\0';
                P2C_Object *r = p2c_obj_from_str(buf); p2c_heap_free(buf); return r;
            }
            if (strcmp(name, "count") == 0 && nargs >= 1) {
                const char *sub = p2c_obj_as_str(args[0]);
                size_t sublen = strlen(sub);
                int64_t start, stop, count = 0;
                P2C_Object *start_obj = nargs >= 2 ? args[1] : NULL;
                P2C_Object *stop_obj = nargs >= 3 ? args[2] : NULL;
                if (!p2c_normalize_search_bounds(strlen(s), start_obj, stop_obj, &start, &stop)) return &P2C_None;
                if (sublen == 0) return p2c_obj_from_int(stop >= start ? stop - start + 1 : 0);
                for (int64_t pos = start; pos + (int64_t)sublen <= stop; ) {
                    if (strncmp(s + pos, sub, sublen) == 0) {
                        count++;
                        pos += (int64_t)sublen;
                    } else {
                        pos++;
                    }
                }
                return p2c_obj_from_int(count);
            }
            if (strcmp(name, "expandtabs") == 0 && nargs <= 1) {
                int64_t tabsize = nargs == 1 ? p2c_obj_as_int(args[0]) : 8;
                if (tabsize < 0) tabsize = 0;
                P2C_String *buf = p2c_str_new(NULL);
                int64_t column = 0;
                for (const char *c = s; c && *c; c++) {
                    if (*c == '\t') {
                        int64_t spaces = tabsize == 0 ? 0 : tabsize - (column % tabsize);
                        for (int64_t i = 0; i < spaces; i++) p2c_str_append(buf, " ");
                        column += spaces;
                    } else {
                        char one[2] = { *c, '\0' };
                        p2c_str_append(buf, one);
                        if (*c == '\n' || *c == '\r') column = 0;
                        else column++;
                    }
                }
                P2C_Object *out = p2c_obj_from_str(p2c_str_cstr(buf));
                p2c_str_free(buf);
                return out;
            }
            if (strcmp(name, "splitlines") == 0 && nargs <= 1) {
                bool keepends = nargs == 1 && p2c_obj_is_truthy(args[0]);
                size_t slen = strlen(s), start = 0, pos = 0;
                P2C_Object *out = p2c_list_new();
                while (pos < slen) {
                    size_t line_end;
                    if (s[pos] == '\r' && pos + 1 < slen && s[pos + 1] == '\n') {
                        line_end = pos + 2;
                    } else if (s[pos] == '\n' || s[pos] == '\r' || s[pos] == '\v' || s[pos] == '\f') {
                        line_end = pos + 1;
                    } else {
                        pos++;
                        continue;
                    }
                    p2c_list_append(out, p2c_obj_from_str_n(s + start, (keepends ? line_end : pos) - start));
                    start = line_end;
                    pos = line_end;
                }
                if (start < slen) p2c_list_append(out, p2c_obj_from_str_n(s + start, slen - start));
                return out;
            }
            if ((strcmp(name, "isalpha") == 0 || strcmp(name, "isdigit") == 0 || strcmp(name, "isalnum") == 0 ||
                 strcmp(name, "isspace") == 0 || strcmp(name, "islower") == 0 || strcmp(name, "isupper") == 0 ||
                 strcmp(name, "isidentifier") == 0 || strcmp(name, "isascii") == 0 ||
                 strcmp(name, "isprintable") == 0) && nargs == 0) {
                size_t slen = strlen(s);
                bool result = slen > 0;
                bool seen_cased = false;
                if (strcmp(name, "isascii") == 0) {
                    /* CPythonでは空文字列もTrue（ASCIIのみで構成されている）。 */
                    result = true;
                    for (size_t i = 0; i < slen; i++) {
                        if ((unsigned char)s[i] > 0x7fu) { result = false; break; }
                    }
                } else if (strcmp(name, "isprintable") == 0) {
                    /* ASCII制御文字・DELは非表示。非ASCII(UTF-8バイト)は表示可能側
                     * として扱う（他の文字種判定と同じASCII近似）。 */
                    result = true;
                    for (size_t i = 0; i < slen; i++) {
                        unsigned char c = (unsigned char)s[i];
                        if (c < 0x20u || c == 0x7fu) { result = false; break; }
                    }
                } else if (strcmp(name, "isidentifier") == 0) {
                    result = slen > 0 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_');
                    for (size_t i = 1; result && i < slen; i++) {
                        unsigned char c = (unsigned char)s[i];
                        result = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                    }
                } else {
                    for (size_t i = 0; result && i < slen; i++) {
                        unsigned char c = (unsigned char)s[i];
                        bool lower = c >= 'a' && c <= 'z';
                        bool upper = c >= 'A' && c <= 'Z';
                        bool digit = c >= '0' && c <= '9';
                        if (strcmp(name, "isalpha") == 0) result = lower || upper;
                        else if (strcmp(name, "isdigit") == 0) result = digit;
                        else if (strcmp(name, "isalnum") == 0) result = lower || upper || digit;
                        else if (strcmp(name, "isspace") == 0) result = isspace(c) != 0;
                        else if (strcmp(name, "islower") == 0) { if (upper) result = false; if (lower) seen_cased = true; }
                        else if (strcmp(name, "isupper") == 0) { if (lower) result = false; if (upper) seen_cased = true; }
                    }
                    if (strcmp(name, "islower") == 0 || strcmp(name, "isupper") == 0) result = result && seen_cased;
                }
                return p2c_obj_from_bool(result);
            }
            /* str.strip([chars]) / str.lstrip([chars]) / str.rstrip([chars]) */
            if (strcmp(name, "strip") == 0 || strcmp(name, "lstrip") == 0 || strcmp(name, "rstrip") == 0) {
                const char *chars = (nargs >= 1 && !p2c_obj_is_none(args[0])) ? p2c_obj_as_str(args[0]) : " \t\n\r\v\f";
                size_t slen = strlen(s), start = 0, end = slen;
                if (nargs > 1) {
                    p2c_raise(p2c_make_exception("TypeError", "strip expects at most one argument"));
                    return &P2C_None;
                }
                if (strcmp(name, "rstrip") != 0) while (start < end && strchr(chars, s[start])) start++;
                if (strcmp(name, "lstrip") != 0) while (end > start && strchr(chars, s[end - 1])) end--;
                return p2c_obj_from_str_n(s + start, end - start);
            }
        }
    }
    return p2c_call(p2c_getattr(obj, name), args, nargs);
}

P2C_Object* p2c_call_attr_kw(P2C_Object *obj, const char *name, P2C_Object **args, size_t nargs,
                              const char **kw_names, P2C_Object **kw_values, size_t nkw) {
    size_t flat_n = 0, pos = 0;
    const char **flat_names = NULL;
    P2C_Object **flat_values = NULL;
    bool owns_flat = false;
    if (nkw == 0) return p2c_call_attr(obj, name, args, nargs);
    /* kw_names[i] == NULL はコード生成側の **mapping を表す。dictを展開して
     * adapterが通常の名前付き引数と同じ経路で束縛できるようにする。 */
    for (size_t i = 0; i < nkw; i++) {
        if (kw_names[i]) flat_n++;
        else {
            P2C_Object *mapping = kw_values[i];
            if (!mapping || !mapping->cls || mapping->cls->type_tag != OBJ_DICT) {
                p2c_raise(p2c_make_exception("TypeError", "method ** argument must be a dict"));
                return &P2C_None;
            }
            flat_n += mapping->u.v_dict.len;
        }
    }
    if (flat_n != nkw || (nkw && !kw_names[0])) {
        flat_names = flat_n ? (const char**)p2c_malloc_checked(flat_n * sizeof(const char*), "call plan names") : NULL;
        flat_values = flat_n ? (P2C_Object**)p2c_malloc_checked(flat_n * sizeof(P2C_Object*), "call plan values") : NULL;
        if (flat_n && (!flat_names || !flat_values)) {
            p2c_heap_free(flat_names); p2c_heap_free(flat_values);
            p2c_raise(p2c_make_exception("MemoryError", "keyword argument expansion failed"));
            return &P2C_None;
        }
        owns_flat = true;
        for (size_t i = 0; i < nkw; i++) {
            if (kw_names[i]) { flat_names[pos] = kw_names[i]; flat_values[pos++] = kw_values[i]; }
            else for (P2C_DictEntry *e = kw_values[i]->u.v_dict.order_head; e; e = e->order_next) {
                if (!e->key || !e->key->cls || e->key->cls->type_tag != OBJ_STR) {
                    p2c_heap_free(flat_names); p2c_heap_free(flat_values);
                    p2c_raise(p2c_make_exception("TypeError", "method ** keys must be strings"));
                    return &P2C_None;
                }
                flat_names[pos] = p2c_obj_as_str(e->key); flat_values[pos++] = e->val;
            }
        }
        kw_names = flat_names; kw_values = flat_values; nkw = flat_n;
    }
    if (obj && obj->cls && obj->cls->type_tag == OBJ_INSTANCE) {
        P2C_MethodDef *found = p2c_find_method_in_chain(obj->u.v_instance.klass, name);
        if (found && found->kwfunc) {
            P2C_Object *result = found->kwfunc(obj, args, nargs, kw_names, kw_values, nkw);
            if (owns_flat) { p2c_heap_free(flat_names); p2c_heap_free(flat_values); }
            return result;
        }
    }
    if (owns_flat) { p2c_heap_free(flat_names); p2c_heap_free(flat_values); }
    p2c_raise(p2c_make_exception("TypeError", "keyword arguments are not supported for this method"));
    return &P2C_None;
}

static P2C_Object* math_sqrt_fn(P2C_Object **args, size_t nargs) { if (nargs != 1) { p2c_raise(p2c_make_exception("TypeError", "sqrt expects 1 argument")); return &P2C_None; } return p2c_obj_from_float(sqrt(p2c_obj_as_float(args[0]))); }
static P2C_Object* math_sin_fn(P2C_Object **args, size_t nargs) { if (nargs != 1) { p2c_raise(p2c_make_exception("TypeError", "sin expects 1 argument")); return &P2C_None; } return p2c_obj_from_float(sin(p2c_obj_as_float(args[0]))); }
static P2C_Object* math_cos_fn(P2C_Object **args, size_t nargs) { if (nargs != 1) { p2c_raise(p2c_make_exception("TypeError", "cos expects 1 argument")); return &P2C_None; } return p2c_obj_from_float(cos(p2c_obj_as_float(args[0]))); }
static P2C_Object* math_pow_fn(P2C_Object **args, size_t nargs) { if (nargs != 2) { p2c_raise(p2c_make_exception("TypeError", "pow expects 2 arguments")); return &P2C_None; } return p2c_obj_from_float(pow(p2c_obj_as_float(args[0]), p2c_obj_as_float(args[1]))); }


/* ============================================================
 * GC 実装: mark & sweep + 保守的スタックスキャン
 * ============================================================ */

/* マークフェーズ: obj とその子を再帰的にマーク */
static void gc_mark(P2C_Object *obj, void *ctx) {
    (void)ctx;
    if (!obj || obj->gc_marked) return;
    /* スタティックなセンチネル (None/True/False) はリストに入っていないので除外 */
    if (obj == &P2C_None || obj == &P2C_True || obj == &P2C_False || obj == &P2C_Ellipsis) return;
    obj->gc_marked = 1;
    /* 子オブジェクトを辿る */
    if (obj->cls && obj->cls->gc_traverse)
        obj->cls->gc_traverse(obj, gc_mark, NULL);
}

/* スタックスキャン用アドレス索引の構築。
 * 収集開始時点の全オブジェクトをオープンアドレッシング表へ入れる。
 * 表が確保できない場合は false を返し、呼び出し側は線形探索へ落ちる。 */
static bool gc_scan_index_build(void) {
    size_t count = g_gc_obj_count;
    /* 負荷率50%を保つため2倍を確保し、2の冪へ切り上げる。 */
    size_t want = count < 8 ? 16 : count * 2;
    if (want < count) return false; /* 桁あふれ */
    if (want > g_scan_index_cap) {
        size_t new_cap = g_scan_index_cap ? g_scan_index_cap : 16;
        while (new_cap < want) {
            if (new_cap > ((size_t)-1) / 2) return false;
            new_cap *= 2;
        }
        void **grown = (void**)p2c_heap_realloc(g_scan_index, new_cap * sizeof(void*));
        if (!grown) return false;
        g_scan_index = grown;
        g_scan_index_cap = new_cap;
    }
    if (!g_scan_index) return false;
    memset(g_scan_index, 0, g_scan_index_cap * sizeof(void*));
    g_scan_index_used = 0;
    size_t mask = g_scan_index_cap - 1;
    for (P2C_Object *o = g_gc_all; o; o = o->gc_next) {
        if (o == &P2C_None || o == &P2C_True || o == &P2C_False) continue;
        size_t slot = ((size_t)(uintptr_t)o >> 4) & mask;
        while (g_scan_index[slot] != NULL) slot = (slot + 1) & mask;
        g_scan_index[slot] = (void*)o;
        g_scan_index_used++;
    }
    return true;
}

static void gc_scan_index_clear(void) {
    if (g_scan_index && g_scan_index_cap) memset(g_scan_index, 0, g_scan_index_cap * sizeof(void*));
    g_scan_index_used = 0;
}

/* candidate がGC管理下のオブジェクトならそれへのポインタを返す。 */
static P2C_Object* gc_candidate_object(void *candidate) {
    if (g_scan_index && g_scan_index_used) {
        size_t mask = g_scan_index_cap - 1;
        size_t slot = ((size_t)(uintptr_t)candidate >> 4) & mask;
        while (g_scan_index[slot] != NULL) {
            if (g_scan_index[slot] == candidate) return (P2C_Object*)candidate;
            slot = (slot + 1) & mask;
        }
        return NULL;
    }
    for (P2C_Object *o = g_gc_all; o; o = o->gc_next) {
        if ((void*)o == candidate) return o;
    }
    return NULL;
}

/* 保守的スタックスキャン用ビジター (スタック上の語がGCオブジェクトなら mark)
 * no_sanitize: 保守的スキャンは隣接する変数の "間" を意図的に読む。
 * ASan はそれを stack-buffer-overflow / use-after-scope として報告するが、
 * これは仕様上の誤検知であるため、この関数だけインスツルメントを外す。 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((no_sanitize("address")))
#endif
static void gc_stack_scan(void *stack_lo, void *stack_hi) {
    /* uintptr_t でアライン */
    uintptr_t lo = ((uintptr_t)stack_lo + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    uintptr_t hi = (uintptr_t)stack_hi & ~(sizeof(void*) - 1);
    /* GCオブジェクトが存在しない、あるいはアドレス範囲外の語は索引を
     * 引かずに捨てる（スタック長ぶんの定数コストを大幅に下げる）。 */
    if (g_gc_obj_count == 0 || g_gc_addr_lo > g_gc_addr_hi) return;
    const uintptr_t obj_lo = g_gc_addr_lo;
    const uintptr_t obj_hi = g_gc_addr_hi;
    for (uintptr_t addr = lo; addr + sizeof(void*) <= hi; addr += sizeof(void*)) {
        g_gc_scan_words++;
        void *candidate;
        memcpy(&candidate, (void*)addr, sizeof(void*));
        uintptr_t cand = (uintptr_t)candidate;
        if (cand < obj_lo || cand > obj_hi) continue;
        /* アドレス索引（構築済みなら平均O(1)、失敗時は線形探索）で
         * GC管理下のオブジェクトかを判定する。 */
        P2C_Object *hit = gc_candidate_object(candidate);
        if (hit) gc_mark(hit, NULL);
    }
}

/* ============================================================
 * 公開 GC API
 * ============================================================ */
void p2c_gc_set_stack_bounds(void *stack_lo, void *stack_hi) {
    if (!stack_lo || !stack_hi || stack_lo == stack_hi) return;
    g_gc_stack_lo = stack_lo;
    g_gc_stack_hi = stack_hi;
    g_gc_scan_warned = false;
}

bool p2c_gc_stack_scan_available(void) {
    /* 走査は「現在のフレームから宣言された上端まで」なので、上端だけで足りる
     * （p2c_gc_init() にスタック上のアドレスを渡した場合も有効）。 */
    return g_gc_stack_hi != NULL;
}

void p2c_gc_init(void *stack_hint) {
    g_gc_stack_bottom = stack_hint; /* 後方互換: hint は未使用になるが API は維持 */
    /* OS から実際のスタック境界を取得 (Linux/POSIX with pthreads).
     * ASan は fakestack / shadow memory のせいで local 変数のアドレスや
     * __builtin_frame_address(0) が実スタックアドレスと異なる値を返すため、
     * OS に直接問い合わせる方が確実で ASan の下でも安全に動く。 */
#if defined(__linux__) && defined(_GNU_SOURCE) && !defined(PYTHON_CODE_TO_C_NO_STDLIB)
    {
        pthread_t self = pthread_self();
        pthread_attr_t attr;
        if (pthread_getattr_np(self, &attr) == 0) {
            void *stack_addr = NULL;
            size_t stack_size = 0;
            pthread_attr_getstack(&attr, &stack_addr, &stack_size);
            pthread_attr_destroy(&attr);
            if (stack_addr && stack_size) {
                g_gc_stack_lo = stack_addr;
                g_gc_stack_hi = (char*)stack_addr + stack_size;
            }
        }
    }
#elif defined(P2C_GC_STACK_HINT_RANGE) && (P2C_GC_STACK_HINT_RANGE > 0)
    /* スタック境界を問い合わせるAPIがない環境向けの、明示オプトインな近似。
     * 「stack_hint（呼び出し側フレームの内側）から最大 P2C_GC_STACK_HINT_RANGE
     * バイト下までがスタック」と宣言し、その区間を保守的に走査する。
     * 値を大きくし過ぎると無関係なメモリを走査するため、タスクのスタック
     * サイズに合わせて指定する。範囲外のタスクスタックを使う場合は
     * p2c_gc_set_stack_bounds() を直接呼ぶこと。 */
    g_gc_stack_lo = (char*)stack_hint - (size_t)(P2C_GC_STACK_HINT_RANGE);
    g_gc_stack_hi = stack_hint;
#else
    /* スタック境界を問い合わせるAPIがない環境（自作OS/ベアメタル）。
     * stack_hint は「呼び出し側フレーム内のローカル変数のアドレス」なので、
     * スタックの上端側の実アドレスとしてそのまま使える。収集時は
     * 「実際に使われている範囲（現在のフレームより上）」だけを走査するため、
     * 領域全体を事前に知る必要はない（p2c_gc_collect() の説明を参照）。
     * hint が NULL の場合だけ境界不明とし、カーネルが
     * p2c_gc_set_stack_bounds() を呼ぶまで収集を安全側に停止する。 */
    g_gc_stack_lo = NULL;
    g_gc_stack_hi = stack_hint;
#endif
}

void p2c_gc_register_root(P2C_Object **slot) {
    if (!slot) return;
    for (size_t i = 0; i < g_gc_root_count; i++) {
        if (g_gc_roots[i] == slot) return;
    }
    if (g_gc_root_count >= P2C_GC_ROOT_CAPACITY) {
        p2c_platform_abort("Python Code to C Alpha0.6 GC root capacity exceeded");
        return;
    }
    g_gc_roots[g_gc_root_count++] = slot;
}
void p2c_gc_unregister_root(P2C_Object **slot) {
    if (!slot) return;
    for (size_t i = 0; i < g_gc_root_count; i++) {
        if (g_gc_roots[i] == slot) {
            g_gc_roots[i] = g_gc_roots[g_gc_root_count - 1];
            g_gc_roots[g_gc_root_count - 1] = NULL;
            g_gc_root_count--;
            return;
        }
    }
}
void p2c_gc_reset_roots(void) {
    memset(g_gc_roots, 0, sizeof(g_gc_roots));
    g_gc_root_count = 0;
}
void p2c_gc_collect(void) {
    if (!g_gc_enabled || g_gc_collecting) return;
    if (!p2c_gc_stack_scan_available()) {
        /* スタック境界が未登録。回収すると生存オブジェクトを解放してしまう
         * ため、ここでは何もしない（安全側の停止）。原因と対処を一度だけ
         * 診断出力し、以降は静かにスキップする。 */
        if (!g_gc_scan_warned) {
            g_gc_scan_warned = true;
            p2c_platform_write("Python Code to C: GC collection skipped because the stack "
                               "bounds are unknown; call p2c_gc_set_stack_bounds() "
                               "(or p2c_embed_start()) to enable collection.\n");
        }
        return;
    }
    g_gc_collecting = true;

    /* ── 0. スタックスキャン用アドレス索引を構築（失敗時は線形探索へ） ── */
    (void)gc_scan_index_build();

    /* ── 1. レジスタをスタックに flush (P2C_SETJMP の副作用を利用) ── */
    jmp_buf env;
    P2C_SETJMP(env);

    /* ── 2. マークフェーズ ── */

    /* 2-a. 明示的ルート */
    for (size_t i = 0; i < g_gc_root_count; i++) {
        if (*g_gc_roots[i]) gc_mark(*g_gc_roots[i], NULL);
    }

    /* 2-b. ランタイム内部レジストリ (g_module_registry, g_class_registry_objs) */
    if (g_module_registry) {
        for (size_t i = 0; i < g_module_registry->bucket_count; i++) {
            for (P2C_MapEntry *e = g_module_registry->buckets[i]; e; e = e->next)
                gc_mark((P2C_Object*)e->val, NULL);
        }
    }
    /* クラスレジストリは固定配列 (g_class_registry_objs[]) 形式。
     * 定義（static）はこのファイル内の前方にあるため再宣言は不要。 */
    {
        for (int _ci = 0; _ci < g_class_registry_count; _ci++)
            gc_mark(g_class_registry_objs[_ci], NULL);
    }

    /* 2-c. refcount > 0 のオブジェクト (ネイティブコードがピン留め中) */
    for (P2C_Object *o = g_gc_all; o; o = o->gc_next) {
        if (o->refcount > 0) gc_mark(o, NULL);
    }

    /* 2-c'. ランタイム内部ルート（事前確保済みMemoryError。
     * メモリ枯渇時に再確保せず raise するため生存させ続ける）。 */
    if (g_oom_exception) gc_mark(g_oom_exception, NULL);

    /* 2-c''. TLS の一時値（式評価中の左オペランド、処理中の例外）。
     * これらはスタック上ではなく TLS に置かれるため、保守的スタックスキャン
     * では見えない。ルート化しないと、式の途中で自動収集が走ったときに
     * 左オペランドが解放され、p2c_binop_finish() が解放済みポインタを
     * 演算へ渡す（Pythonの式が静かに壊れる）。 */
    g_gc_temp_roots = gc_mark_tls_temporaries();

    /* 2-d. 保守的スタックスキャン
     * g_gc_stack_lo/hi は p2c_gc_init() が OS から取得した実スタック境界、
     * p2c_gc_init() へ渡された「スタック上のアドレス（上端のヒント）」、
     * または p2c_gc_set_stack_bounds() でカーネルが宣言したタスクスタック区間。
     *
     * 走査するのは「現在の関数フレーム（= 実際に使われているスタック）から
     * 宣言された上端まで」だけ。区間全体を毎回走査すると、Linux の既定
     * 8MiB スタックのように未使用部分が大きい環境で
     * 「収集回数 × スタック長」のコストが支配的になる（しきい値を小さく
     * すると顕著）。呼び出し元のフレームは常に現在のSPより上位アドレス側に
     * あるため、[sp, 上端] の走査で生存ローカルを取りこぼさない。
     * スタック成長方向の仮定も不要（上下端を min/max で正規化するだけ）。 */
    {
        uintptr_t sp   = (uintptr_t)&env;
        uintptr_t b1   = (uintptr_t)g_gc_stack_lo;
        uintptr_t b2   = (uintptr_t)g_gc_stack_hi;
        g_gc_scan_words = 0;
        /* 上下端を正規化（b1 は未宣言なら 0 なので下端として無害）。 */
        uintptr_t top  = b1 > b2 ? b1 : b2;
        uintptr_t low  = b1 < b2 ? b1 : b2;
        if (sp <= top) {
            uintptr_t scan_lo = sp > low ? sp : low;
            if (scan_lo < top) gc_stack_scan((void*)scan_lo, (void*)top);
        } else if (low < top) {
            /* 宣言区間が現在のフレームより下にある（通常は起こらない）: 念のため区間全体 */
            gc_stack_scan((void*)low, (void*)top);
        }
    }
    /* 索引はスキャン専用。スイープでオブジェクトが消える前に無効化し、
     * 次の収集まで古いポインタを参照しないことを保証する。 */
    gc_scan_index_clear();

    /* ── 3. スイープフェーズ ── */
    P2C_Object **prev_next = &g_gc_all;
    size_t freed = 0;
    P2C_Object *cur = g_gc_all;
    while (cur) {
        P2C_Object *next = cur->gc_next;
        if (!cur->gc_marked) {
            /* 到達不能: リストから外して解放 */
            *prev_next = next;
            gc_free_obj_data(cur);
            p2c_heap_free(cur);
            freed++;
            g_gc_obj_count--;
        } else {
            /* 生存: マークをクリアして次サイクルに備える */
            cur->gc_marked = 0;
            prev_next = &cur->gc_next;
        }
        cur = next;
    }

    g_gc_bytes_alloc = 0;
    g_gc_last_freed  = freed;
    g_gc_collections++;
    g_gc_collecting  = false;
}
void    p2c_gc_set_enabled(bool enabled)    { g_gc_enabled   = enabled; }
bool    p2c_gc_is_enabled(void)             { return g_gc_enabled; }
void    p2c_gc_set_threshold(size_t bytes)  { g_gc_threshold = bytes ? bytes : 1; }
bool    p2c_gc_is_collecting(void)         { return g_gc_collecting; }
size_t  p2c_gc_object_count(void)          { return g_gc_obj_count; }
size_t  p2c_gc_collections_run(void)       { return g_gc_collections; }
size_t  p2c_gc_last_freed(void)            { return g_gc_last_freed; }
size_t  p2c_gc_last_stack_words(void)      { return g_gc_scan_words; }
size_t  p2c_gc_last_temp_roots(void)        { return g_gc_temp_roots; }
size_t  p2c_gc_root_count(void)             { return g_gc_root_count; }
size_t  p2c_gc_root_capacity(void)          { return P2C_GC_ROOT_CAPACITY; }

void p2c_runtime_init(void *heap_base, size_t heap_sz) {
    if (g_runtime_active) return;
#ifdef PYTHON_CODE_TO_C_NO_STDLIB
#  ifndef PYTHON_CODE_TO_C_NO_LIBC_STUBS
    heap_start = (char*)heap_base; heap_size = heap_sz; heap_used = 0; heap_peak = 0;
#  else
    /* カーネル提供アロケータを使う構成ではヒープ引数は無視される。 */
    (void)heap_base; (void)heap_sz;
#  endif
#else
    (void)heap_base; (void)heap_sz;
#endif
#if defined(PYTHON_CODE_TO_C_NO_STDLIB) && !defined(PYTHON_CODE_TO_C_NO_LIBC_STUBS)
    /* 変換器コア（文字列ビルダ等）が静的フォールバックへ落ちないよう、
     * 線形ヒープの準備完了を共有ヒープ層へ通知する（実ヒープを渡された
     * ときだけ true になる）。 */
    p2c_heap_note_stub_ready(heap_start != NULL && heap_size > 0);
#endif
    g_runtime_active = true;
    p2c_platform_init();
    if (!g_module_registry) g_module_registry = new_attr_map();
    /* OOM通知用の MemoryError を事前確保しておく。この1個は「ランタイム内部
     * ルート」として毎回のマークフェーズで生存扱いし、メモリ枯渇時に再確保せず
     * raise できるようにする（ユーザーが登録するルート枠は消費しない）。 */
    if (!g_oom_exception) {
        g_oom_exception = p2c_make_exception("MemoryError", "out of memory");
    }
    P2C_Object *math_mod = p2c_module_new("math");
    if (math_mod) {
        p2c_module_set_attr(math_mod, "pi", p2c_obj_from_float(3.14159265358979323846));
        p2c_module_set_attr(math_mod, "e", p2c_obj_from_float(2.71828182845904523536));
        p2c_module_set_attr(math_mod, "sqrt", p2c_function_new("sqrt", math_sqrt_fn));
        p2c_module_set_attr(math_mod, "sin", p2c_function_new("sin", math_sin_fn));
        p2c_module_set_attr(math_mod, "cos", p2c_function_new("cos", math_cos_fn));
        p2c_module_set_attr(math_mod, "pow", p2c_function_new("pow", math_pow_fn));
        p2c_register_module(math_mod);
    }
#ifndef PYTHON_CODE_TO_C_NO_PYGAME
    p2c_register_pygame_module();
#endif
}
void p2c_runtime_shutdown(void) {
    if (!g_runtime_active) return;

    p2c_reset_async_queue();
    p2c_gc_reset_roots();
#ifndef PYTHON_CODE_TO_C_NO_PYGAME
    p2c_pygame_runtime_reset();
#endif
    free_map_shallow(g_module_registry);
    g_module_registry = NULL;
    p2c_reset_class_registry();

    P2C_Object *cur = g_gc_all;
    while (cur) {
        P2C_Object *next = cur->gc_next;
        gc_free_obj_data(cur);
        p2c_heap_free(cur);
        cur = next;
    }
    g_gc_all = NULL;
    g_gc_obj_count = 0;
    g_gc_bytes_alloc = 0;
    g_gc_collections = 0;
    g_gc_last_freed = 0;
    g_gc_collecting = false;
    g_gc_enabled = true;
    g_gc_threshold = 256 * 1024;
    g_gc_stack_bottom = NULL;
    g_gc_stack_lo = NULL;
    g_gc_stack_hi = NULL;
    g_gc_scan_warned = false;
    g_gc_addr_lo = (uintptr_t)-1;
    g_gc_addr_hi = 0;
    g_gc_scan_words = 0;
    g_gc_temp_roots = 0;
    /* 式評価の途中で停止した場合に備えて、TLS の一時値も空にする
     * （f-string のビルダはここで解放される）。 */
    p2c_binop_rewind(0);
    p2c_fstr_rewind(0);
    g_oom_exception = NULL; /* 上で解放済み（再初期化時に作り直す） */
    g_oom_in_handler = false;
    gc_scan_index_clear();
#ifdef PYTHON_CODE_TO_C_NO_STDLIB
#  ifndef PYTHON_CODE_TO_C_NO_LIBC_STUBS
    heap_start = NULL; heap_size = 0; heap_used = 0; heap_peak = 0;
    p2c_heap_note_stub_ready(false);
#  endif
#endif
    p2c_exc_stack = NULL;
    p2c_active_exception = NULL;
    g_runtime_active = false;
    p2c_platform_shutdown();
}
bool p2c_runtime_is_active(void) { return g_runtime_active; }
size_t p2c_runtime_heap_size(void) {
#ifdef PYTHON_CODE_TO_C_NO_STDLIB
#  ifndef PYTHON_CODE_TO_C_NO_LIBC_STUBS
    return heap_size;
#  else
    return 0;
#  endif
#else
    return 0;
#endif
}
size_t p2c_runtime_heap_used(void) {
#ifdef PYTHON_CODE_TO_C_NO_STDLIB
#  ifndef PYTHON_CODE_TO_C_NO_LIBC_STUBS
    return heap_used;
#  else
    return 0;
#  endif
#else
    return 0;
#endif
}
size_t p2c_runtime_heap_peak(void) {
#ifdef PYTHON_CODE_TO_C_NO_STDLIB
#  ifndef PYTHON_CODE_TO_C_NO_LIBC_STUBS
    return heap_peak;
#  else
    return 0;
#  endif
#else
    return 0;
#endif
}
void* p2c_runtime_alloc(size_t size) { return p2c_heap_alloc(size); }
void* p2c_runtime_realloc(void *ptr, size_t old_size, size_t new_size) { (void)old_size; return p2c_heap_realloc(ptr, new_size); }
void p2c_runtime_free(void *ptr) { p2c_heap_free(ptr); }

void p2c_raise(P2C_Object *exc) {
    if (!exc) exc = p2c_make_exception("RuntimeError", "unknown error");
    if (!exc) {
        /* 例外オブジェクトの生成自体に失敗した場合（メモリ枯渇）。NULLを
         * 例外として伝播させるとハンドラ側で落ちるため、ここで安全に停止する
         * （-fanalyzer が指摘した経路）。 */
        p2c_platform_write("Exception\n: out of memory while creating exception object\n");
        p2c_platform_abort("unhandled exception");
        return;
    }
    if (p2c_exc_stack) { p2c_exc_stack->exc = exc; P2C_LONGJMP(p2c_exc_stack->env, 1); }
    if (exc->cls && exc->cls->type_tag == OBJ_EXCEPTION) {
        p2c_platform_write(exc->u.v_exception.type_name ? exc->u.v_exception.type_name : "Exception");
        p2c_platform_write(": ");
        p2c_platform_write(exc->u.v_exception.msg ? exc->u.v_exception.msg : "");
        p2c_platform_write("\n");
    }
    p2c_platform_abort("unhandled exception");
}
void p2c_reraise(void) {
    if (!p2c_active_exception) {
        p2c_raise(p2c_make_exception("RuntimeError", "No active exception to reraise"));
        return;
    }
    P2C_Object *exc = p2c_active_exception;
    p2c_active_exception = NULL;
    p2c_raise(exc);
}

P2C_Object* p2c_make_exception(const char *type_name, const char *msg) {
    P2C_Object *o = p2c_obj_new(&P2C_Class_Exception); if (!o) return NULL;
    o->u.v_exception.type_name = p2c_strdup_local(type_name ? type_name : "Exception");
    o->u.v_exception.msg = p2c_strdup_local(msg ? msg : "");
    o->u.v_exception.cause = NULL;
    return o;
}

P2C_Object* p2c_exception_with_cause(P2C_Object *exc, P2C_Object *cause) {
    if (exc && exc->cls && exc->cls->type_tag == OBJ_EXCEPTION) exc->u.v_exception.cause = cause;
    return exc ? exc : &P2C_None;
}

P2C_Object* p2c_posonly_keyword_error(const char *function_name, const char *parameter_name) {
    char message[192];
    const char *fn = function_name ? function_name : "function";
    const char *param = parameter_name ? parameter_name : "parameter";
#ifdef PYTHON_CODE_TO_C_NO_STDLIB
    size_t pos = 0;
    const char *parts[3] = {fn, "() got some positional-only arguments passed as keyword arguments: '", param};
    for (size_t part = 0; part < 3; part++) {
        for (size_t i = 0; parts[part][i] != '\0' && pos + 2 < sizeof(message); i++) message[pos++] = parts[part][i];
    }
    if (pos + 2 < sizeof(message)) message[pos++] = '\'';
    message[pos] = '\0';
#else
    (void)snprintf(message, sizeof(message), "%s() got some positional-only arguments passed as keyword arguments: '%s'", fn, param);
#endif
    p2c_raise(p2c_make_exception("TypeError", message));
    return &P2C_None;
}
bool p2c_exc_match(P2C_Object *exc, P2C_ClassDef *cls) {
    if (!exc || !cls) return false;
    if (exc->cls == cls) return true;
    if (cls == &P2C_Class_Exception) return p2c_obj_is_exception_instance(exc) || (exc->cls && exc->cls->type_tag == OBJ_EXCEPTION);
    return false;
}
bool p2c_exc_name_match(P2C_Object *exc, const char *type_name) {
    if (!exc) return false;
    if (!type_name || !*type_name) return true;
    if (exc->cls && exc->cls->type_tag == OBJ_EXCEPTION) return strcmp(exc->u.v_exception.type_name ? exc->u.v_exception.type_name : "", type_name) == 0 || strcmp(type_name, "Exception") == 0;
    if (exc->cls && exc->cls->type_tag == OBJ_INSTANCE && exc->u.v_instance.klass && exc->u.v_instance.klass->cls && exc->u.v_instance.klass->cls->type_tag == OBJ_CLASS) {
        /* 以前は直接の基底名(base_name)しか比較しておらず、基底が2段以上先に
         * ある例外階層（class A(Exception); class B(A); で except A）を捕捉
         * できなかった。MRO上の名前と比較して階層を辿れるようにする。 */
        if (strcmp(type_name, "Exception") == 0) return true;
        return p2c_class_chain_has_name(exc->u.v_instance.klass, type_name);
    }
    return false;
}

static void p2c_format_float(double v, char *buf, size_t bufsz) {
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
    if (p2c_float_ne(v, v)) { /* NaN は自分自身と等しくない */ snprintf(buf, bufsz, "nan"); return; }
    if (v > 1e308 * 10.0) { snprintf(buf, bufsz, "inf"); return; }
    if (v < -1e308 * 10.0) { snprintf(buf, bufsz, "-inf"); return; }
    /* Pythonのfloatは常に小数点を含む形式で表示される(例: 4.0)。
     * %g は整数値になる浮動小数点数から小数点以下を省略してしまう(例: "-4")ため、
     * 指数表記でも小数表記でもない場合は明示的に ".0" を補う。 */
    snprintf(buf, bufsz, "%.17g", v);
    /* 17桁精度は往復変換の安全域だが冗長になりがちなので、
     * 短い表現で同じ値に戻るなら短縮する */
    for (int prec = 1; prec < 17; prec++) {
        char shorter[64];
        snprintf(shorter, sizeof(shorter), "%.*g", prec, v);
        double back = strtod(shorter, NULL);
        if (p2c_float_eq(back, v)) { snprintf(buf, bufsz, "%s", shorter); break; }
    }
    bool has_dot_or_exp = false;
    for (char *c = buf; *c; c++) { if (*c == '.' || *c == 'e' || *c == 'E' || *c == 'n' || *c == 'i') { has_dot_or_exp = true; break; } }
    if (!has_dot_or_exp) strcat(buf, ".0");
#else
    (void)v;
    snprintf(buf, bufsz, "0");
#endif
}

static void p2c_format_int64_decimal(int64_t value, char *buf, size_t cap) {
    char digits[32];
    size_t count = 0;
    uint64_t magnitude;
    bool negative = value < 0;
    if (!buf || cap == 0) return;
    magnitude = negative ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
    do {
        digits[count++] = (char)('0' + (magnitude % 10u));
        magnitude /= 10u;
    } while (magnitude != 0 && count < sizeof(digits));
    size_t out = 0;
    if (negative && out + 1 < cap) buf[out++] = '-';
    while (count > 0 && out + 1 < cap) buf[out++] = digits[--count];
    buf[out] = '\0';
}

static void p2c_write_quoted_str(const char *s) {
    p2c_platform_write("'");
    for (const char *c = s; c && *c; c++) {
        if (*c == '\'' || *c == '\\') p2c_platform_write("\\");
        if (*c == '\n') { p2c_platform_write("\\n"); continue; }
        if (*c == '\r') { p2c_platform_write("\\r"); continue; }
        if (*c == '\t') { p2c_platform_write("\\t"); continue; }
        if (*c == '\v') { p2c_platform_write("\\v"); continue; }
        if (*c == '\f') { p2c_platform_write("\\f"); continue; }
        if (*c == '\b') { p2c_platform_write("\\b"); continue; }
        if (*c == '\a') { p2c_platform_write("\\a"); continue; }
        char one[2] = { *c, '\0' };
        p2c_platform_write(one);
    }
    p2c_platform_write("'");
}

static void p2c_print_raw_ex(P2C_Object *obj, bool repr) {
    if (!obj) { p2c_platform_write("None"); return; }
    switch (obj->cls ? obj->cls->type_tag : OBJ_NONE) {
        case OBJ_NONE: p2c_platform_write("None"); break;
        case OBJ_STR:
            if (repr) p2c_write_quoted_str(obj->u.v_str.data ? obj->u.v_str.data : "");
            else p2c_platform_write(obj->u.v_str.data ? obj->u.v_str.data : "");
            break;
        case OBJ_INT: {
            char buf[64];
            p2c_format_int64_decimal(obj->u.v_int, buf, sizeof(buf));
            p2c_platform_write(buf); break;
        }
        case OBJ_FLOAT: {
            char buf[64];
            p2c_format_float(obj->u.v_float, buf, sizeof(buf));
            p2c_platform_write(buf); break;
        }
        case OBJ_BOOL: p2c_platform_write(obj->u.v_bool ? "True" : "False"); break;
        case OBJ_ELLIPSIS: p2c_platform_write("Ellipsis"); break;
        case OBJ_LIST:
            p2c_platform_write("[");
            for (size_t i = 0; i < obj->u.v_list.len; i++) { if (i) p2c_platform_write(", "); p2c_print_raw_ex(obj->u.v_list.items[i], true); }
            p2c_platform_write("]");
            break;
        case OBJ_TUPLE:
            p2c_platform_write("(");
            for (size_t i = 0; i < obj->u.v_tuple.len; i++) { if (i) p2c_platform_write(", "); p2c_print_raw_ex(obj->u.v_tuple.items[i], true); }
            if (obj->u.v_tuple.len == 1) p2c_platform_write(",");
            p2c_platform_write(")");
            break;
        case OBJ_DICT: {
            p2c_platform_write("{");
            bool first = true;
            for (P2C_DictEntry *e = obj->u.v_dict.order_head; e; e = e->order_next) {
                if (!first) p2c_platform_write(", ");
                first = false;
                p2c_print_raw_ex(e->key, true); p2c_platform_write(": "); p2c_print_raw_ex(e->val, true);
            }
            p2c_platform_write("}");
            break;
        }
        case OBJ_SET: {
            if (obj->u.v_dict.len == 0) { p2c_platform_write("set()"); break; }
            p2c_platform_write("{");
            bool first = true;
            for (P2C_DictEntry *e = obj->u.v_dict.order_head; e; e = e->order_next) {
                if (!first) p2c_platform_write(", ");
                first = false;
                p2c_print_raw_ex(e->key, true);
            }
            p2c_platform_write("}");
            break;
        }
        case OBJ_MODULE:
            p2c_platform_write("<module "); p2c_platform_write(obj->u.v_module.name ? obj->u.v_module.name : "?"); p2c_platform_write(">");
            break;
        case OBJ_CLASS:
            p2c_platform_write("<class "); p2c_platform_write(obj->u.v_class.name ? obj->u.v_class.name : "?"); p2c_platform_write(">");
            break;
        case OBJ_INSTANCE:
            /* Pythonのディスパッチ規則に合わせる:
             * - repr()（コンテナ要素の表示等）は __repr__ を優先し、無ければ汎用表示。
             * - str()（print()の直接表示等）は __str__ を優先し、無ければ __repr__、
             *   それも無ければ汎用表示にフォールバックする。
             * （以前はここが常に <ClassName object> という汎用表示に固定されており、
             * ユーザー定義の __str__ / __repr__ が一切反映されていなかった）。 */
            if (repr && p2c_has_method(obj, "__repr__")) {
                P2C_Object *s = p2c_call_attr(obj, "__repr__", NULL, 0);
                p2c_platform_write(p2c_obj_as_str(s));
            } else if (!repr && p2c_has_method(obj, "__str__")) {
                P2C_Object *s = p2c_call_attr(obj, "__str__", NULL, 0);
                p2c_platform_write(p2c_obj_as_str(s));
            } else if (!repr && p2c_has_method(obj, "__repr__")) {
                P2C_Object *s = p2c_call_attr(obj, "__repr__", NULL, 0);
                p2c_platform_write(p2c_obj_as_str(s));
            } else {
                p2c_platform_write("<"); p2c_platform_write(obj->u.v_instance.klass && obj->u.v_instance.klass->u.v_class.name ? obj->u.v_instance.klass->u.v_class.name : "instance"); p2c_platform_write(" object>");
            }
            break;
        case OBJ_EXCEPTION:
            /* Pythonのstr(exception)/print(exception)は型名を含まず、メッセージのみを表示する
             * （型名込みの "Type(msg)" 形式はrepr()の見た目であり、str()ではない）。 */
            p2c_platform_write(obj->u.v_exception.msg ? obj->u.v_exception.msg : "");
            break;
        default:
            p2c_platform_write("<object>"); break;
    }
}
static void p2c_print_raw(P2C_Object *obj) { p2c_print_raw_ex(obj, false); }

void p2c_print(P2C_Object *obj) { p2c_print_raw(obj); p2c_platform_write("\n"); }
P2C_Object* p2c_print_multi_opts(P2C_Object **args, size_t nargs, P2C_Object *sep, P2C_Object *end) {
    const char *sep_text = " ";
    const char *end_text = "\n";
    if (!p2c_obj_is_none(sep)) {
        if (!p2c_obj_is_str(sep)) { p2c_raise(p2c_make_exception("TypeError", "sep must be None or a string")); return &P2C_None; }
        sep_text = p2c_obj_as_str(sep);
    }
    if (!p2c_obj_is_none(end)) {
        if (!p2c_obj_is_str(end)) { p2c_raise(p2c_make_exception("TypeError", "end must be None or a string")); return &P2C_None; }
        end_text = p2c_obj_as_str(end);
    }
    for (size_t i = 0; i < nargs; i++) { if (i) p2c_platform_write(sep_text); p2c_print_raw(args[i]); }
    p2c_platform_write(end_text);
    return &P2C_None;
}
P2C_Object* p2c_print_multi(P2C_Object **args, size_t nargs) { return p2c_print_multi_opts(args, nargs, &P2C_None, &P2C_None); }

/* obj -> Pythonの str() 相当の文字列オブジェクトを生成する。
 * f-string の {expr} 埋め込みや str(x) 呼び出しから使われる。
 * p2c_print_raw と表示ロジックを揃えるため、同じ書式ルールで
 * P2C_String バッファへ書き出す形にしている（出力先が違うだけ）。 */
static void p2c_write_quoted_str_buf(const char *s, P2C_String *out) {
    p2c_str_append(out, "'");
    for (const char *c = s; c && *c; c++) {
        if (*c == '\'' || *c == '\\') p2c_str_append(out, "\\");
        if (*c == '\n') { p2c_str_append(out, "\\n"); continue; }
        if (*c == '\r') { p2c_str_append(out, "\\r"); continue; }
        if (*c == '\t') { p2c_str_append(out, "\\t"); continue; }
        if (*c == '\v') { p2c_str_append(out, "\\v"); continue; }
        if (*c == '\f') { p2c_str_append(out, "\\f"); continue; }
        if (*c == '\b') { p2c_str_append(out, "\\b"); continue; }
        if (*c == '\a') { p2c_str_append(out, "\\a"); continue; }
        char one[2] = { *c, '\0' };
        p2c_str_append(out, one);
    }
    p2c_str_append(out, "'");
}

static void p2c_obj_to_buf_ex(P2C_Object *obj, P2C_String *out, bool repr) {
    if (!obj) { p2c_str_append(out, "None"); return; }
    switch (obj->cls ? obj->cls->type_tag : OBJ_NONE) {
        case OBJ_NONE: p2c_str_append(out, "None"); break;
        case OBJ_STR:
            if (repr) p2c_write_quoted_str_buf(obj->u.v_str.data ? obj->u.v_str.data : "", out);
            else p2c_str_append(out, obj->u.v_str.data ? obj->u.v_str.data : "");
            break;
        case OBJ_INT: {
            char buf[64];
            snprintf(buf, sizeof(buf), "%lld", (long long)obj->u.v_int);
            p2c_str_append(out, buf); break;
        }
        case OBJ_FLOAT: {
            char buf[64];
            p2c_format_float(obj->u.v_float, buf, sizeof(buf));
            p2c_str_append(out, buf); break;
        }
        case OBJ_BOOL: p2c_str_append(out, obj->u.v_bool ? "True" : "False"); break;
        case OBJ_ELLIPSIS: p2c_str_append(out, "Ellipsis"); break;
        case OBJ_LIST:
            p2c_str_append(out, "[");
            for (size_t i = 0; i < obj->u.v_list.len; i++) { if (i) p2c_str_append(out, ", "); p2c_obj_to_buf_ex(obj->u.v_list.items[i], out, true); }
            p2c_str_append(out, "]");
            break;
        case OBJ_TUPLE:
            p2c_str_append(out, "(");
            for (size_t i = 0; i < obj->u.v_tuple.len; i++) { if (i) p2c_str_append(out, ", "); p2c_obj_to_buf_ex(obj->u.v_tuple.items[i], out, true); }
            if (obj->u.v_tuple.len == 1) p2c_str_append(out, ",");
            p2c_str_append(out, ")");
            break;
        case OBJ_DICT: {
            p2c_str_append(out, "{");
            bool first = true;
            for (P2C_DictEntry *e = obj->u.v_dict.order_head; e; e = e->order_next) {
                if (!first) p2c_str_append(out, ", ");
                first = false;
                p2c_obj_to_buf_ex(e->key, out, true); p2c_str_append(out, ": "); p2c_obj_to_buf_ex(e->val, out, true);
            }
            p2c_str_append(out, "}");
            break;
        }
        case OBJ_SET: {
            if (obj->u.v_dict.len == 0) { p2c_str_append(out, "set()"); break; }
            p2c_str_append(out, "{");
            bool first = true;
            for (P2C_DictEntry *e = obj->u.v_dict.order_head; e; e = e->order_next) {
                if (!first) p2c_str_append(out, ", ");
                first = false;
                p2c_obj_to_buf_ex(e->key, out, true);
            }
            p2c_str_append(out, "}");
            break;
        }
        case OBJ_MODULE:
            p2c_str_append(out, "<module "); p2c_str_append(out, obj->u.v_module.name ? obj->u.v_module.name : "?"); p2c_str_append(out, ">");
            break;
        case OBJ_CLASS:
            p2c_str_append(out, "<class "); p2c_str_append(out, obj->u.v_class.name ? obj->u.v_class.name : "?"); p2c_str_append(out, ">");
            break;
        case OBJ_INSTANCE:
            if (repr && p2c_has_method(obj, "__repr__")) {
                P2C_Object *s = p2c_call_attr(obj, "__repr__", NULL, 0);
                p2c_str_append(out, p2c_obj_as_str(s));
            } else if (!repr && p2c_has_method(obj, "__str__")) {
                P2C_Object *s = p2c_call_attr(obj, "__str__", NULL, 0);
                p2c_str_append(out, p2c_obj_as_str(s));
            } else if (!repr && p2c_has_method(obj, "__repr__")) {
                P2C_Object *s = p2c_call_attr(obj, "__repr__", NULL, 0);
                p2c_str_append(out, p2c_obj_as_str(s));
            } else {
                p2c_str_append(out, "<"); p2c_str_append(out, obj->u.v_instance.klass && obj->u.v_instance.klass->u.v_class.name ? obj->u.v_instance.klass->u.v_class.name : "instance"); p2c_str_append(out, " object>");
            }
            break;
        case OBJ_EXCEPTION:
            p2c_str_append(out, obj->u.v_exception.msg ? obj->u.v_exception.msg : "");
            break;
        default:
            p2c_str_append(out, "<object>"); break;
    }
}
static void p2c_obj_to_buf(P2C_Object *obj, P2C_String *out) { p2c_obj_to_buf_ex(obj, out, false); }

/* f-string の汎用フォーマット: f"{expr:spec}" → p2c_fstr_fmt(expr, "spec")
 * Python の書式仕様 (PEP 3101) の主要サブセットを処理する:
 *   :05d  :>10  :<10  :^10  :+.3e  :.2f  :x  :b  :o  :s  など */
P2C_Object* p2c_fstr_fmt(P2C_Object *obj, P2C_Object *spec_obj) {
    const char *spec = p2c_obj_as_str(spec_obj);
    if (!spec || !spec[0]) {
        /* 仕様なし → str() 相当 */
        return p2c_obj_str(obj);
    }
    /* 書式文字の解析: [[fill]align][sign][#][0][width][grouping_option][.precision][type] */
    char fill = ' ', align = 0, sign = 0;
    bool zero_pad = false;
    bool alternate_form = false;
    int width = 0, precision = -1;
    char type_char = 0;
    const char *p = spec;
    /* fill と align: 2文字目が align 文字 (<>^=) なら1文字目が fill */
    if (p[0] && (p[1] == '<' || p[1] == '>' || p[1] == '^' || p[1] == '=')) {
        fill = p[0]; align = p[1]; p += 2;
    } else if (p[0] == '<' || p[0] == '>' || p[0] == '^' || p[0] == '=') {
        align = p[0]; p++;
    }
    /* sign */
    if (*p == '+' || *p == '-' || *p == ' ') sign = *p++;
    /* # flag (hex/octal/binary alternative form) */
    if (*p == '#') { alternate_form = true; p++; }
    /* 0 (zero-pad) */
    if (*p == '0') { zero_pad = true; if (!align) align = '='; p++; }
    /* width */
    while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
    /* grouping */
    bool grouping = false;
    if (*p == '_' || *p == ',') { grouping = true; p++; }
    /* precision */
    if (*p == '.') {
        p++; precision = 0;
        while (*p >= '0' && *p <= '9') precision = precision * 10 + (*p++ - '0');
    }
    /* type */
    type_char = *p;

    if (type_char == 0 && (p2c_obj_is_int(obj) || p2c_obj_is_bool(obj))) type_char = 'd';
    else if (type_char == 0 && p2c_obj_is_float(obj)) type_char = 'g';

    /* 幅・精度の異常値をクランプ（安全のため上限を設ける。実用上十分な大きさ） */
    if (width < 0) width = 0;
    if (width > 65536) width = 65536;
    if (precision > 4096) precision = 4096;

    char val_buf[256] = {0};
    char c_fmt[64];
    int printf_width = zero_pad ? 0 : (width > 200 ? 200 : width); /* val_buf[256]に収まる範囲でprintfに渡す */
    switch (type_char) {
        case 'd': case 'i':
            if (sign) snprintf(c_fmt, sizeof(c_fmt), "%%%c%d%c", sign, printf_width, type_char);
            else      snprintf(c_fmt, sizeof(c_fmt), "%%%d%c", printf_width, type_char);
            snprintf(val_buf, sizeof(val_buf), c_fmt, (long long)p2c_obj_as_int(obj));
            break;
        case 'f': case 'F':
            if (sign) snprintf(c_fmt, sizeof(c_fmt), "%%%c.%d%c", sign, precision >= 0 ? precision : 6, type_char);
            else      snprintf(c_fmt, sizeof(c_fmt), "%%.%d%c", precision >= 0 ? precision : 6, type_char);
            snprintf(val_buf, sizeof(val_buf), c_fmt, p2c_obj_as_float(obj));
            break;
        case '%': {
            /* パーセント表示: 100倍して既定6桁（精度指定があれば優先）で小数化し、
             * 末尾へ '%' を付ける。*/
            double scaled = p2c_obj_as_float(obj) * 100.0;
            if (sign) snprintf(c_fmt, sizeof(c_fmt), "%%%c.%df", sign, precision >= 0 ? precision : 6);
            else      snprintf(c_fmt, sizeof(c_fmt), "%%.%df", precision >= 0 ? precision : 6);
            snprintf(val_buf, sizeof(val_buf), c_fmt, scaled);
            size_t plen = strlen(val_buf);
            if (plen + 1 < sizeof(val_buf)) { val_buf[plen] = '%'; val_buf[plen + 1] = '\0'; }
            break;
        }
        case 'e': case 'E':
            if (sign) snprintf(c_fmt, sizeof(c_fmt), "%%%c.%d%c", sign, precision >= 0 ? precision : 6, type_char);
            else      snprintf(c_fmt, sizeof(c_fmt), "%%.%d%c", precision >= 0 ? precision : 6, type_char);
            snprintf(val_buf, sizeof(val_buf), c_fmt, p2c_obj_as_float(obj));
            break;
        case 'g': case 'G':
            if (sign) snprintf(c_fmt, sizeof(c_fmt), "%%%c.%d%c", sign, precision >= 0 ? precision : 6, type_char);
            else      snprintf(c_fmt, sizeof(c_fmt), "%%.%d%c", precision >= 0 ? precision : 6, type_char);
            snprintf(val_buf, sizeof(val_buf), c_fmt, p2c_obj_as_float(obj));
            break;
        case 'x': case 'X':
            snprintf(c_fmt, sizeof(c_fmt), "%%%d%c", printf_width, type_char);
            snprintf(val_buf, sizeof(val_buf), c_fmt, (unsigned long long)p2c_obj_as_int(obj));
            if (alternate_form && p2c_obj_as_int(obj) != 0) {
                const char *prefix = type_char == 'X' ? "0X" : "0x";
                size_t value_len = strlen(val_buf);
                size_t prefix_len = 2;
                if (value_len + prefix_len < sizeof(val_buf)) {
                    memmove(val_buf + prefix_len, val_buf, value_len + 1);
                    memcpy(val_buf, prefix, prefix_len);
                }
            }
            break;
        case 'o':
            snprintf(c_fmt, sizeof(c_fmt), "%%%do", printf_width);
            snprintf(val_buf, sizeof(val_buf), c_fmt, (unsigned long long)p2c_obj_as_int(obj));
            if (alternate_form && p2c_obj_as_int(obj) != 0) {
                size_t value_len = strlen(val_buf);
                if (value_len + 1 < sizeof(val_buf)) {
                    memmove(val_buf + 1, val_buf, value_len + 1);
                    val_buf[0] = '0';
                }
            }
            break;
        case 'b': {
            long long iv = p2c_obj_as_int(obj);
            /* 簡易2進数変換 (最大64ビット) */
            char tmp[70] = {0}; int ti = 68;
            unsigned long long uv = (unsigned long long)iv;
            if (uv == 0) { tmp[ti--] = '0'; }
            else while (uv) { tmp[ti--] = (char)('0' + (uv & 1)); uv >>= 1; }
            const char *binstr = tmp + ti + 1;
            if (alternate_form && iv != 0) snprintf(val_buf, sizeof(val_buf), "0b%s", binstr);
            else snprintf(val_buf, sizeof(val_buf), "%s", binstr);
            break;
        }
        case 's': case 0: default: {
            /* 文字列フォーマット */
            P2C_Object *sv = p2c_obj_str(obj);
            const char *s = p2c_obj_as_str(sv);
            if (precision >= 0 && s) {
                size_t slen = strlen(s);
                size_t cut = (size_t)precision < slen ? (size_t)precision : slen;
                if (cut < sizeof(val_buf)) {
                    memcpy(val_buf, s, cut); val_buf[cut] = '\0';
                } else {
                    /* val_buf(256byte)に収まらない場合は切り詰めて安全側に倒す
                     * （幅/アラインの適用は別途malloc確保で正しく処理される）*/
                    memcpy(val_buf, s, sizeof(val_buf) - 1); val_buf[sizeof(val_buf) - 1] = '\0';
                }
            } else {
                snprintf(val_buf, sizeof(val_buf), "%s", s ? s : "");
            }
            break;
        }
    }

    /* 幅の異常値をクランプ（安全のため上限を設ける。実用上十分な大きさ） */
    if (width < 0) width = 0;
    if (width > 65536) width = 65536;

    /* zero-pad の適用 (整数かつ align='=') */
    if (zero_pad && width && align == '=' && type_char && strchr("dioxXbe", type_char)) {
        size_t vl = strlen(val_buf);
        size_t sign_chars = (val_buf[0] == '-' || val_buf[0] == '+') ? 1u : 0u;
        size_t need = (size_t)width;
        if (vl < need) {
            size_t pad = need - vl;
            char *tmp = (char*)p2c_heap_alloc(need + 1);
            if (!tmp) return p2c_obj_from_str(val_buf);
            memcpy(tmp, val_buf, sign_chars);
            for (size_t i = 0; i < pad; i++) tmp[sign_chars + i] = '0';
            memcpy(tmp + sign_chars + pad, val_buf + sign_chars, vl - sign_chars + 1);
            P2C_Object *r = p2c_obj_from_str(tmp);
            p2c_heap_free(tmp);
            return r;
        }
        return p2c_obj_from_str(val_buf);
    }

    /* 桁区切り（grouping）: ',' / '_' 指定時に整数部へ3桁ごとの区切りを挿入する。
     * CPython は 'd'/'f'/'%' と10進系にのみ ',' を許可するため、
     * 対応する型でのみ適用する。 */
    if (grouping && (type_char == 'd' || type_char == 'i' || type_char == 'f' ||
                     type_char == 'F' || type_char == '%')) {
        char grouped[320];
        const char *src = val_buf;
        size_t len = strlen(src);
        size_t int_start = 0;
        if (len > 0 && (src[0] == '+' || src[0] == '-' || src[0] == ' ')) int_start = 1;
        size_t int_end = int_start;
        while (int_end < len && src[int_end] >= '0' && src[int_end] <= '9') int_end++;
        size_t int_digits = int_end - int_start;
        size_t out = 0;
        bool fits = true;
        for (size_t i = 0; i < len; i++) {
            if (i == int_start && i > 0) {
                /* 符号はすでにコピー済み */
            }
            if (i >= int_start && i < int_end) {
                size_t remaining = int_end - i;
                if (remaining < int_digits && (remaining % 3u) == 0) {
                    if (out + 1 < sizeof(grouped)) grouped[out++] = ',';
                    else fits = false;
                }
            }
            if (out + 1 < sizeof(grouped)) grouped[out++] = src[i];
            else fits = false;
        }
        if (fits) {
            grouped[out] = '\0';
            /* 桁区切りで伸びた分は val_buf(256) に収まる範囲だけ写す。 */
            size_t copy = out < sizeof(val_buf) - 1u ? out : sizeof(val_buf) - 1u;
            memcpy(val_buf, grouped, copy);
            val_buf[copy] = '\0';
        }
    }

    /* 幅 / アライン */
    if (width > 0) {
        size_t vl = strlen(val_buf);
        size_t need = (size_t)width;
        if (vl < need) {
            size_t pad = need - vl;
            char *tmp = (char*)p2c_heap_alloc(need + 1);
            if (!tmp) return p2c_obj_from_str(val_buf);
            char eff_align = align ? align : (type_char && strchr("dioxXbef", type_char) ? '>' : '<');
            if (eff_align == '<') {
                memcpy(tmp, val_buf, vl);
                for (size_t i = 0; i < pad; i++) tmp[vl + i] = fill;
                tmp[need] = '\0';
            } else if (eff_align == '>') {
                for (size_t i = 0; i < pad; i++) tmp[i] = fill;
                memcpy(tmp + pad, val_buf, vl + 1);
            } else { /* ^ */
                size_t lpad = pad / 2, rpad = pad - lpad;
                for (size_t i = 0; i < lpad; i++) tmp[i] = fill;
                memcpy(tmp + lpad, val_buf, vl);
                for (size_t i = 0; i < rpad; i++) tmp[lpad + vl + i] = fill;
                tmp[need] = '\0';
            }
            P2C_Object *r = p2c_obj_from_str(tmp);
            p2c_heap_free(tmp);
            return r;
        }
    }
    return p2c_obj_from_str(val_buf);
}

#define P2C_BINOP_STACK_CAPACITY 64
static P2C_THREAD_LOCAL P2C_Object *g_binop_stack[P2C_BINOP_STACK_CAPACITY];
static P2C_THREAD_LOCAL size_t g_binop_depth = 0;

void p2c_binop_begin(P2C_Object *left) {
    if (g_binop_depth >= P2C_BINOP_STACK_CAPACITY) {
        p2c_raise(p2c_make_exception("RuntimeError", "binary expression nesting limit exceeded"));
        return;
    }
    g_binop_stack[g_binop_depth++] = left;
}

P2C_Object* p2c_binop_finish(P2C_BinaryOpFn op, P2C_Object *right) {
    if (g_binop_depth == 0 || !op) {
        p2c_raise(p2c_make_exception("RuntimeError", "binary expression stack is not active"));
        return &P2C_None;
    }
    return op(g_binop_stack[--g_binop_depth], right);
}

#define P2C_FSTR_STACK_CAPACITY 32
static P2C_THREAD_LOCAL P2C_String *g_fstr_stack[P2C_FSTR_STACK_CAPACITY];
static P2C_THREAD_LOCAL size_t g_fstr_depth = 0;

void p2c_fstr_begin(void) {
    if (g_fstr_depth >= P2C_FSTR_STACK_CAPACITY) {
        p2c_raise(p2c_make_exception("RuntimeError", "f-string nesting limit exceeded"));
        return;
    }
    P2C_String *builder = p2c_str_new(NULL);
    if (!builder) {
        p2c_raise(p2c_make_exception("MemoryError", "f-string allocation failed"));
        return;
    }
    g_fstr_stack[g_fstr_depth++] = builder;
}

void p2c_fstr_append(P2C_Object *obj) {
    if (g_fstr_depth == 0) {
        p2c_raise(p2c_make_exception("RuntimeError", "f-string builder is not active"));
        return;
    }
    P2C_Object *text = p2c_obj_str(obj);
    p2c_str_append(g_fstr_stack[g_fstr_depth - 1], p2c_obj_as_str(text));
}

P2C_Object* p2c_fstr_finish(void) {
    if (g_fstr_depth == 0) {
        p2c_raise(p2c_make_exception("RuntimeError", "f-string builder is not active"));
        return &P2C_None;
    }
    P2C_String *builder = g_fstr_stack[--g_fstr_depth];
    P2C_Object *out = p2c_obj_from_str(p2c_str_cstr(builder));
    p2c_str_free(builder);
    return out;
}

/* ---- 式評価中の一時値の保存/復元（例外脱出時の後始末）-------------------
 * p2c_binop_begin()/p2c_fstr_begin()が積む一時値はスレッドローカル領域に
 * あるため、例外が式の途中で脱出する（longjmpする）と解放されずに残る。
 * 生成Cのtryは、try開始時の深さを保存してハンドラ到達時にそこへ戻すことで、
 *  1. 放棄された左オペランドを取り残さない（GCのルートも収集時に整合する）、
 *  2. ネイティブ確保であるf-stringビルダを確実に解放する（リーク防止）、
 *  3. 例外が繰り返し脱出してもBINOP/FSTRスタックを使い切らない
 *     （64/32回で「nesting limit exceeded」を誤発火させない）
 * を保証する。
 *
 * 巻き戻しは冪等（保存値以下の深さでは何もしない）なので、ハンドラごとに
 * 呼んでも安全である。生成C以外（ホストが自前でlongjmpする場合）も、
 * 例外フレームへ到達した時点で同じ保存値へ戻す契約とする。 */
size_t p2c_binop_depth(void) { return g_binop_depth; }

void p2c_binop_rewind(size_t depth) {
    /* 容量を超える保存値は不正なので、スタック全体を空にする扱いにする。 */
    if (depth >= P2C_BINOP_STACK_CAPACITY) depth = 0;
    while (g_binop_depth > depth) {
        g_binop_stack[--g_binop_depth] = NULL;
    }
}

size_t p2c_fstr_depth(void) { return g_fstr_depth; }

void p2c_fstr_rewind(size_t depth) {
    if (depth >= P2C_FSTR_STACK_CAPACITY) depth = 0;
    while (g_fstr_depth > depth) {
        P2C_String *builder = g_fstr_stack[--g_fstr_depth];
        g_fstr_stack[g_fstr_depth] = NULL;
        if (builder) p2c_str_free(builder);
    }
}

/* GCのmarkフェーズから呼ばれ、スタック上のどこからも参照されていない
 * TLS一時値（式評価中の左オペランドと処理中の例外）をルート化する。
 * ルート化した個数を返し、p2c_gc_last_temp_roots()で観測できる。 */
static size_t gc_mark_tls_temporaries(void) {
    size_t count = 0;
    if (p2c_active_exception) {
        gc_mark(p2c_active_exception, NULL);
        count++;
    }
    for (size_t i = 0; i < g_binop_depth && i < P2C_BINOP_STACK_CAPACITY; i++) {
        if (g_binop_stack[i]) {
            gc_mark(g_binop_stack[i], NULL);
            count++;
        }
    }
    return count;
}

P2C_Object* p2c_format_fixed(P2C_Object *obj, P2C_Object *ndigits) {
    char fmt[16];
    int n = (int)p2c_obj_as_int(ndigits);
    if (n < 0) n = 0;
    if (n > 17) n = 17;
    snprintf(fmt, sizeof(fmt), "%%.%df", n);
    char buf[128];
    snprintf(buf, sizeof(buf), fmt, p2c_obj_as_float(obj));
    return p2c_obj_from_str(buf);
}

/* Pythonのスライス s[start:stop:step] 相当。list/tuple/str に対応。
 * start/stop/step は省略時 &P2C_None が渡ってくる想定。
 * Pythonの境界クランプ・負数インデックス・負のstepの挙動に合わせている。 */
P2C_Object* p2c_obj_slice(P2C_Object *obj, P2C_Object *start, P2C_Object *stop, P2C_Object *step) {
    if (!obj || !obj->cls) return &P2C_None;
    bool is_str = (obj->cls->type_tag == OBJ_STR);
    bool is_tuple = (obj->cls->type_tag == OBJ_TUPLE);
    bool is_list = (obj->cls->type_tag == OBJ_LIST);
    if (!is_str && !is_tuple && !is_list) { p2c_raise(p2c_make_exception("TypeError", "object is not sliceable")); return &P2C_None; }

    int64_t len = (int64_t)(is_str ? obj->u.v_str.len : (is_tuple ? obj->u.v_tuple.len : obj->u.v_list.len));
    int64_t st = 1;
    if (step && step != &P2C_None && !p2c_obj_index_value(step, &st)) return &P2C_None;
    if (st == 0) { p2c_raise(p2c_make_exception("ValueError", "slice step cannot be zero")); return &P2C_None; }

    int64_t s_val, e_val;
    if (st > 0) { s_val = 0; e_val = len; } else { s_val = len - 1; e_val = -1; }
    bool has_start = start && start != &P2C_None;
    bool has_stop = stop && stop != &P2C_None;
    if (has_start) {
        if (!p2c_obj_index_value(start, &s_val)) return &P2C_None;
        if (s_val < 0) s_val += len;
        if (st > 0) { if (s_val < 0) s_val = 0; if (s_val > len) s_val = len; }
        else { if (s_val < -1) s_val = -1; if (s_val >= len) s_val = len - 1; }
    }
    if (has_stop) {
        if (!p2c_obj_index_value(stop, &e_val)) return &P2C_None;
        if (e_val < 0) e_val += len;
        if (st > 0) { if (e_val < 0) e_val = 0; if (e_val > len) e_val = len; }
        else { if (e_val < -1) e_val = -1; if (e_val >= len) e_val = len - 1; }
    }

    if (is_str) {
        P2C_String *buf = p2c_str_new(NULL);
        if (st > 0) { for (int64_t i = s_val; i < e_val; i += st) { char c1[2] = { obj->u.v_str.data[i], '\0' }; p2c_str_append(buf, c1); } }
        else { for (int64_t i = s_val; i > e_val; i += st) { char c1[2] = { obj->u.v_str.data[i], '\0' }; p2c_str_append(buf, c1); } }
        P2C_Object *out = p2c_obj_from_str(p2c_str_cstr(buf));
        p2c_str_free(buf);
        return out;
    }
    P2C_Object **items = is_tuple ? obj->u.v_tuple.items : obj->u.v_list.items;
    if (is_tuple) {
        size_t cap = (st > 0) ? (size_t)((e_val - s_val + st - 1) / st > 0 ? (e_val - s_val + st - 1) / st : 0)
                               : (size_t)((s_val - e_val + (-st) - 1) / (-st) > 0 ? (s_val - e_val + (-st) - 1) / (-st) : 0);
        if (cap == 0) return p2c_tuple_from_array(NULL, 0);
        P2C_Object **buf = (P2C_Object**)p2c_malloc_checked(cap * sizeof(P2C_Object*), "call args");
        if (!buf) {
            p2c_raise(p2c_make_exception("MemoryError", "tuple slice allocation failed"));
            return &P2C_None;
        }
        size_t n = 0;
        if (st > 0) { for (int64_t i = s_val; i < e_val; i += st) buf[n++] = items[i]; }
        else { for (int64_t i = s_val; i > e_val; i += st) buf[n++] = items[i]; }
        P2C_Object *out_tuple = p2c_obj_new(&P2C_Class_Tuple);
        out_tuple->u.v_tuple.items = buf;
        out_tuple->u.v_tuple.len = n;
        return out_tuple;
    }
    P2C_Object *out_list = p2c_list_new();
    if (st > 0) { for (int64_t i = s_val; i < e_val; i += st) p2c_list_append(out_list, items[i]); }
    else { for (int64_t i = s_val; i > e_val; i += st) p2c_list_append(out_list, items[i]); }
    return out_list;
}

void p2c_slice_assign(P2C_Object *obj, P2C_Object *start, P2C_Object *stop, P2C_Object *step, P2C_Object *value) {
    int64_t len, s, e, st = 1, i;
    P2C_Object **rhs, **rhs_copy = NULL;
    size_t rhs_len, selected = 0, pos = 0;
    if (!obj || !p2c_obj_is_list(obj)) {
        p2c_raise(p2c_make_exception("TypeError", "slice assignment requires a list"));
        return;
    }
    if (!value || (!p2c_obj_is_list(value) && !p2c_obj_is_tuple(value))) {
        p2c_raise(p2c_make_exception("TypeError", "can only assign an iterable to a list slice"));
        return;
    }
    if (step && step != &P2C_None && !p2c_obj_index_value(step, &st)) return;
    if (st == 0) {
        p2c_raise(p2c_make_exception("ValueError", "slice step cannot be zero"));
        return;
    }
    len = (int64_t)obj->u.v_list.len;
    s = st > 0 ? 0 : len - 1;
    e = st > 0 ? len : -1;
    if (start && start != &P2C_None) {
        if (!p2c_obj_index_value(start, &s)) return;
        if (s < 0) s += len;
        if (st > 0) { if (s < 0) s = 0; if (s > len) s = len; }
        else { if (s < -1) s = -1; if (s >= len) s = len - 1; }
    }
    if (stop && stop != &P2C_None) {
        if (!p2c_obj_index_value(stop, &e)) return;
        if (e < 0) e += len;
        if (st > 0) { if (e < 0) e = 0; if (e > len) e = len; }
        else { if (e < -1) e = -1; if (e >= len) e = len - 1; }
    }
    rhs = p2c_obj_is_list(value) ? value->u.v_list.items : value->u.v_tuple.items;
    rhs_len = p2c_obj_is_list(value) ? value->u.v_list.len : value->u.v_tuple.len;
    /* a[i:j] = a のように右辺と代入先が同じlistの場合、memmove/reallocで
     * 右辺が壊れないよう先にスナップショットを取る。 */
    if (value == obj && rhs_len) {
        rhs_copy = (P2C_Object**)p2c_malloc_checked(rhs_len * sizeof(P2C_Object*), "call rhs copy");
        if (!rhs_copy) { p2c_raise(p2c_make_exception("MemoryError", "slice assignment snapshot failed")); return; }
        memcpy(rhs_copy, rhs, rhs_len * sizeof(P2C_Object*));
        rhs = rhs_copy;
    }
    if (st == 1) {
        size_t first = (size_t)s, last = (size_t)e, old_count = last >= first ? last - first : 0;
        size_t new_len = obj->u.v_list.len - old_count + rhs_len;
        if (new_len > obj->u.v_list.cap) {
            size_t cap = obj->u.v_list.cap ? obj->u.v_list.cap : 8;
            while (cap < new_len) cap *= 2;
            P2C_Object **grown = (P2C_Object**)p2c_realloc_checked(obj->u.v_list.items, cap * sizeof(P2C_Object*), "list extend growth");
            if (!grown) { p2c_heap_free(rhs_copy); p2c_raise(p2c_make_exception("MemoryError", "slice assignment allocation failed")); return; }
            obj->u.v_list.items = grown; obj->u.v_list.cap = cap;
        }
        if (rhs_len != old_count) {
            memmove(obj->u.v_list.items + first + rhs_len,
                    obj->u.v_list.items + last,
                    (obj->u.v_list.len - last) * sizeof(P2C_Object*));
        }
        if (rhs_len) memcpy(obj->u.v_list.items + first, rhs, rhs_len * sizeof(P2C_Object*));
        obj->u.v_list.len = new_len;
        p2c_heap_free(rhs_copy);
        return;
    }
    if (st > 0) for (i = s; i < e; i += st) selected++;
    else for (i = s; i > e; i += st) selected++;
    if (selected != rhs_len) {
        p2c_heap_free(rhs_copy);
        p2c_raise(p2c_make_exception("ValueError", "attempt to assign sequence of size different from extended slice"));
        return;
    }
    if (st > 0) for (i = s; i < e; i += st) obj->u.v_list.items[i] = rhs[pos++];
    else for (i = s; i > e; i += st) obj->u.v_list.items[i] = rhs[pos++];
    p2c_heap_free(rhs_copy);
}

void p2c_slice_delete(P2C_Object *obj, P2C_Object *start, P2C_Object *stop, P2C_Object *step) {
    int64_t len, s, e, st = 1, i;
    bool *remove;
    size_t write_index = 0;
    if (!obj || !p2c_obj_is_list(obj)) {
        p2c_raise(p2c_make_exception("TypeError", "slice deletion requires a list"));
        return;
    }
    if (step && step != &P2C_None && !p2c_obj_index_value(step, &st)) return;
    if (st == 0) {
        p2c_raise(p2c_make_exception("ValueError", "slice step cannot be zero"));
        return;
    }
    len = (int64_t)obj->u.v_list.len;
    s = st > 0 ? 0 : len - 1;
    e = st > 0 ? len : -1;
    if (start && start != &P2C_None) {
        if (!p2c_obj_index_value(start, &s)) return;
        if (s < 0) s += len;
        if (st > 0) { if (s < 0) s = 0; if (s > len) s = len; }
        else { if (s < -1) s = -1; if (s >= len) s = len - 1; }
    }
    if (stop && stop != &P2C_None) {
        if (!p2c_obj_index_value(stop, &e)) return;
        if (e < 0) e += len;
        if (st > 0) { if (e < 0) e = 0; if (e > len) e = len; }
        else { if (e < -1) e = -1; if (e >= len) e = len - 1; }
    }
    if (len <= 0) return;
    /* 連続スライスはmemmoveだけで削除でき、補助ビットマップを必要としない。 */
    if (st == 1) {
        size_t first = (size_t)s, last = (size_t)e;
        if (last > first) {
            memmove(obj->u.v_list.items + first, obj->u.v_list.items + last,
                    (obj->u.v_list.len - last) * sizeof(P2C_Object*));
            obj->u.v_list.len -= last - first;
        }
        return;
    }
    remove = (bool*)p2c_heap_calloc((size_t)len, sizeof(bool));
    if (!remove) { p2c_raise(p2c_make_exception("MemoryError", "slice deletion allocation failed")); return; }
    if (st > 0) for (i = s; i < e; i += st) remove[i] = true;
    else for (i = s; i > e; i += st) remove[i] = true;
    for (i = 0; i < len; i++) if (!remove[i]) obj->u.v_list.items[write_index++] = obj->u.v_list.items[i];
    obj->u.v_list.len = write_index;
    p2c_heap_free(remove);
}

P2C_Object* p2c_builtin_enumerate(P2C_Object *iterable, P2C_Object *start) {
    size_t n = 0; bool owned = false;
    P2C_Object **items = p2c_iter_items(iterable, &n, &owned);
    P2C_Object *out = p2c_list_new();
    int64_t first_index = p2c_obj_as_int(start);
    for (size_t i = 0; i < n; i++) {
        P2C_Object *pair = p2c_obj_new(&P2C_Class_Tuple);
        pair->u.v_tuple.items = (P2C_Object**)p2c_malloc_checked(2 * sizeof(P2C_Object*), "pair items");
        pair->u.v_tuple.items[0] = p2c_obj_from_int(first_index + (int64_t)i);
        pair->u.v_tuple.items[1] = items[i];
        pair->u.v_tuple.len = 2;
        p2c_list_append(out, pair);
    }
    if (owned) p2c_heap_free(items);
    return out;
}

P2C_Object* p2c_builtin_zip(P2C_Object **args, size_t nargs) {
    P2C_Object *out = p2c_list_new();
    if (nargs == 0) return out;
    size_t *lens = (size_t*)p2c_malloc_checked(nargs * sizeof(size_t), "varargs lengths");
    P2C_Object ***items = (P2C_Object***)p2c_malloc_checked(nargs * sizeof(P2C_Object**), "varargs slots");
    bool *owned = (bool*)p2c_malloc_checked(nargs * sizeof(bool), "varargs ownership");
    size_t min_len = (size_t)-1;
    for (size_t a = 0; a < nargs; a++) {
        items[a] = p2c_iter_items(args[a], &lens[a], &owned[a]);
        if (lens[a] < min_len) min_len = lens[a];
    }
    for (size_t i = 0; i < min_len; i++) {
        P2C_Object *tup = p2c_obj_new(&P2C_Class_Tuple);
        tup->u.v_tuple.items = (P2C_Object**)p2c_malloc_checked(nargs * sizeof(P2C_Object*), "varargs tuple items");
        for (size_t a = 0; a < nargs; a++) tup->u.v_tuple.items[a] = items[a][i];
        tup->u.v_tuple.len = nargs;
        p2c_list_append(out, tup);
    }
    for (size_t a = 0; a < nargs; a++) if (owned[a]) p2c_heap_free(items[a]);
    p2c_heap_free(owned);
    p2c_heap_free(lens);
    p2c_heap_free(items);
    return out;
}

P2C_Object* p2c_obj_str(P2C_Object *obj) {
    if (obj && p2c_obj_is_str(obj)) return obj; /* str(s)はsそのもの */
    P2C_String *buf = p2c_str_new(NULL);
    if (!buf) return p2c_obj_from_str("");
    p2c_obj_to_buf(obj, buf);
    P2C_Object *out = p2c_obj_from_str(p2c_str_cstr(buf));
    p2c_str_free(buf);
    return out;
}
/* repr()。str()と違い文字列自身に対しても引用符付きの表現を返す
 * （repr('hi') == "'hi'"）。それ以外の型はp2c_obj_to_buf_exにrepr=trueで
 * 委譲し、__repr__優先のディスパッチをそのまま再利用する。 */
P2C_Object* p2c_obj_repr(P2C_Object *obj) {
    P2C_String *buf = p2c_str_new(NULL);
    if (!buf) return p2c_obj_from_str("");
    p2c_obj_to_buf_ex(obj, buf, true);
    P2C_Object *out = p2c_obj_from_str(p2c_str_cstr(buf));
    p2c_str_free(buf);
    return out;
}

static bool p2c_obj_less(P2C_Object *a, P2C_Object *b);

/* タプル/リスト同士の辞書式比較（Pythonの系列比較規則）。
 * 先頭から要素ごとに比べ、最初に大小がついた要素で決まる。共通部分が等しく
 * 長さが違う場合は短い方が小さい。
 * 以前はタプルを整数として扱っていたため、sorted([(2, 1), (1, 1)]) や
 * min((a, b) for ...) が黙って「比較していない」結果を返していた。 */
static bool p2c_sequence_less(P2C_Object *a, P2C_Object *b) {
    int64_t na = p2c_len(a);
    int64_t nb = p2c_len(b);
    int64_t n = na < nb ? na : nb;
    for (int64_t i = 0; i < n; i++) {
        P2C_Object *ea = p2c_iter_at(a, i);
        P2C_Object *eb = p2c_iter_at(b, i);
        if (p2c_obj_less(ea, eb)) return true;
        if (p2c_obj_less(eb, ea)) return false;
    }
    return na < nb;
}

/* min/max/sortedで使う比較。数値・文字列・系列（タプル/リスト）に対応する。
 * （p2c_obj_lt等は数値専用のため、文字列同士の比較では常にfalseになってしまう。） */
static bool p2c_obj_less(P2C_Object *a, P2C_Object *b) {
    bool a_seq, b_seq, a_num, b_num;
    if (a == b) return false;
    if (a && b && p2c_obj_is_str(a) && p2c_obj_is_str(b)) {
        return strcmp(p2c_obj_as_str(a), p2c_obj_as_str(b)) < 0;
    }
    a_seq = a && (p2c_obj_is_tuple(a) || p2c_obj_is_list(a));
    b_seq = b && (p2c_obj_is_tuple(b) || p2c_obj_is_list(b));
    if (a_seq && b_seq) return p2c_sequence_less(a, b);
    a_num = a && (p2c_obj_is_int(a) || p2c_obj_is_float(a) || p2c_obj_is_bool(a));
    b_num = b && (p2c_obj_is_int(b) || p2c_obj_is_float(b) || p2c_obj_is_bool(b));
    if (a_num && b_num) {
        return (p2c_obj_is_float(a) || p2c_obj_is_float(b)) ? (p2c_obj_as_float(a) < p2c_obj_as_float(b)) : (p2c_obj_as_int(a) < p2c_obj_as_int(b));
    }
    /* 数値・文字列・系列以外（None、dict、インスタンス等）はPython同様に順序を
     * 持たない。以前は整数0として比較しており、sorted()が黙って未ソートの結果を
     * 返していたため、CPythonと同じくTypeErrorにする。 */
    p2c_raise(p2c_make_exception("TypeError", "'<' not supported between instances of these types"));
    return false;
}

P2C_Object* p2c_obj_abs(P2C_Object *obj) {
    if (p2c_obj_is_float(obj)) { double v = p2c_obj_as_float(obj); return p2c_obj_from_float(v < 0 ? -v : v); }
    int64_t v = p2c_obj_as_int(obj);
    return p2c_obj_from_int(v < 0 ? -v : v);
}

static P2C_Object** p2c_iter_items(P2C_Object *obj, size_t *out_n, bool *out_owned) {
    /* out_owned: 返した配列が新規mallocされたもの(呼び出し側でfreeが必要)か、
     * 既存オブジェクトのバッキング配列をそのまま指しているだけ(freeしては
     * いけない)かを呼び出し側に伝える。以前はこの区別がなく、dict等の
     * 新規実体化パスで確保した配列がどの呼び出し元でも解放されず、
     * dictをsum()/sorted()/zip()等に渡すたびに小さなリークが発生していた。 */
    *out_owned = false;
    if (obj && p2c_obj_is_list(obj)) { *out_n = obj->u.v_list.len; return obj->u.v_list.items; }
    if (obj && p2c_obj_is_str(obj)) {
        size_t n = obj->u.v_str.len;
        P2C_Object **arr = n ? (P2C_Object**)p2c_malloc_checked(n * sizeof(P2C_Object*), "sorted copy") : NULL;
        if (arr) {
            for (size_t i = 0; i < n; i++) arr[i] = p2c_obj_from_str_n(obj->u.v_str.data + i, 1);
        }
        *out_n = arr ? n : 0;
        *out_owned = (arr != NULL);
        return arr;
    }
    if (obj && obj->cls && obj->cls->type_tag == OBJ_TUPLE) { *out_n = obj->u.v_tuple.len; return obj->u.v_tuple.items; }
    if (obj && obj->cls && (obj->cls->type_tag == OBJ_DICT || obj->cls->type_tag == OBJ_SET)) {
        size_t n = obj->u.v_dict.len;
        P2C_Object **arr = n ? (P2C_Object**)p2c_malloc_checked(n * sizeof(P2C_Object*), "sorted copy") : NULL;
        size_t idx = 0;
        if (arr) {
            for (P2C_DictEntry *e = obj->u.v_dict.order_head; e && idx < n; e = e->order_next) arr[idx++] = e->key;
        }
        *out_n = arr ? idx : 0;
        *out_owned = (arr != NULL);
        return arr;
    }
    if (obj && obj->cls && obj->cls->type_tag == OBJ_ITERATOR) {
        P2C_Object *iterator = p2c_builtin_iter(obj);
        P2C_Object **volatile arr = NULL;
        volatile size_t count = 0;
        volatile size_t cap = 0;
        for (;;) {
            P2C_ExceptFrame frame;
            P2C_Object *item = NULL;
            frame.prev = p2c_exc_stack;
            frame.exc = NULL;
            p2c_exc_stack = &frame;
            if (P2C_SETJMP(frame.env) != 0) {
                P2C_Object *exc = frame.exc;
                p2c_exc_stack = frame.prev;
                if (!p2c_exc_name_match(exc, "StopIteration")) {
                    if (arr) p2c_heap_free((void*)arr);
                    p2c_raise(exc);
                    *out_n = 0;
                    return NULL;
                }
                break;
            }
            item = p2c_builtin_next(iterator);
            p2c_exc_stack = frame.prev;
            if (count == cap) {
                size_t next_cap = cap ? (size_t)cap * 2U : 8U;
                P2C_Object **grown = (P2C_Object**)p2c_realloc_checked((void*)arr, next_cap * sizeof(P2C_Object*), "sorted growth");
                if (!grown) {
                    if (arr) p2c_heap_free((void*)arr);
                    p2c_raise(p2c_make_exception("MemoryError", "iterator materialization failed"));
                    *out_n = 0;
                    return NULL;
                }
                arr = grown;
                cap = next_cap;
            }
            arr[count++] = item;
        }
        *out_n = (size_t)count;
        *out_owned = (arr != NULL);
        return (P2C_Object**)arr;
    }
    if (obj && obj->cls && obj->cls->type_tag == OBJ_INSTANCE && p2c_has_method(obj, "__len__")) {
        /* __len__(+__getitem__)を実装するインスタンスも同様に実体化する。 */
        int64_t n = p2c_len(obj);
        P2C_Object **arr = n > 0 ? (P2C_Object**)p2c_malloc_checked((size_t)n * sizeof(P2C_Object*), "reversed copy") : NULL;
        if (arr) { for (int64_t i = 0; i < n; i++) arr[i] = p2c_subscript_get(obj, p2c_obj_from_int(i)); }
        *out_n = arr ? (size_t)n : 0;
        *out_owned = (arr != NULL);
        return arr;
    }
    *out_n = 0;
    return NULL;
}

static P2C_Object* p2c_builtin_len_callable(P2C_Object **args, size_t nargs) {
    if (nargs != 1) {
        p2c_raise(p2c_make_exception("TypeError", "len() takes exactly one argument"));
        return &P2C_None;
    }
    return p2c_obj_from_int(p2c_len(args[0]));
}

static P2C_Object* p2c_builtin_abs_callable(P2C_Object **args, size_t nargs) {
    if (nargs != 1) {
        p2c_raise(p2c_make_exception("TypeError", "abs() takes exactly one argument"));
        return &P2C_None;
    }
    return p2c_obj_abs(args[0]);
}

P2C_Object* p2c_builtin_function_object(const char *name) {
    if (name && strcmp(name, "len") == 0) return p2c_function_new("len", p2c_builtin_len_callable);
    if (name && strcmp(name, "abs") == 0) return p2c_function_new("abs", p2c_builtin_abs_callable);
    p2c_raise(p2c_make_exception("NameError", name ? name : "<builtin>"));
    return &P2C_None;
}

static P2C_Object* p2c_builtin_extreme_key(P2C_Object **args, size_t nargs, P2C_Object *key, bool want_min) {
    P2C_Object **items = args;
    size_t n = nargs;
    bool owned = false;
    if (nargs == 1) {
        size_t in = 0;
        P2C_Object **it = p2c_iter_items(args[0], &in, &owned);
        if (it) { items = it; n = in; }
    }
    if (n == 0) {
        p2c_raise(p2c_make_exception("ValueError", want_min ? "min() arg is an empty sequence" : "max() arg is an empty sequence"));
        return &P2C_None;
    }
    P2C_Object *best = items[0];
    P2C_Object *best_key = key && !p2c_obj_is_none(key) ? p2c_call(key, &best, 1) : best;
    for (size_t i = 1; i < n; i++) {
        P2C_Object *candidate = items[i];
        P2C_Object *candidate_key = key && !p2c_obj_is_none(key) ? p2c_call(key, &candidate, 1) : candidate;
        bool replace = want_min ? p2c_obj_less(candidate_key, best_key) : p2c_obj_less(best_key, candidate_key);
        if (replace) { best = candidate; best_key = candidate_key; }
    }
    if (owned) p2c_heap_free(items);
    return best;
}

P2C_Object* p2c_builtin_min(P2C_Object **args, size_t nargs) { return p2c_builtin_extreme_key(args, nargs, &P2C_None, true); }
P2C_Object* p2c_builtin_min_key(P2C_Object **args, size_t nargs, P2C_Object *key) { return p2c_builtin_extreme_key(args, nargs, key, true); }
P2C_Object* p2c_builtin_max(P2C_Object **args, size_t nargs) { return p2c_builtin_extreme_key(args, nargs, &P2C_None, false); }
P2C_Object* p2c_builtin_max_key(P2C_Object **args, size_t nargs, P2C_Object *key) { return p2c_builtin_extreme_key(args, nargs, key, false); }
P2C_Object* p2c_builtin_sum(P2C_Object **args, size_t nargs) {
    if (nargs < 1 || nargs > 2) {
        p2c_raise(p2c_make_exception("TypeError", "sum() takes one or two arguments"));
        return &P2C_None;
    }
    size_t n = 0; bool owned = false;
    P2C_Object **items = p2c_iter_items(args[0], &n, &owned);
    P2C_Object *acc = nargs == 2 ? args[1] : p2c_obj_from_int(0);
    for (size_t i = 0; i < n; i++) acc = p2c_obj_add(acc, items[i]);
    if (owned) p2c_heap_free(items);
    return acc;
}

P2C_Object* p2c_builtin_ord(P2C_Object *obj) {
    const unsigned char *s;
    size_t len;
    uint32_t code;
    size_t width;
    if (!obj || !obj->cls || obj->cls->type_tag != OBJ_STR) {
        p2c_raise(p2c_make_exception("TypeError", "ord() expected string of length 1"));
        return &P2C_None;
    }
    s = (const unsigned char*)obj->u.v_str.data;
    len = obj->u.v_str.len;
    if (len == 0) {
        p2c_raise(p2c_make_exception("TypeError", "ord() expected a character, but string of length 0 found"));
        return &P2C_None;
    }
    if (s[0] < 0x80u) { code = s[0]; width = 1; }
    else if ((s[0] & 0xe0u) == 0xc0u && len >= 2 && (s[1] & 0xc0u) == 0x80u) { code = ((uint32_t)(s[0] & 0x1fu) << 6) | (uint32_t)(s[1] & 0x3fu); width = 2; }
    else if ((s[0] & 0xf0u) == 0xe0u && len >= 3 && (s[1] & 0xc0u) == 0x80u && (s[2] & 0xc0u) == 0x80u) { code = ((uint32_t)(s[0] & 0x0fu) << 12) | ((uint32_t)(s[1] & 0x3fu) << 6) | (uint32_t)(s[2] & 0x3fu); width = 3; }
    else if ((s[0] & 0xf8u) == 0xf0u && len >= 4 && (s[1] & 0xc0u) == 0x80u && (s[2] & 0xc0u) == 0x80u && (s[3] & 0xc0u) == 0x80u) { code = ((uint32_t)(s[0] & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3fu) << 12) | ((uint32_t)(s[2] & 0x3fu) << 6) | (uint32_t)(s[3] & 0x3fu); width = 4; }
    else {
        p2c_raise(p2c_make_exception("ValueError", "ord() expected a valid UTF-8 character"));
        return &P2C_None;
    }
    if (width != len || (width == 2 && code < 0x80u) || (width == 3 && code < 0x800u) ||
        (width == 4 && (code < 0x10000u || code > 0x10ffffu)) || (code >= 0xd800u && code <= 0xdfffu)) {
        p2c_raise(p2c_make_exception("TypeError", "ord() expected a character, but string of length is not 1"));
        return &P2C_None;
    }
    return p2c_obj_from_int((int64_t)code);
}

P2C_Object* p2c_builtin_chr(P2C_Object *obj) {
    int64_t value = p2c_obj_as_int(obj);
    char utf8[4];
    size_t len;
    if (value < 0 || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
        p2c_raise(p2c_make_exception("ValueError", "chr() arg not in range(0x110000)"));
        return &P2C_None;
    }
    if (value < 0x80) { utf8[0] = (char)value; len = 1; }
    else if (value < 0x800) { utf8[0] = (char)(0xc0 | (value >> 6)); utf8[1] = (char)(0x80 | (value & 0x3f)); len = 2; }
    else if (value < 0x10000) { utf8[0] = (char)(0xe0 | (value >> 12)); utf8[1] = (char)(0x80 | ((value >> 6) & 0x3f)); utf8[2] = (char)(0x80 | (value & 0x3f)); len = 3; }
    else { utf8[0] = (char)(0xf0 | (value >> 18)); utf8[1] = (char)(0x80 | ((value >> 12) & 0x3f)); utf8[2] = (char)(0x80 | ((value >> 6) & 0x3f)); utf8[3] = (char)(0x80 | (value & 0x3f)); len = 4; }
    return p2c_obj_from_str_n(utf8, len);
}

P2C_Object* p2c_builtin_int_base(P2C_Object *obj, unsigned base, const char *prefix) {
    static const char digits[] = "0123456789abcdef";
    char reversed[66];
    char out[70];
    int64_t value = p2c_obj_as_int(obj);
    uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
    size_t count = 0;
    size_t pos = 0;
    if (base < 2 || base > 16 || !prefix) {
        p2c_raise(p2c_make_exception("ValueError", "invalid integer base"));
        return &P2C_None;
    }
    do {
        reversed[count++] = digits[magnitude % base];
        magnitude /= base;
    } while (magnitude != 0);
    if (value < 0) out[pos++] = '-';
    for (size_t i = 0; prefix[i] != '\0'; i++) out[pos++] = prefix[i];
    while (count > 0) out[pos++] = reversed[--count];
    return p2c_obj_from_str_n(out, pos);
}
/* キー列を使った安定マージソートの再帰部分。items/keysのlo..hi-1をソートする。
 * 比較関数はp2c_obj_less（順序を持たない型ではTypeErrorを送出する）。 */
static void p2c_merge_sort_run(P2C_Object **items, P2C_Object **keys, P2C_Object **tmp_items, P2C_Object **tmp_keys, size_t lo, size_t hi, bool descending) {
    size_t mid, i, j, k;
    if (hi - lo < 2) return;
    mid = lo + (hi - lo) / 2;
    p2c_merge_sort_run(items, keys, tmp_items, tmp_keys, lo, mid, descending);
    p2c_merge_sort_run(items, keys, tmp_items, tmp_keys, mid, hi, descending);
    i = lo; j = mid; k = lo;
    while (i < mid && j < hi) {
        /* 右の要素が「より小さい」ときだけ右を先に取り、それ以外は左を先に取る。
         * これで昇順・降順いずれでも同じキーの相対順序が保たれる（安定）。 */
        bool take_left = descending ? !p2c_obj_less(keys[i], keys[j]) : !p2c_obj_less(keys[j], keys[i]);
        if (take_left) { tmp_items[k] = items[i]; tmp_keys[k] = keys[i]; i++; }
        else { tmp_items[k] = items[j]; tmp_keys[k] = keys[j]; j++; }
        k++;
    }
    while (i < mid) { tmp_items[k] = items[i]; tmp_keys[k] = keys[i]; i++; k++; }
    while (j < hi) { tmp_items[k] = items[j]; tmp_keys[k] = keys[j]; j++; k++; }
    for (k = lo; k < hi; k++) { items[k] = tmp_items[k]; keys[k] = tmp_keys[k]; }
}

/* sorted()/list.sort()の並べ替え。以前は挿入ソート（O(n^2)）だったため、
 * 要素数の多いリストでは実用時間に収まらなかった。CPythonと同じ安定な
 * マージソート（O(n log n)）に置き換える。 */
static void p2c_stable_sort(P2C_Object **items, P2C_Object **keys, size_t n, bool descending) {
    P2C_Object **tmp_items;
    P2C_Object **tmp_keys;
    if (n < 2) return;
    tmp_items = (P2C_Object**)p2c_malloc_checked(n * sizeof(P2C_Object*), "sort items");
    tmp_keys = (P2C_Object**)p2c_malloc_checked(n * sizeof(P2C_Object*), "sort keys");
    if (!tmp_items || !tmp_keys) {
        p2c_heap_free(tmp_items);
        p2c_heap_free(tmp_keys);
        p2c_raise(p2c_make_exception("MemoryError", "sorted() scratch allocation failed"));
        return;
    }
    p2c_merge_sort_run(items, keys, tmp_items, tmp_keys, 0, n, descending);
    p2c_heap_free(tmp_items);
    p2c_heap_free(tmp_keys);
}

P2C_Object* p2c_builtin_sorted_key(P2C_Object *iterable, P2C_Object *key, P2C_Object *reverse) {
    size_t n = 0;
    bool owned = false;
    P2C_Object **items = p2c_iter_items(iterable, &n, &owned);
    P2C_Object *out = p2c_list_new();
    P2C_Object **tmp = n ? (P2C_Object**)p2c_malloc_checked(n * sizeof(P2C_Object*), "set scratch") : NULL;
    P2C_Object **keys = n ? (P2C_Object**)p2c_malloc_checked(n * sizeof(P2C_Object*), "set keys") : NULL;
    if (n > 0 && (!tmp || !keys)) {
        p2c_heap_free(tmp);
        p2c_heap_free(keys);
        if (owned) p2c_heap_free(items);
        p2c_raise(p2c_make_exception("MemoryError", "sorted() key storage allocation failed"));
        return &P2C_None;
    }
    for (size_t i = 0; i < n; i++) {
        tmp[i] = items[i];
        keys[i] = key && !p2c_obj_is_none(key) ? p2c_call(key, &tmp[i], 1) : tmp[i];
    }
    p2c_stable_sort(tmp, keys, n, reverse && p2c_obj_is_truthy(reverse));
    for (size_t i = 0; i < n; i++) p2c_list_append(out, tmp[i]);
    p2c_heap_free(tmp);
    p2c_heap_free(keys);
    if (owned) p2c_heap_free(items);
    return out;
}

P2C_Object* p2c_builtin_sorted(P2C_Object *iterable) { return p2c_builtin_sorted_key(iterable, &P2C_None, &P2C_False); }
P2C_Object* p2c_builtin_sorted_rev(P2C_Object *iterable, P2C_Object *reverse) { return p2c_builtin_sorted_key(iterable, &P2C_None, reverse); }

P2C_Object* p2c_builtin_reversed(P2C_Object *iterable) {
    size_t n = 0;
    bool owned = false;
    P2C_Object **items = p2c_iter_items(iterable, &n, &owned);
    P2C_Object *out = p2c_list_new();
    if (!out) {
        if (owned) p2c_heap_free(items);
        return NULL;
    }
    for (size_t i = n; i > 0; i--) p2c_list_append(out, items[i - 1]);
    if (owned) p2c_heap_free(items);
    return out;
}

/* round(x) / round(x, ndigits) */
P2C_Object* p2c_builtin_round(P2C_Object *x, P2C_Object *ndigits) {
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
    double v = p2c_obj_as_float(x);
    if (p2c_obj_is_none(ndigits)) {
        /* round(x) -> int。Pythonは「最近偶数への丸め」(banker's rounding)。
         * 0.5ちょうどのケースだけ偶数側に倒す処理を明示的に行う。 */
        double floor_v = floor(v);
        double diff = v - floor_v;
        int64_t r;
        if (diff < 0.5) {
            r = (int64_t)floor_v;
        } else if (diff > 0.5) {
            r = (int64_t)floor_v + 1;
        } else {
            /* ちょうど0.5: 偶数側へ */
            int64_t fl = (int64_t)floor_v;
            r = (fl % 2 == 0) ? fl : fl + 1;
        }
        return p2c_obj_from_int(r);
    }
    /* round(x, ndigits) -> float */
    int64_t nd = p2c_obj_as_int(ndigits);
    /* %.*f の出力長は「値の整数部（最大約309桁）+ 小数点 + 精度」で決まるため、
     * 精度を現実的な上限へ制限したうえで十分なバッファを確保する
     * （CPythonのroundも桁数を制限している）。 */
    char fmt[512];
    int64_t precision = nd > 0 ? nd : 0;
    if (precision > 64) precision = 64;
    snprintf(fmt, sizeof(fmt), "%.*f", (int)precision, v);
    return p2c_obj_from_float(strtod(fmt, NULL));
#else
    /* freestandingではround()を使えないため、0桁丸めの近似実装にする
     * （関数呼び出し結果を直接キャストすると -Wbad-function-cast になるため
     * 一度変数へ受ける）。 */
    (void)ndigits;
    double value = p2c_obj_as_float(x);
    return p2c_obj_from_int((int64_t)value);
#endif
}

/* divmod(a, b) — Pythonの商と剰余の組。
 * int同士は既存のfloor除算/剰余（Python意味論）をそのまま使い、
 * floatが混ざる場合はCPythonのfloat_divmodと同じ補正を行う。 */
P2C_Object* p2c_builtin_divmod(P2C_Object *a, P2C_Object *b) {
    P2C_Object *quotient;
    P2C_Object *remainder;
    if (p2c_obj_is_int(a) && p2c_obj_is_int(b)) {
        quotient = p2c_obj_floordiv(a, b);
        remainder = p2c_obj_mod(a, b);
    } else {
        double x = p2c_obj_as_float(a);
        double y = p2c_obj_as_float(b);
        if (p2c_float_eq(y, 0.0)) {
            p2c_raise(p2c_make_exception("ZeroDivisionError", "division by zero"));
            return &P2C_None;
        }
        double q = floor(x / y);
        double r = x - q * y;
        if (p2c_float_ne(r, 0.0) && ((y < 0.0) != (r < 0.0))) {
            r += y;
            q -= 1.0;
        }
        quotient = p2c_obj_from_float(q);
        remainder = p2c_obj_from_float(r);
    }
    P2C_Object *out = p2c_tuple_new(2);
    if (!out) return &P2C_None;
    p2c_tuple_set(out, 0, quotient ? quotient : &P2C_None);
    p2c_tuple_set(out, 1, remainder ? remainder : &P2C_None);
    return out;
}

/* a*b (mod m) を64ビットの範囲で正確に計算する（シフト加算、O(64)）。
 * 符号は非負へ正規化したmを前提とし、結果は [0, m) に収まる。 */
static uint64_t p2c_mulmod_u64(uint64_t a, uint64_t b, uint64_t m) {
    uint64_t result = 0;
    a %= m;
    while (b) {
        if (b & 1u) {
            result += a;
            if (result >= m) result -= m;
        }
        a += a;
        if (a >= m) a -= m;
        b >>= 1;
    }
    return result;
}

/* 拡張ユークリッド互除法による法mでの逆元。gcd(a, m) != 1 なら false。 */
static bool p2c_modinv_u64(uint64_t a, uint64_t m, uint64_t *out) {
    int64_t old_r = (int64_t)a, r = (int64_t)m;
    int64_t old_s = 1, s = 0;
    while (r != 0) {
        int64_t q = old_r / r;
        int64_t tmp = old_r - q * r; old_r = r; r = tmp;
        tmp = old_s - q * s; old_s = s; s = tmp;
    }
    if (old_r != 1) return false;
    int64_t inv = old_s % (int64_t)m;
    if (inv < 0) inv += (int64_t)m;
    *out = (uint64_t)inv;
    return true;
}

/* pow(base, exp, mod) — モジュラべき乗。CPythonと同じく、
 *   - 係数0は ValueError
 *   - 負の指数は法における逆元を要求し、存在しなければ ValueError
 *   - 結果の符号は法の符号に従う
 * を満たす。整数以外の引数は TypeError。 */
P2C_Object* p2c_builtin_pow_mod(P2C_Object *base, P2C_Object *exp, P2C_Object *mod) {
    if (!p2c_obj_is_int(base) || !p2c_obj_is_int(exp) || !p2c_obj_is_int(mod)) {
        p2c_raise(p2c_make_exception("TypeError", "pow() 3rd argument not allowed unless all arguments are integers"));
        return &P2C_None;
    }
    int64_t e = exp->u.v_int;
    int64_t m_signed = mod->u.v_int;
    if (m_signed == 0) {
        p2c_raise(p2c_make_exception("ValueError", "pow() 3rd argument cannot be 0"));
        return &P2C_None;
    }
    bool negative_modulus = m_signed < 0;
    uint64_t m = negative_modulus ? (uint64_t)(-(m_signed + 1)) + 1u : (uint64_t)m_signed;
    if (m > (uint64_t)INT64_MAX) {
        /* 64ビット固定幅ランタイムでは 2^63 を法とする結果を表現できない。 */
        p2c_raise(p2c_make_exception("OverflowError", "pow() modulus exceeds the 64-bit runtime range"));
        return &P2C_None;
    }
    int64_t m_i = (int64_t)m;
    /* 底は法における正準な非負剰余へ直す。CPythonは (-2)**3 % 5 を 2 と定義する
     * ため、絶対値で計算すると奇数指数で符号がずれる。 */
    int64_t b_mod = base->u.v_int % m_i;
    if (b_mod < 0) b_mod += m_i;
    uint64_t b = (uint64_t)b_mod;
    uint64_t result;
    if (e < 0) {
        uint64_t inv = 0;
        if (b == 0 || !p2c_modinv_u64(b, m, &inv)) {
            p2c_raise(p2c_make_exception("ValueError", "base is not invertible for the given modulus"));
            return &P2C_None;
        }
        result = 1u;
        uint64_t exponent = (uint64_t)(-(e + 1)) + 1u; /* |e| を安全に求める */
        while (exponent) {
            if (exponent & 1u) result = p2c_mulmod_u64(result, inv, m);
            inv = p2c_mulmod_u64(inv, inv, m);
            exponent >>= 1;
        }
    } else {
        result = 1u % m;
        uint64_t exponent = (uint64_t)e;
        while (exponent) {
            if (exponent & 1u) result = p2c_mulmod_u64(result, b, m);
            b = p2c_mulmod_u64(b, b, m);
            exponent >>= 1;
        }
    }
    int64_t signed_result = (int64_t)result;
    if (negative_modulus && signed_result > 0) signed_result -= m_i;
    return p2c_obj_from_int(signed_result);
}

/* format(value, spec) — f-string/str.formatと同じ書式指定エンジンを使う。 */
P2C_Object* p2c_builtin_format(P2C_Object *value, P2C_Object *spec) {
    return p2c_fstr_fmt(value, spec);
}

/* callable(obj) — 呼び出せるかの近似判定。
 * 関数/closure、クラス、ビルトインcallable、__call__を持つインスタンスを真とする。 */
P2C_Object* p2c_builtin_callable(P2C_Object *obj) {
    if (!obj) return &P2C_False;
    switch (obj->cls ? obj->cls->type_tag : OBJ_NONE) {
        case OBJ_FUNCTION:
        case OBJ_CLASS:
            return &P2C_True;
        case OBJ_INSTANCE:
            return p2c_has_method(obj, "__call__") ? &P2C_True : &P2C_False;
        default:
            return &P2C_False;
    }
}

P2C_Object* p2c_builtin_any(P2C_Object *iterable) {
    size_t n = 0; bool owned = false;
    P2C_Object **items = p2c_iter_items(iterable, &n, &owned);
    for (size_t i = 0; i < n; i++) {
        if (p2c_obj_is_truthy(items[i])) { if (owned) p2c_heap_free(items); return &P2C_True; }
    }
    if (owned) p2c_heap_free(items);
    return &P2C_False;
}

P2C_Object* p2c_builtin_all(P2C_Object *iterable) {
    size_t n = 0; bool owned = false;
    P2C_Object **items = p2c_iter_items(iterable, &n, &owned);
    for (size_t i = 0; i < n; i++) {
        if (!p2c_obj_is_truthy(items[i])) { if (owned) p2c_heap_free(items); return &P2C_False; }
    }
    if (owned) p2c_heap_free(items);
    return &P2C_True;
}

/* map(fn, iterable) -> list */
P2C_Object* p2c_builtin_map(P2C_Object *fn, P2C_Object *iterable) {
    size_t n = 0; bool owned = false;
    P2C_Object **items = p2c_iter_items(iterable, &n, &owned);
    P2C_Object *out = p2c_list_new();
    /* 呼び出しは必ず p2c_call 経由にする（関数オブジェクト・クロージャ・
     * 束縛メソッド・クラスを同じ規則で扱う）。以前は v_function.func を直接
     * 呼んでいたため、クロージャや束縛メソッド（func==NULL）を渡すと
     * 呼び出しをスキップして空リストを返す、という静かな誤りになっていた。 */
    for (size_t i = 0; i < n; i++) {
        P2C_Object *args[1] = { items[i] };
        P2C_Object *result = p2c_call(fn, args, 1);
        p2c_list_append(out, result ? result : &P2C_None);
    }
    if (owned) p2c_heap_free(items);
    return out;
}

/* filter(fn, iterable) -> list (fn==None はtruthyフィルタ) */
P2C_Object* p2c_builtin_filter(P2C_Object *fn, P2C_Object *iterable) {
    size_t n = 0; bool owned = false;
    P2C_Object **items = p2c_iter_items(iterable, &n, &owned);
    P2C_Object *out = p2c_list_new();
    for (size_t i = 0; i < n; i++) {
        bool keep;
        if (!fn || fn == &P2C_None) {
            keep = p2c_obj_is_truthy(items[i]);
        } else {
            /* クロージャ・束縛メソッドも呼べるよう p2c_call 経由にする
             * （以前は v_function.func が無い述語をNone扱いして、
             * 黙ってtruthyフィルタとして動いていた）。 */
            P2C_Object *args[1] = { items[i] };
            keep = p2c_obj_is_truthy(p2c_call(fn, args, 1));
        }
        if (keep) p2c_list_append(out, items[i]);
    }
    if (owned) p2c_heap_free(items);
    return out;
}

/* list(iterable) -> list */
P2C_Object* p2c_builtin_list(P2C_Object *iterable) {
    size_t n = 0; bool owned = false;
    P2C_Object **items = p2c_iter_items(iterable, &n, &owned);
    P2C_Object *result = p2c_list_from_array(items, n);
    if (owned) p2c_heap_free(items);
    return result;
}

P2C_Object* p2c_builtin_dict(P2C_Object *iterable) {
    P2C_Object *result = p2c_dict_new();
    if (!result) return NULL;
    if (p2c_obj_is_dict(iterable)) {
        p2c_dict_update(result, iterable);
        return result;
    }
    size_t n = 0;
    bool owned = false;
    P2C_Object **items = p2c_iter_items(iterable, &n, &owned);
    for (size_t i = 0; i < n; i++) {
        P2C_Object *pair = items[i];
        if (p2c_len(pair) != 2) {
            if (owned) p2c_heap_free(items);
            p2c_raise(p2c_make_exception("ValueError", "dictionary update sequence element has length other than 2"));
            return &P2C_None;
        }
        p2c_dict_set(result, p2c_iter_at(pair, 0), p2c_iter_at(pair, 1));
    }
    if (owned) p2c_heap_free(items);
    return result;
}

P2C_Object* p2c_builtin_set(P2C_Object *iterable) {
    size_t n = 0; bool owned = false;
    P2C_Object **items = p2c_iter_items(iterable, &n, &owned);
    P2C_Object *result = p2c_set_from_array(items, n);
    if (owned) p2c_heap_free(items);
    return result;
}

/* tuple(iterable) -> tuple */
P2C_Object* p2c_builtin_tuple(P2C_Object *iterable) {
    if (iterable && iterable->cls == &P2C_Class_Tuple) return iterable;
    size_t n = 0; bool owned = false;
    P2C_Object **items = p2c_iter_items(iterable, &n, &owned);
    P2C_Object *result = p2c_tuple_from_array(items, n);
    if (owned) p2c_heap_free(items);
    return result;
}
void p2c_print_str(const char *s) { p2c_platform_write(s ? s : ""); }
P2C_Object* p2c_input(void) {
    char buf[1024];
    /* カーネル側の read_line 実装がバッファへ書き込まない場合でも未初期化値を
     * 読まないよう、先頭を空文字列にしておく（-Wmaybe-uninitialized 対策と、
     * 壊れた/未実装アダプタに対する防御の両方を兼ねる）。 */
    buf[0] = '\0';
    size_t n = p2c_platform_read_line(buf, sizeof(buf));
    if (n >= sizeof(buf)) n = sizeof(buf) - 1u; /* 実装が過大な長さを返した場合の防御 */
    return p2c_obj_from_str_n(buf, n);
}
int64_t p2c_len(P2C_Object *obj) {
    if (!obj) return 0;
    switch (obj->cls ? obj->cls->type_tag : OBJ_NONE) {
        case OBJ_STR: return (int64_t)obj->u.v_str.len;
        case OBJ_LIST: return (int64_t)obj->u.v_list.len;
        case OBJ_DICT: return (int64_t)obj->u.v_dict.len;
        case OBJ_SET: return (int64_t)obj->u.v_dict.len;
        case OBJ_TUPLE: return (int64_t)obj->u.v_tuple.len;
        case OBJ_INSTANCE:
            /* __len__ を実装しているインスタンスに対応する。
             * 以前はここが常に0を返しており、len(custom_obj) が
             * __len__ の中身を無視して静かに間違った値(0)を返していた。 */
            if (p2c_has_method(obj, "__len__")) {
                P2C_Object *r = p2c_call_attr(obj, "__len__", NULL, 0);
                return p2c_obj_as_int(r);
            }
            p2c_raise(p2c_make_exception("TypeError", "object of type has no len()"));
            return 0;
        default: return 0;
    }
}
P2C_Object* p2c_range(P2C_Object *start, P2C_Object *stop, P2C_Object *step) {
    int64_t s = p2c_obj_as_int(start), e = p2c_obj_as_int(stop), inc = p2c_obj_as_int(step);
    if (inc == 0) inc = 1;
    P2C_Object *list = p2c_list_new(); if (!list) return NULL;
    if (inc > 0) for (int64_t i = s; i < e; i += inc) p2c_list_append(list, p2c_obj_from_int(i));
    else for (int64_t i = s; i > e; i += inc) p2c_list_append(list, p2c_obj_from_int(i));
    return list;
}

/* 属性の有無を「値がNoneかどうか」ではなく実際の存在有無で判定する
 * 内部ヘルパー。見つからなければNULLを返す（例外は投げない）。
 * hasattr()・3引数getattr()のデフォルト値処理・obj.attr の存在チェックの
 * すべてがこれを土台にする。
 * （以前のp2c_hasattrは p2c_getattr(...) != &P2C_None で判定しており、
 * self.x = None のように属性の値がたまたまNoneのときに、属性自体は
 * 存在するのにhasattrがfalseを返す、という誤った挙動になっていた。） */
static P2C_Object* getattr_raw(P2C_Object *obj, const char *name) {
    if (!obj || !name || !obj->cls) return NULL;
    switch (obj->cls->type_tag) {
        case OBJ_INSTANCE: {
            P2C_Object *val = attr_map_get(obj->u.v_instance.attrs, name);
            if (val) return val;
            /* クラス属性は基底クラスも含めてMRO順に探す（Python同様、サブクラスの
             * インスタンスから基底クラスのクラス属性が見える）。以前は自分の
             * クラスのattrsしか見ておらず、class B(A) のインスタンスでA側の
             * クラス属性を読むとAttributeErrorになっていた。 */
            val = p2c_class_attr_along_mro(obj->u.v_instance.klass, name);
            if (val) return val;
            /* メソッドを値として取り出す場合（m = obj.method、sorted(key=obj.key)
             * など）は束縛メソッドを返す。以前はここでNULLを返しており、
             * obj.method() の呼び出し形以外はAttributeErrorになっていた。 */
            if (p2c_find_method_in_chain(obj->u.v_instance.klass, name)) {
                return p2c_bound_method_new(obj, name);
            }
            return NULL;
        }
        case OBJ_CLASS:
            /* クラスオブジェクトの属性も基底クラスを辿る（class D(B) で D.x が
             * Bのクラス属性を見つける）。 */
            return p2c_class_attr_along_mro(obj, name);
        case OBJ_MODULE:
            return attr_map_get(obj->u.v_module.attrs, name);
        case OBJ_EXCEPTION:
            if (strcmp(name, "__cause__") == 0) return obj->u.v_exception.cause ? obj->u.v_exception.cause : &P2C_None;
            if (strcmp(name, "__context__") == 0) return &P2C_None;
            return NULL;
        /* OBJ_DICTはここに含めない: 実際のPythonではdictはキーを属性として
         * 公開しない（getattr({'a':1}, 'a')はAttributeErrorになる）。
         * 以前はここでdictの添字アクセスにフォールバックしており、
         * Python本来の挙動と異なっていた。 */
        default:
            return NULL;
    }
}
bool p2c_hasattr(P2C_Object *obj, const char *name) { return getattr_raw(obj, name) != NULL; }
/* obj.attr （Pythonのドット記法）。属性が存在しなければAttributeErrorを
 * 送出する。以前はここが常に&P2C_Noneを返しており、存在しない属性への
 * アクセスが静かにNoneになってしまっていた（実際のPythonではAttributeError）。 */
P2C_Object* p2c_getattr(P2C_Object *obj, const char *name) {
    P2C_Object *val = getattr_raw(obj, name);
    if (val) return val;
    p2c_raise(p2c_make_exception("AttributeError", name));
    return &P2C_None;
}
/* getattr(obj, name) / getattr(obj, name, default) の2引数・3引数形式用。
 * default_val は2引数形式ではNULLを渡す（その場合は属性が無ければ
 * AttributeErrorを送出し、p2c_getattrと同じ挙動になる）。 */
P2C_Object* p2c_getattr_default(P2C_Object *obj, const char *name, P2C_Object *default_val) {
    P2C_Object *val = getattr_raw(obj, name);
    if (val) return val;
    if (default_val) return default_val;
    p2c_raise(p2c_make_exception("AttributeError", name));
    return &P2C_None;
}
void p2c_setattr(P2C_Object *obj, const char *name, P2C_Object *val) {
    if (!obj || !name || !obj->cls) return;
    switch (obj->cls->type_tag) {
        case OBJ_INSTANCE: attr_map_set(obj->u.v_instance.attrs, name, val); break;
        case OBJ_CLASS: attr_map_set(obj->u.v_class.attrs, name, val); break;
        case OBJ_MODULE: attr_map_set(obj->u.v_module.attrs, name, val); break;
        default: p2c_raise(p2c_make_exception("AttributeError", name)); break;
    }
}
/* for文の反復専用: 「位置position番目の要素」を返す。d[k]（実際のキー参照、
 * p2c_subscript_get）とは意味が異なる点に注意。
 * list/tuple/strでは位置とキーが一致するためp2c_subscript_getと同じ結果に
 * なるが、dictでは全く別物: d[5]は「キー5の値」を意味するのに対し、
 * for文でdictを反復するときの「position番目」は「(挿入・ハッシュ順で)
 * position番目のキー」を意味する。
 * 以前はfor文がdict含め全ての型でp2c_subscript_getを使っていたため、
 * for k in some_dict: のようなdict直接反復が、整数キーの通し番号による
 * ハッシュ検索として扱われてしまい、整数キー以外のdict（実際にはこれが
 * 大多数）では毎回KeyErrorで異常終了するという重大なバグがあった。 */
P2C_Object* p2c_iter_at(P2C_Object *obj, int64_t position) {
    if (!obj || !obj->cls) return &P2C_None;
    if (obj->cls->type_tag == OBJ_DICT || obj->cls->type_tag == OBJ_SET) {
        int64_t count = 0;
        for (P2C_DictEntry *e = obj->u.v_dict.order_head; e; e = e->order_next) {
            if (count == position) return e->key;
            count++;
        }
        p2c_raise(p2c_make_exception("RuntimeError", "container changed size during iteration"));
        return &P2C_None;
    }
    return p2c_subscript_get(obj, p2c_obj_from_int(position));
}
P2C_Object* p2c_subscript_get(P2C_Object *obj, P2C_Object *key) {
    if (!obj || !obj->cls) return &P2C_None;
    switch (obj->cls->type_tag) {
        case OBJ_LIST: {
            int64_t idx = 0;
            if (!p2c_obj_index_value(key, &idx)) return &P2C_None;
            size_t len = obj->u.v_list.len;
            if (idx < 0) idx += (int64_t)len;
            if (idx < 0 || (size_t)idx >= len) { p2c_raise(p2c_make_exception("IndexError", "list index out of range")); return &P2C_None; }
            return p2c_list_get(obj, (size_t)idx);
        }
        case OBJ_TUPLE: {
            int64_t idx = 0;
            if (!p2c_obj_index_value(key, &idx)) return &P2C_None;
            size_t len = obj->u.v_tuple.len;
            if (idx < 0) idx += (int64_t)len;
            if (idx < 0 || (size_t)idx >= len) { p2c_raise(p2c_make_exception("IndexError", "tuple index out of range")); return &P2C_None; }
            return p2c_tuple_get(obj, (size_t)idx);
        }
        case OBJ_STR: {
            int64_t idx = 0;
            if (!p2c_obj_index_value(key, &idx)) return &P2C_None;
            size_t len = obj->u.v_str.len;
            if (idx < 0) idx += (int64_t)len;
            if (idx < 0 || (size_t)idx >= len) { p2c_raise(p2c_make_exception("IndexError", "string index out of range")); return &P2C_None; }
            char buf[2] = { obj->u.v_str.data[idx], '\0' };
            return p2c_obj_from_str(buf);
        }
        case OBJ_DICT: return p2c_dict_get(obj, key);
        case OBJ_INSTANCE:
            /* __getitem__ を実装しているインスタンスに対応する
             * （これによりfor文の既存の実装(p2c_len+p2c_subscript_get)経由で
             * __len__+__getitem__ を実装したカスタムクラスもそのままfor文で
             * 反復できるようになる）。 */
            if (p2c_has_method(obj, "__getitem__")) {
                P2C_Object *args[1] = { key };
                return p2c_call_attr(obj, "__getitem__", args, 1);
            }
            p2c_raise(p2c_make_exception("TypeError", "object is not subscriptable"));
            return &P2C_None;
        default: p2c_raise(p2c_make_exception("TypeError", "object is not subscriptable")); return &P2C_None;
    }
}
bool p2c_match_sequence(P2C_Object *obj, size_t min_len, bool allow_rest) {
    size_t len;
    if (!obj || !(p2c_obj_is_list(obj) || p2c_obj_is_tuple(obj))) return false;
    len = p2c_obj_is_list(obj) ? obj->u.v_list.len : obj->u.v_tuple.len;
    return allow_rest ? len >= min_len : len == min_len;
}

P2C_Object* p2c_match_sequence_item(P2C_Object *obj, size_t index) {
    if (!obj) return &P2C_None;
    if (p2c_obj_is_list(obj)) return index < obj->u.v_list.len ? p2c_list_get(obj, index) : &P2C_None;
    if (p2c_obj_is_tuple(obj)) return index < obj->u.v_tuple.len ? p2c_tuple_get(obj, index) : &P2C_None;
    return &P2C_None;
}

P2C_Object* p2c_match_sequence_rest(P2C_Object *obj, size_t start, size_t end_from_tail) {
    size_t len;
    P2C_Object *out;
    if (!p2c_match_sequence(obj, start + end_from_tail, true)) return &P2C_None;
    len = p2c_obj_is_list(obj) ? obj->u.v_list.len : obj->u.v_tuple.len;
    out = p2c_list_new();
    if (!out) return &P2C_None;
    for (size_t i = start; i < len - end_from_tail; i++) p2c_list_append(out, p2c_match_sequence_item(obj, i));
    return out;
}

bool p2c_match_mapping_has(P2C_Object *obj, P2C_Object *key) {
    size_t hash;
    P2C_DictEntry *entry;
    if (!obj || !p2c_obj_is_dict(obj) || !key || !p2c_require_hashable(key)) return false;
    if (!obj->u.v_dict.buckets || obj->u.v_dict.bucket_count == 0) return false;
    hash = p2c_obj_hash(key) % obj->u.v_dict.bucket_count;
    entry = obj->u.v_dict.buckets[hash];
    while (entry) {
        if (p2c_obj_equal_raw(entry->key, key)) return true;
        entry = entry->next;
    }
    return false;
}

P2C_Object* p2c_match_mapping_get(P2C_Object *obj, P2C_Object *key) {
    return p2c_match_mapping_has(obj, key) ? p2c_dict_get(obj, key) : &P2C_None;
}

P2C_Object* p2c_match_mapping_rest(P2C_Object *obj, P2C_Object *const *keys, size_t key_count) {
    P2C_Object *out = p2c_dict_new();
    if (!out || !p2c_obj_is_dict(obj)) return out;
    for (P2C_DictEntry *entry = obj->u.v_dict.order_head; entry; entry = entry->order_next) {
        bool excluded = false;
        for (size_t i = 0; i < key_count; i++) {
            if (keys[i] && p2c_obj_equal_raw(entry->key, keys[i])) {
                excluded = true;
                break;
            }
        }
        if (!excluded) p2c_dict_set(out, entry->key, entry->val);
    }
    return out;
}

bool p2c_match_class_positional(P2C_Object *obj, const char *class_name, size_t index, P2C_Object **out_value) {
    P2C_Object *match_args;
    P2C_Object *attr_name;
    if (!out_value || !obj || !obj->cls || !class_name) return false;
    *out_value = &P2C_None;
    if (strcmp(class_name, "bool") == 0 || strcmp(class_name, "int") == 0 ||
        strcmp(class_name, "float") == 0 || strcmp(class_name, "str") == 0 ||
        strcmp(class_name, "list") == 0 || strcmp(class_name, "tuple") == 0 ||
        strcmp(class_name, "dict") == 0 || strcmp(class_name, "set") == 0) {
        if (index != 0) return false;
        *out_value = obj;
        return true;
    }
    if (obj->cls->type_tag != OBJ_INSTANCE || !obj->u.v_instance.klass ||
        !p2c_hasattr(obj->u.v_instance.klass, "__match_args__")) return false;
    match_args = p2c_getattr(obj->u.v_instance.klass, "__match_args__");
    if (!p2c_obj_is_tuple(match_args) || index >= match_args->u.v_tuple.len) return false;
    attr_name = match_args->u.v_tuple.items[index];
    if (!p2c_obj_is_str(attr_name) || !p2c_hasattr(obj, p2c_obj_as_str(attr_name))) return false;
    *out_value = p2c_getattr(obj, p2c_obj_as_str(attr_name));
    return true;
}

void p2c_subscript_set(P2C_Object *obj, P2C_Object *key, P2C_Object *val) {
    if (!obj || !obj->cls) return;
    switch (obj->cls->type_tag) {
        case OBJ_LIST: {
            int64_t idx = 0;
            if (!p2c_obj_index_value(key, &idx)) return;
            if (idx < 0) idx += (int64_t)obj->u.v_list.len;
            if (idx < 0 || (size_t)idx >= obj->u.v_list.len) {
                p2c_raise(p2c_make_exception("IndexError", "list assignment index out of range"));
                return;
            }
            p2c_list_set(obj, (size_t)idx, val);
            break;
        }
        case OBJ_DICT: p2c_dict_set(obj, key, val); break;
        case OBJ_INSTANCE:
            if (p2c_has_method(obj, "__setitem__")) {
                P2C_Object *args[2] = { key, val };
                p2c_call_attr(obj, "__setitem__", args, 2);
            } else {
                p2c_raise(p2c_make_exception("TypeError", "object does not support item assignment"));
            }
            break;
        default: p2c_raise(p2c_make_exception("TypeError", "object does not support item assignment")); break;
    }
}
/* del obj.attr */
void p2c_delattr(P2C_Object *obj, const char *name) {
    if (!obj || !name || !obj->cls) return;
    bool ok = false;
    switch (obj->cls->type_tag) {
        case OBJ_INSTANCE: ok = attr_map_remove(obj->u.v_instance.attrs, name); break;
        case OBJ_CLASS:    ok = attr_map_remove(obj->u.v_class.attrs, name); break;
        case OBJ_MODULE:   ok = attr_map_remove(obj->u.v_module.attrs, name); break;
        default: p2c_raise(p2c_make_exception("AttributeError", name)); return;
    }
    if (!ok) p2c_raise(p2c_make_exception("AttributeError", name));
}
/* del obj[key] */
void p2c_subscript_delete(P2C_Object *obj, P2C_Object *key) {
    if (!obj || !obj->cls) return;
    switch (obj->cls->type_tag) {
        case OBJ_LIST: {
            int64_t idx = 0;
            if (!p2c_obj_index_value(key, &idx)) return;
            p2c_list_delete_at(obj, idx);
            break;
        }
        case OBJ_DICT: p2c_dict_remove(obj, key); break;
        default: p2c_raise(p2c_make_exception("TypeError", "object doesn't support item deletion")); break;
    }
}
