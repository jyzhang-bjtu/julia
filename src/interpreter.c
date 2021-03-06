// This file is a part of Julia. License is MIT: http://julialang.org/license

#include <stdlib.h>
#include <setjmp.h>
#include <assert.h>
#ifdef _OS_WINDOWS_
#include <malloc.h>
#endif
#include "julia.h"
#include "julia_internal.h"
#include "builtin_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    jl_lambda_info_t *lam;
    jl_value_t **locals;
    jl_svec_t *sparam_vals;
} interpreter_state;

static jl_value_t *eval(jl_value_t *e, interpreter_state *s);
static jl_value_t *eval_body(jl_array_t *stmts, interpreter_state *s, int start, int toplevel);

jl_value_t *jl_eval_module_expr(jl_expr_t *ex);
int jl_is_toplevel_only_expr(jl_value_t *e);

jl_value_t *jl_interpret_toplevel_expr(jl_value_t *e)
{
    return eval(e, NULL);
}

JL_DLLEXPORT jl_value_t *jl_interpret_toplevel_expr_in(jl_module_t *m, jl_value_t *e,
                                                       jl_lambda_info_t *lam)
{
    jl_value_t *v=NULL;
    jl_module_t *last_m = jl_current_module;
    jl_module_t *task_last_m = jl_current_task->current_module;
    interpreter_state s;
    s.lam = lam; s.locals = NULL; s.sparam_vals = NULL;

    JL_TRY {
        jl_current_task->current_module = jl_current_module = m;
        v = eval(e, &s);
    }
    JL_CATCH {
        jl_current_module = last_m;
        jl_current_task->current_module = task_last_m;
        jl_rethrow();
    }
    jl_current_module = last_m;
    jl_current_task->current_module = task_last_m;
    assert(v);
    return v;
}

static jl_value_t *do_call(jl_value_t **args, size_t nargs, interpreter_state *s)
{
    jl_value_t **argv;
    JL_GC_PUSHARGS(argv, nargs);
    size_t i;
    for(i=0; i < nargs; i++)
        argv[i] = eval(args[i], s);
    jl_value_t *result = jl_apply_generic(argv, nargs);
    JL_GC_POP();
    return result;
}

static jl_value_t *do_invoke(jl_value_t **args, size_t nargs, interpreter_state *s)
{
    jl_value_t **argv;
    JL_GC_PUSHARGS(argv, nargs - 1);
    size_t i;
    for (i = 1; i < nargs; i++)
        argv[i - 1] = eval(args[i], s);
    jl_lambda_info_t *meth = (jl_lambda_info_t*)args[0];
    assert(jl_is_lambda_info(meth) && !meth->inInference);
    jl_value_t *result = jl_call_method_internal(meth, argv, nargs - 1);
    JL_GC_POP();
    return result;
}

jl_value_t *jl_eval_global_var(jl_module_t *m, jl_sym_t *e)
{
    jl_value_t *v = jl_get_global(m, e);
    if (v == NULL)
        jl_undefined_var_error(e);
    return v;
}

extern int jl_boot_file_loaded;
extern int inside_typedef;

// this is a heuristic for allowing "redefining" a type to something identical
static int equiv_type(jl_datatype_t *dta, jl_datatype_t *dtb)
{
    return (jl_typeof(dta) == jl_typeof(dtb) &&
            // TODO: can't yet handle parametric types due to how constructors work
            dta->parameters == jl_emptysvec &&
            dta->name->name == dtb->name->name &&
            jl_egal((jl_value_t*)dta->types, (jl_value_t*)dtb->types) &&
            dta->abstract == dtb->abstract &&
            dta->mutabl == dtb->mutabl &&
            dta->size == dtb->size &&
            dta->ninitialized == dtb->ninitialized &&
            jl_egal((jl_value_t*)dta->super, (jl_value_t*)dtb->super) &&
            jl_egal((jl_value_t*)dta->name->names, (jl_value_t*)dtb->name->names) &&
            jl_egal((jl_value_t*)dta->parameters, (jl_value_t*)dtb->parameters));
}

