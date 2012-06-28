/*
  Upcalls

  These are runtime functions that the compiler knows about and generates
  calls to. They are called on the Rust stack and, in most cases, immediately
  switch to the C stack.
 */

#include "rust_globals.h"
#include "rust_task.h"
#include "rust_cc.h"
#include "rust_sched_loop.h"
#include "rust_unwind.h"
#include "rust_upcall.h"
#include "rust_util.h"

#ifdef __GNUC__
#define LOG_UPCALL_ENTRY(task)                            \
    LOG(task, upcall,                                     \
        "> UPCALL %s - task: %s 0x%" PRIxPTR              \
        " retpc: x%" PRIxPTR,                             \
        __FUNCTION__,                                     \
        (task)->name, (task),                             \
        __builtin_return_address(0));
#else
#define LOG_UPCALL_ENTRY(task)                            \
    LOG(task, upcall, "> UPCALL task: %s @x%" PRIxPTR,    \
        (task)->name, (task));
#endif

#define UPCALL_SWITCH_STACK(T, A, F) \
    call_upcall_on_c_stack(T, (void*)A, (void*)F)

inline void
call_upcall_on_c_stack(rust_task *task, void *args, void *fn_ptr) {
    task->call_on_c_stack(args, fn_ptr);
}

/**********************************************************************
 * Switches to the C-stack and invokes |fn_ptr|, passing |args| as argument.
 * This is used by the C compiler to call foreign functions and by other
 * upcalls to switch to the C stack.  The return value is passed through a
 * field in the args parameter. This upcall is specifically for switching
 * to the shim functions generated by rustc.
 */
extern "C" CDECL void
upcall_call_shim_on_c_stack(void *args, void *fn_ptr) {
    rust_task *task = rust_get_current_task();

    try {
        task->call_on_c_stack(args, fn_ptr);
    } catch (...) {
        // Logging here is not reliable
        assert(false && "Foreign code threw an exception");
    }
}

/*
 * The opposite of above. Starts on a C stack and switches to the Rust
 * stack. This is the only upcall that runs from the C stack.
 */
extern "C" CDECL void
upcall_call_shim_on_rust_stack(void *args, void *fn_ptr) {
    rust_task *task = rust_get_current_task();

    try {
        task->call_on_rust_stack(args, fn_ptr);
    } catch (...) {
        // We can't count on being able to unwind through arbitrary
        // code. Our best option is to just fail hard.
        // Logging here is not reliable
        assert(false && "Rust task failed after reentering the Rust stack");
    }
}

/**********************************************************************/

struct s_fail_args {
    rust_task *task;
    char const *expr;
    char const *file;
    size_t line;
};

extern "C" CDECL void
upcall_s_fail(s_fail_args *args) {
    rust_task *task = args->task;
    LOG_UPCALL_ENTRY(task);
    task->fail(args->expr, args->file, args->line);
}

extern "C" CDECL void
upcall_fail(char const *expr,
            char const *file,
            size_t line) {
    rust_task *task = rust_get_current_task();
    s_fail_args args = {task,expr,file,line};
    UPCALL_SWITCH_STACK(task, &args, upcall_s_fail);
}

struct s_trace_args {
    rust_task *task;
    char const *msg;
    char const *file;
    size_t line;
};

extern "C" CDECL void
upcall_s_trace(s_trace_args *args) {
    rust_task *task = args->task;
    LOG_UPCALL_ENTRY(task);
    LOG(task, trace, "Trace %s:%d: %s",
        args->file, args->line, args->msg);
}

extern "C" CDECL void
upcall_trace(char const *msg,
             char const *file,
             size_t line) {
    rust_task *task = rust_get_current_task();
    s_trace_args args = {task,msg,file,line};
    UPCALL_SWITCH_STACK(task, &args, upcall_s_trace);
}

/**********************************************************************
 * Allocate an object in the exchange heap
 */

struct s_exchange_malloc_args {
    rust_task *task;
    uintptr_t retval;
    type_desc *td;
    uintptr_t size;
};