static void check_can_assign_type(jl_binding_t *b)
{
    if (b->constp && b->value != NULL && !jl_is_datatype(b->value))
        jl_errorf("invalid redefinition of constant %s",
                  jl_symbol_name(b->name));
}

void jl_reinstantiate_inner_types(jl_datatype_t *t);
void jl_reset_instantiate_inner_types(jl_datatype_t *t);

void jl_set_datatype_super(jl_datatype_t *tt, jl_value_t *super)
{
    if (!jl_is_datatype(super) || !jl_is_abstracttype(super) ||
        tt->name == ((jl_datatype_t*)super)->name ||
        jl_subtype(super,(jl_value_t*)jl_vararg_type,0) ||
        jl_is_tuple_type(super) ||
        jl_subtype(super,(jl_value_t*)jl_type_type,0) ||
        super == (jl_value_t*)jl_builtin_type) {
        jl_errorf("invalid subtyping in definition of %s",
                  jl_symbol_name(tt->name->name));
    }
    tt->super = (jl_datatype_t*)super;
    jl_gc_wb(tt, tt->super);
}

static int jl_linfo_nslots(jl_lambda_info_t *li)
{
    return jl_array_len(li->slotflags);
}

static int jl_linfo_nssavalues(jl_lambda_info_t *li)
{
    return jl_is_long(li->ssavaluetypes) ? jl_unbox_long(li->ssavaluetypes) : jl_array_len(li->ssavaluetypes);
}

static jl_value_t *eval(jl_value_t *e, interpreter_state *s)
{
    jl_lambda_info_t *lam = s==NULL ? NULL : s->lam;
    if (jl_is_ssavalue(e)) {
        ssize_t id = ((jl_ssavalue_t*)e)->id;
        if (id >= jl_linfo_nssavalues(lam) || id < 0 || s->locals == NULL)
            jl_error("access to invalid SSAValue");
        else
            return s->locals[jl_linfo_nslots(lam) + id];
    }
    if (jl_is_slot(e)) {
        ssize_t n = jl_slot_number(e);
        if (n > jl_linfo_nslots(lam) || n < 1 || s->locals == NULL)
            jl_error("access to invalid slot number");
        jl_value_t *v = s->locals[n-1];
        if (v == NULL)
            jl_undefined_var_error((jl_sym_t*)jl_array_ptr_ref(lam->slotnames,n-1));
        return v;
    }
    if (jl_is_globalref(e)) {
        jl_sym_t *s = jl_globalref_name(e);
        jl_value_t *v = jl_get_global(jl_globalref_mod(e), s);
        if (v == NULL)
            jl_undefined_var_error(s);
        return v;
    }
    if (jl_is_quotenode(e))
        return jl_fieldref(e,0);
    jl_module_t *modu = (lam == NULL || lam->def == NULL) ? jl_current_module : lam->def->module;
    if (jl_is_symbol(e)) {  // bare symbols appear in toplevel exprs not wrapped in `thunk`
        jl_value_t *v = jl_get_global(modu, (jl_sym_t*)e);
        if (v == NULL)
            jl_undefined_var_error((jl_sym_t*)e);
        return v;
    }
    if (!jl_is_expr(e))
        return e;
    jl_expr_t *ex = (jl_expr_t*)e;
    jl_value_t **args = (jl_value_t**)jl_array_data(ex->args);
    size_t nargs = jl_array_len(ex->args);
    if (ex->head == call_sym) {
        return do_call(args, nargs, s);
    }
    else if (ex->head == invoke_sym) {
        return do_invoke(args, nargs, s);
    }
    else if (ex->head == new_sym) {
        jl_value_t *thetype = eval(args[0], s);
        jl_value_t *v=NULL;
        JL_GC_PUSH2(&thetype, &v);
        assert(jl_is_structtype(thetype));
        v = jl_new_struct_uninit((jl_datatype_t*)thetype);
        for(size_t i=1; i < nargs; i++) {
            jl_set_nth_field(v, i-1, eval(args[i], s));
        }
        JL_GC_POP();
        return v;
    }
    else if (ex->head == static_parameter_sym) {
        ssize_t n = jl_unbox_long(args[0]);
        assert(n > 0);
        if (s->sparam_vals)
            return jl_svecref(s->sparam_vals, n - 1);
        if (n <= jl_svec_len(lam->sparam_vals)) {
            jl_value_t *sp = jl_svecref(lam->sparam_vals, n - 1);
            if (!jl_is_typevar(sp))
                return sp;
        }
        // static parameter val unknown needs to be an error for ccall
        jl_error("could not determine static parameter value");
    }
    else if (ex->head == inert_sym) {
        return args[0];
    }
    else if (ex->head == copyast_sym) {
        return jl_copy_ast(eval(args[0], s));
    }
    else if (ex->head == static_typeof_sym) {
        return (jl_value_t*)jl_any_type;
    }
    else if (ex->head == exc_sym) {
        return jl_exception_in_transit;
    }
    else if (ex->head == method_sym) {
        jl_sym_t *fname = (jl_sym_t*)args[0];
        assert(jl_expr_nargs(ex) != 1 || jl_is_symbol(fname));

        if (jl_is_symbol(fname)) {
            jl_value_t **bp=NULL;
            jl_value_t *bp_owner=NULL;
            jl_binding_t *b=NULL;
            if (bp == NULL) {
                b = jl_get_binding_for_method_def(modu, fname);
                bp = &b->value;
                bp_owner = (jl_value_t*)modu;
            }
            jl_value_t *gf = jl_generic_function_def(fname, bp, bp_owner, b);
            if (jl_expr_nargs(ex) == 1)
                return gf;
        }

        jl_value_t *atypes=NULL, *meth=NULL;
        JL_GC_PUSH2(&atypes, &meth);
        atypes = eval(args[1], s);
        meth = eval(args[2], s);
        jl_method_def((jl_svec_t*)atypes, (jl_lambda_info_t*)meth, args[3]);
        JL_GC_POP();
        return jl_nothing;
    }
    else if (ex->head == const_sym) {
        jl_value_t *sym = args[0];
        assert(jl_is_symbol(sym));
        jl_binding_t *b = jl_get_binding_wr(modu, (jl_sym_t*)sym);
        jl_declare_constant(b);
        return (jl_value_t*)jl_nothing;
    }
    else if (ex->head == global_sym) {
        // create uninitialized mutable binding for "global x" decl
        // TODO: handle type decls
        for (size_t i=0; i < jl_array_len(ex->args); i++) {
            assert(jl_is_symbol(args[i]));
            jl_get_binding_wr(modu, (jl_sym_t*)args[i]);
        }
        return (jl_value_t*)jl_nothing;
    }
    else if (ex->head == abstracttype_sym) {
        if (inside_typedef)
            jl_error("cannot eval a new abstract type definition while defining another type");
        jl_value_t *name = args[0];
        jl_value_t *para = eval(args[1], s);
        jl_value_t *super = NULL;
        jl_value_t *temp = NULL;
        jl_datatype_t *dt = NULL;
        JL_GC_PUSH4(&para, &super, &temp, &dt);
        assert(jl_is_svec(para));
        assert(jl_is_symbol(name));
        dt = jl_new_abstracttype(name, NULL, (jl_svec_t*)para);
        jl_binding_t *b = jl_get_binding_wr(modu, (jl_sym_t*)name);
        temp = b->value;
        check_can_assign_type(b);
        b->value = (jl_value_t*)dt;
        jl_gc_wb_binding(b, dt);
        JL_TRY {
            inside_typedef = 1;
            super = eval(args[2], s);
            jl_set_datatype_super(dt, super);
            jl_reinstantiate_inner_types(dt);
        }
        JL_CATCH {
            jl_reset_instantiate_inner_types(dt);
            b->value = temp;
            jl_rethrow();
        }
        b->value = temp;
        if (temp==NULL || !equiv_type(dt, (jl_datatype_t*)temp)) {
            jl_checked_assignment(b, (jl_value_t*)dt);
        }
        JL_GC_POP();
        return (jl_value_t*)jl_nothing;
    }
    else if (ex->head == bitstype_sym) {
        if (inside_typedef)
            jl_error("cannot eval a new bits type definition while defining another type");
        jl_value_t *name = args[0];
        jl_value_t *super = NULL, *para = NULL, *vnb = NULL, *temp = NULL;
        jl_datatype_t *dt = NULL;
        JL_GC_PUSH4(&para, &super, &temp, &dt);
        assert(jl_is_symbol(name));
        para = eval(args[1], s);
        assert(jl_is_svec(para));
        vnb  = eval(args[2], s);
        if (!jl_is_long(vnb))
            jl_errorf("invalid declaration of bits type %s",
                      jl_symbol_name((jl_sym_t*)name));
        ssize_t nb = jl_unbox_long(vnb);
        if (nb < 1 || nb>=(1<<23) || (nb&7) != 0)
            jl_errorf("invalid number of bits in type %s",
                      jl_symbol_name((jl_sym_t*)name));
        dt = jl_new_bitstype(name, NULL, (jl_svec_t*)para, nb);
        jl_binding_t *b = jl_get_binding_wr(modu, (jl_sym_t*)name);
        temp = b->value;
        check_can_assign_type(b);
        b->value = (jl_value_t*)dt;
        jl_gc_wb_binding(b, dt);
        JL_TRY {
            inside_typedef = 1;
            super = eval(args[3], s);
            jl_set_datatype_super(dt, super);
            jl_reinstantiate_inner_types(dt);
        }
        JL_CATCH {
            jl_reset_instantiate_inner_types(dt);
            b->value = temp;
            jl_rethrow();
        }
        b->value = temp;
        if (temp==NULL || !equiv_type(dt, (jl_datatype_t*)temp)) {
            jl_checked_assignment(b, (jl_value_t*)dt);
        }
        JL_GC_POP();
        return (jl_value_t*)jl_nothing;
    }
    else if (ex->head == compositetype_sym) {
        if (inside_typedef)
            jl_error("cannot eval a new data type definition while defining another type");
        jl_value_t *name = args[0];
        assert(jl_is_symbol(name));
        jl_value_t *para = eval(args[1], s);
        assert(jl_is_svec(para));
        jl_value_t *temp = NULL;
        jl_value_t *super = NULL;
        jl_datatype_t *dt = NULL;
        JL_GC_PUSH4(&para, &super, &temp, &dt);
        temp = eval(args[2], s);  // field names
#ifndef NDEBUG
        size_t i, l = jl_svec_len(para);
        for (i = 0; i < l; i++) {
            assert(!((jl_tvar_t*)jl_svecref(para, i))->bound);
        }
#endif
        dt = jl_new_datatype((jl_sym_t*)name, NULL, (jl_svec_t*)para,
                             (jl_svec_t*)temp, NULL,
                             0, args[5]==jl_true ? 1 : 0, jl_unbox_long(args[6]));

        jl_binding_t *b = jl_get_binding_wr(modu, (jl_sym_t*)name);
        temp = b->value;  // save old value
        // temporarily assign so binding is available for field types
        check_can_assign_type(b);
        b->value = (jl_value_t*)dt;
        jl_gc_wb_binding(b,dt);

        JL_TRY {
            inside_typedef = 1;
            // operations that can fail
            super = eval(args[3], s);
            jl_set_datatype_super(dt, super);
            dt->types = (jl_svec_t*)eval(args[4], s);
            jl_gc_wb(dt, dt->types);
            for(size_t i=0; i < jl_svec_len(dt->types); i++) {
                jl_value_t *elt = jl_svecref(dt->types, i);
                if (!jl_is_type(elt) && !jl_is_typevar(elt))
                    jl_type_error_rt(jl_symbol_name(dt->name->name),
                                     "type definition",
                                     (jl_value_t*)jl_type_type, elt);
            }
            jl_reinstantiate_inner_types(dt);
        }
        JL_CATCH {
            jl_reset_instantiate_inner_types(dt);
            b->value = temp;
            jl_rethrow();
        }
        jl_compute_field_offsets(dt);
        if (para == (jl_value_t*)jl_emptysvec && jl_is_datatype_make_singleton(dt)) {
            dt->instance = newstruct(dt);
            jl_gc_wb(dt, dt->instance);
        }

        b->value = temp;
        if (temp==NULL || !equiv_type(dt, (jl_datatype_t*)temp)) {
            jl_checked_assignment(b, (jl_value_t*)dt);
        }
        else {
            // TODO: remove all old ctors and set temp->name->ctor_factory = dt->name->ctor_factory
        }

        JL_GC_POP();
        return (jl_value_t*)jl_nothing;
    }
    else if (ex->head == module_sym) {
        return jl_eval_module_expr(ex);
    }
    else if (ex->head == thunk_sym) {
        return jl_toplevel_eval((jl_value_t*)ex);
    }
    else if (ex->head == error_sym || ex->head == jl_incomplete_sym) {
        if (nargs == 0)
            jl_error("malformed \"error\" expression");
        if (jl_is_string(args[0]))
            jl_errorf("syntax: %s", jl_string_data(args[0]));
        jl_throw(args[0]);
    }
    else if (ex->head == boundscheck_sym || ex->head == inbounds_sym || ex->head == fastmath_sym ||
             ex->head == simdloop_sym || ex->head == meta_sym || ex->head == type_goto_sym) {
        return jl_nothing;
    }
    jl_errorf("unsupported or misplaced expression %s", jl_symbol_name(ex->head));
    return (jl_value_t*)jl_nothing;
}