extern "C" CDECL void
upcall_s_exchange_malloc(s_exchange_malloc_args *args) {
    rust_task *task = args->task;
    LOG_UPCALL_ENTRY(task);
    LOG(task, mem, "upcall exchange malloc(0x%" PRIxPTR ")", args->td);

    size_t total_size = get_box_size(args->size, args->td->align);
    // FIXME--does this have to be calloc? (Issue #2682)
    void *p = task->kernel->calloc(total_size, "exchange malloc");

    rust_opaque_box *header = static_cast<rust_opaque_box*>(p);
    header->ref_count = -1; // This is not ref counted
    header->td = args->td;
    header->prev = 0;
    header->next = 0;

    args->retval = (uintptr_t)header;
}

extern "C" CDECL uintptr_t
upcall_exchange_malloc(type_desc *td, uintptr_t size) {
    rust_task *task = rust_get_current_task();
    s_exchange_malloc_args args = {task, 0, td, size};
    UPCALL_SWITCH_STACK(task, &args, upcall_s_exchange_malloc);
    return args.retval;
}

struct s_exchange_free_args {
    rust_task *task;
    void *ptr;
};

extern "C" CDECL void
upcall_s_exchange_free(s_exchange_free_args *args) {
    rust_task *task = args->task;
    LOG_UPCALL_ENTRY(task);
    task->kernel->free(args->ptr);
}

extern "C" CDECL void
upcall_exchange_free(void *ptr) {
    rust_task *task = rust_get_current_task();
    s_exchange_free_args args = {task,ptr};
    UPCALL_SWITCH_STACK(task, &args, upcall_s_exchange_free);
}

/**********************************************************************
 * Allocate an object in the task-local heap.
 */

struct s_malloc_args {
    rust_task *task;
    uintptr_t retval;
    type_desc *td;
    uintptr_t size;
};

extern "C" CDECL void
upcall_s_malloc(s_malloc_args *args) {
    rust_task *task = args->task;
    LOG_UPCALL_ENTRY(task);
    LOG(task, mem, "upcall malloc(0x%" PRIxPTR ")", args->td);

    cc::maybe_cc(task);

    // FIXME--does this have to be calloc? (Issue #2682)
    rust_opaque_box *box = task->boxed.calloc(args->td, args->size);
    void *body = box_body(box);

    debug::maybe_track_origin(task, box);

    LOG(task, mem,
        "upcall malloc(0x%" PRIxPTR ") = box 0x%" PRIxPTR
        " with body 0x%" PRIxPTR,
        args->td, (uintptr_t)box, (uintptr_t)body);

    args->retval = (uintptr_t)box;
}

extern "C" CDECL uintptr_t
upcall_malloc(type_desc *td, uintptr_t size) {
    rust_task *task = rust_get_current_task();
    s_malloc_args args = {task, 0, td, size};
    UPCALL_SWITCH_STACK(task, &args, upcall_s_malloc);
    return args.retval;
}

/**********************************************************************
 * Called whenever an object in the task-local heap is freed.
 */

struct s_free_args {
    rust_task *task;
    void *ptr;
};

extern "C" CDECL void
upcall_s_free(s_free_args *args) {
    rust_task *task = args->task;
    LOG_UPCALL_ENTRY(task);

    rust_sched_loop *sched_loop = task->sched_loop;
    DLOG(sched_loop, mem,
             "upcall free(0x%" PRIxPTR ", is_gc=%" PRIdPTR ")",
             (uintptr_t)args->ptr);

    debug::maybe_untrack_origin(task, args->ptr);

    rust_opaque_box *box = (rust_opaque_box*) args->ptr;
    task->boxed.free(box);
}

extern "C" CDECL void
upcall_free(void* ptr) {
    rust_task *task = rust_get_current_task();
    s_free_args args = {task,ptr};
    UPCALL_SWITCH_STACK(task, &args, upcall_s_free);
}

/**********************************************************************
 * Sanity checks on boxes, insert when debugging possible
 * use-after-free bugs.  See maybe_validate_box() in trans.rs.
 */

extern "C" CDECL void
upcall_validate_box(rust_opaque_box* ptr) {
    if (ptr) {
        assert(ptr->ref_count > 0);
        assert(ptr->td != NULL);
        assert(ptr->td->align <= 8);
        assert(ptr->td->size <= 4096); // might not really be true...
    }
}

/**********************************************************************/

struct s_str_new_uniq_args {
    rust_task *task;
    const char *cstr;
    size_t len;
    rust_str *retval;
};