jl_value_t *jl_toplevel_eval_body(jl_array_t *stmts)
{
    return eval_body(stmts, NULL, 0, 1);
}

static jl_value_t *eval_body(jl_array_t *stmts, interpreter_state *s, int start, int toplevel)
{
    jl_handler_t __eh;
    size_t i=start, ns = jl_array_len(stmts);

    while (1) {
        if (i >= ns)
            jl_error("`body` expression must terminate in `return`. Use `block` instead.");
        jl_value_t *stmt = jl_array_ptr_ref(stmts,i);
        if (jl_is_gotonode(stmt)) {
            i = jl_gotonode_label(stmt)-1;
            continue;
        }
        else if (jl_is_expr(stmt)) {
            jl_sym_t *head = ((jl_expr_t*)stmt)->head;
            if (head == return_sym) {
                jl_value_t *ex = jl_exprarg(stmt,0);
                if (toplevel && jl_is_toplevel_only_expr(ex))
                    return jl_toplevel_eval(ex);
                else
                    return eval(ex, s);
            }
            else if (head == assign_sym) {
                jl_value_t *sym = jl_exprarg(stmt, 0);
                jl_value_t *rhs = eval(jl_exprarg(stmt,1), s);
                if (jl_is_ssavalue(sym)) {
                    ssize_t genid = ((jl_ssavalue_t*)sym)->id;
                    if (genid >= jl_linfo_nssavalues(s->lam) || genid < 0)
                        jl_error("assignment to invalid GenSym location");
                    s->locals[jl_linfo_nslots(s->lam) + genid] = rhs;
                }
                else if (jl_is_slot(sym)) {
                    ssize_t n = jl_slot_number(sym);
                    assert(n <= jl_linfo_nslots(s->lam) && n > 0);
                    s->locals[n-1] = rhs;
                }
                else {
                    jl_module_t *m;
                    if (jl_is_globalref(sym)) {
                        m = jl_globalref_mod(sym);
                        sym = (jl_value_t*)jl_globalref_name(sym);
                    }
                    else {
                        m = (s==NULL || s->lam==NULL || s->lam->def==NULL) ? jl_current_module : s->lam->def->module;
                    }
                    assert(jl_is_symbol(sym));
                    JL_GC_PUSH1(&rhs);
                    jl_binding_t *b = jl_get_binding_wr(m, (jl_sym_t*)sym);
                    jl_checked_assignment(b, rhs);
                    JL_GC_POP();
                }
            }
            else if (head == goto_ifnot_sym) {
                jl_value_t *cond = eval(jl_exprarg(stmt,0), s);
                if (cond == jl_false) {
                    i = jl_unbox_long(jl_exprarg(stmt, 1))-1;
                    continue;
                }
                else if (cond != jl_true) {
                    jl_type_error_rt("toplevel", "if", (jl_value_t*)jl_bool_type, cond);
                }
            }
            else if (head == line_sym) {
                if (toplevel)
                    jl_lineno = jl_unbox_long(jl_exprarg(stmt,0));
                // TODO: interpreted function line numbers
            }
            else if (head == enter_sym) {
                jl_enter_handler(&__eh);
                if (!jl_setjmp(__eh.eh_ctx,1)) {
                    return eval_body(stmts, s, i+1, toplevel);
                }
                else {
#ifdef _OS_WINDOWS_
                    if (jl_exception_in_transit == jl_stackovf_exception)
                        _resetstkoflw();
#endif
                    i = jl_unbox_long(jl_exprarg(stmt,0))-1;
                    continue;
                }
            }
            else if (head == leave_sym) {
                int hand_n_leave = jl_unbox_long(jl_exprarg(stmt,0));
                jl_pop_handler(hand_n_leave);
            }
            else if (toplevel && jl_is_toplevel_only_expr(stmt)) {
                jl_toplevel_eval(stmt);
            }
            else {
                eval(stmt, s);
            }
        }
        else if (jl_is_linenode(stmt)) {
            if (toplevel)
                jl_lineno = jl_linenode_line(stmt);
            // TODO: interpreted function line numbers
        }
        else if (jl_is_newvarnode(stmt)) {
            jl_value_t *var = jl_fieldref(stmt,0);
            assert(jl_is_slot(var));
            ssize_t n = jl_slot_number(var);
            assert(n <= jl_linfo_nslots(s->lam) && n > 0);
            s->locals[n-1] = NULL;
        }
        else {
            eval(stmt, s);
        }
        i++;
    }
    assert(0);
    return NULL;
}

jl_value_t *jl_interpret_call(jl_lambda_info_t *lam, jl_value_t **args, uint32_t nargs, jl_svec_t *sparam_vals)
{
    jl_array_t *stmts = (jl_array_t*)lam->code;
    assert(jl_typeis(stmts, jl_array_any_type));
    jl_value_t **locals;
    JL_GC_PUSHARGS(locals, jl_linfo_nslots(lam) + jl_linfo_nssavalues(lam));
    interpreter_state s;
    s.lam = lam; s.locals = locals; s.sparam_vals = sparam_vals;
    size_t i;
    for(i=0; i < lam->nargs; i++) {
        if (lam->isva && i == lam->nargs-1)
            locals[i] = jl_f_tuple(NULL, &args[i], nargs-i);
        else
            locals[i] = args[i];
    }
    jl_value_t *r = eval_body(stmts, &s, 0, lam->nargs==0);
    JL_GC_POP();
    return r;
}

jl_value_t *jl_interpret_toplevel_thunk(jl_lambda_info_t *lam)
{
    return jl_interpret_call(lam, NULL, 0, NULL);
}

#ifdef __cplusplus
}
#endif