extern "C" CDECL void
upcall_s_str_new_uniq(s_str_new_uniq_args *args) {
    rust_task *task = args->task;
    LOG_UPCALL_ENTRY(task);
    args->retval = make_str(task->kernel, args->cstr, args->len,
                            "str_new_uniq");
}

extern "C" CDECL rust_str*
upcall_str_new_uniq(const char *cstr, size_t len) {
    rust_task *task = rust_get_current_task();
    s_str_new_uniq_args args = { task, cstr, len, 0 };
    UPCALL_SWITCH_STACK(task, &args, upcall_s_str_new_uniq);
    return args.retval;
}

extern "C" CDECL rust_str*
upcall_str_new(const char *cstr, size_t len) {
    rust_task *task = rust_get_current_task();
    s_str_new_uniq_args args = { task, cstr, len, 0 };
    UPCALL_SWITCH_STACK(task, &args, upcall_s_str_new_uniq);
    return args.retval;
}



struct s_str_new_shared_args {
    rust_task *task;
    const char *cstr;
    size_t len;
    rust_opaque_box *retval;
};

extern "C" CDECL void
upcall_s_str_new_shared(s_str_new_shared_args *args) {
    rust_task *task = args->task;
    LOG_UPCALL_ENTRY(task);

    size_t str_fill = args->len + 1;
    size_t str_alloc = str_fill;
    args->retval = (rust_opaque_box *)
        task->boxed.malloc(&str_body_tydesc,
                           str_fill + sizeof(rust_vec));
    rust_str *str = (rust_str *)args->retval;
    str->body.fill = str_fill;
    str->body.alloc = str_alloc;
    memcpy(&str->body.data, args->cstr, args->len);
    str->body.data[args->len] = '\0';
}

extern "C" CDECL rust_opaque_box*
upcall_str_new_shared(const char *cstr, size_t len) {
    rust_task *task = rust_get_current_task();
    s_str_new_shared_args args = { task, cstr, len, 0 };
    UPCALL_SWITCH_STACK(task, &args, upcall_s_str_new_shared);
    return args.retval;
}


struct s_vec_grow_args {
    rust_task *task;
    rust_vec_box** vp;
    size_t new_sz;
};

extern "C" CDECL void
upcall_s_vec_grow(s_vec_grow_args *args) {
    rust_task *task = args->task;
    LOG_UPCALL_ENTRY(task);
    reserve_vec(task, args->vp, args->new_sz);
    (*args->vp)->body.fill = args->new_sz;
}

extern "C" CDECL void
upcall_vec_grow(rust_vec_box** vp, size_t new_sz) {
    rust_task *task = rust_get_current_task();
    s_vec_grow_args args = {task, vp, new_sz};
    UPCALL_SWITCH_STACK(task, &args, upcall_s_vec_grow);
}

struct s_str_concat_args {
    rust_task *task;
    rust_vec_box* lhs;
    rust_vec_box* rhs;
    rust_vec_box* retval;
};

extern "C" CDECL void
upcall_s_str_concat(s_str_concat_args *args) {
    rust_vec *lhs = &args->lhs->body;
    rust_vec *rhs = &args->rhs->body;
    rust_task *task = args->task;
    size_t fill = lhs->fill + rhs->fill - 1;
    rust_vec_box* v = (rust_vec_box*)
        task->kernel->malloc(fill + sizeof(rust_vec_box),
                             "str_concat");
    v->header.td = args->lhs->header.td;
    v->body.fill = v->body.alloc = fill;
    memmove(&v->body.data[0], &lhs->data[0], lhs->fill - 1);
    memmove(&v->body.data[lhs->fill - 1], &rhs->data[0], rhs->fill);
    args->retval = v;
}

extern "C" CDECL rust_vec_box*
upcall_str_concat(rust_vec_box* lhs, rust_vec_box* rhs) {
    rust_task *task = rust_get_current_task();
    s_str_concat_args args = {task, lhs, rhs, 0};
    UPCALL_SWITCH_STACK(task, &args, upcall_s_str_concat);
    return args.retval;
}


extern "C" _Unwind_Reason_Code
__gxx_personality_v0(int version,
                     _Unwind_Action actions,
                     uint64_t exception_class,
                     _Unwind_Exception *ue_header,
                     _Unwind_Context *context);

struct s_rust_personality_args {
    _Unwind_Reason_Code retval;
    int version;
    _Unwind_Action actions;
    uint64_t exception_class;
    _Unwind_Exception *ue_header;
    _Unwind_Context *context;
};

extern "C" void
upcall_s_rust_personality(s_rust_personality_args *args) {
    args->retval = __gxx_personality_v0(args->version,
                                        args->actions,
                                        args->exception_class,
                                        args->ue_header,
                                        args->context);
}

/**
   The exception handling personality function. It figures
   out what to do with each landing pad. Just a stack-switching
   wrapper around the C++ personality function.
*/
extern "C" _Unwind_Reason_Code
upcall_rust_personality(int version,
                        _Unwind_Action actions,
                        uint64_t exception_class,
                        _Unwind_Exception *ue_header,
                        _Unwind_Context *context) {
    s_rust_personality_args args = {(_Unwind_Reason_Code)0,
                                    version, actions, exception_class,
                                    ue_header, context};
    rust_task *task = rust_get_current_task();

    // The personality function is run on the stack of the
    // last function that threw or landed, which is going
    // to sometimes be the C stack. If we're on the Rust stack
    // then switch to the C stack.

    if (task->on_rust_stack()) {
        UPCALL_SWITCH_STACK(task, &args, upcall_s_rust_personality);
    } else {
        upcall_s_rust_personality(&args);
    }
    return args.retval;
}

extern "C" void
shape_cmp_type(int8_t *result, const type_desc *tydesc,
               const type_desc **subtydescs, uint8_t *data_0,
               uint8_t *data_1, uint8_t cmp_type);

struct s_cmp_type_args {
    int8_t *result;
    const type_desc *tydesc;
    const type_desc **subtydescs;
    uint8_t *data_0;
    uint8_t *data_1;
    uint8_t cmp_type;
};

extern "C" void
upcall_s_cmp_type(s_cmp_type_args *args) {
    shape_cmp_type(args->result, args->tydesc, args->subtydescs,
                   args->data_0, args->data_1, args->cmp_type);
}

extern "C" void
upcall_cmp_type(int8_t *result, const type_desc *tydesc,
                const type_desc **subtydescs, uint8_t *data_0,
                uint8_t *data_1, uint8_t cmp_type) {
    rust_task *task = rust_get_current_task();
    s_cmp_type_args args = {result, tydesc, subtydescs,
                            data_0, data_1, cmp_type};
    UPCALL_SWITCH_STACK(task, &args, upcall_s_cmp_type);
}

extern "C" void
shape_log_type(const type_desc *tydesc, uint8_t *data, uint32_t level);

struct s_log_type_args {
    const type_desc *tydesc;
    uint8_t *data;
    uint32_t level;
};

extern "C" void
upcall_s_log_type(s_log_type_args *args) {
    shape_log_type(args->tydesc, args->data, args->level);
}

extern "C" void
upcall_log_type(const type_desc *tydesc, uint8_t *data, uint32_t level) {
    rust_task *task = rust_get_current_task();
    s_log_type_args args = {tydesc, data, level};
    UPCALL_SWITCH_STACK(task, &args, upcall_s_log_type);
}

// NB: This needs to be blazing fast. Don't switch stacks
extern "C" CDECL void *
upcall_new_stack(size_t stk_sz, void *args_addr, size_t args_sz) {
    rust_task *task = rust_get_current_task();
    return task->next_stack(stk_sz,
                            args_addr,
                            args_sz);
}

// NB: This needs to be blazing fast. Don't switch stacks
extern "C" CDECL void
upcall_del_stack() {
    rust_task *task = rust_get_current_task();
    task->prev_stack();
}

// Landing pads need to call this to insert the
// correct limit into TLS.
// NB: This must run on the Rust stack because it
// needs to acquire the value of the stack pointer
extern "C" CDECL void
upcall_reset_stack_limit() {
    rust_task *task = rust_get_current_task();
    task->reset_stack_limit();
}

//
// Local Variables:
// mode: C++
// fill-column: 78;
// indent-tabs-mode: nil
// c-basic-offset: 4
// buffer-file-coding-system: utf-8-unix
// End:
//
