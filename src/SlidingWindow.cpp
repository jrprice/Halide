#include "SlidingWindow.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Scope.h"
#include "Debug.h"
#include "Substitute.h"
#include "IRPrinter.h"
#include "Simplify.h"
#include "Derivative.h"
#include "Bounds.h"

namespace Halide {
namespace Internal {

using std::string;
using std::map;

namespace {

// Does an expression depend on a particular variable?
class ExprDependsOnVar : public IRVisitor {
    using IRVisitor::visit;

    void visit(const Variable *op) {
        if (op->name == var) result = true;
    }

    void visit(const Let *op) {
        op->value.accept(this);
        // The name might be hidden within the body of the let, in
        // which case there's no point descending.
        if (op->name != var) {
            op->body.accept(this);
        }
    }
public:

    bool result;
    string var;

    ExprDependsOnVar(string v) : result(false), var(v) {
    }
};

bool expr_depends_on_var(Expr e, string v) {
    ExprDependsOnVar depends(v);
    e.accept(&depends);
    return depends.result;
}


class ExpandExpr : public IRMutator {
    using IRMutator::visit;
    const Scope<Expr> &scope;

    void visit(const Variable *var) {
        if (scope.contains(var->name)) {
            expr = scope.get(var->name);
            debug(3) << "Fully expanded " << var->name << " -> " << expr << "\n";
        } else {
            expr = var;
        }
    }

public:
    ExpandExpr(const Scope<Expr> &s) : scope(s) {}

};

// Perform all the substitutions in a scope
Expr expand_expr(Expr e, const Scope<Expr> &scope) {
    ExpandExpr ee(scope);
    Expr result = ee.mutate(e);
    debug(3) << "Expanded " << e << " into " << result << "\n";
    return result;
}

}

// Perform sliding window optimization for a function over a
// particular serial for loop
class SlidingWindowOnFunctionAndLoop : public IRMutator {
    Function func;
    string loop_var;
    Expr loop_min;
    Scope<Expr> scope;

    map<string, Expr> replacements;

    using IRMutator::visit;

    void visit(const Pipeline *op) {
        if (op->name != func.name()) {
            IRMutator::visit(op);
        } else {

            stmt = op;

            // We're interested in the case where exactly one of the
            // dimensions of the buffer has a min/extent that depends
            // on the loop_var.
            string dim = "";
            int dim_idx = 0;
            Expr min_required, max_required;

            debug(3) << "Considering sliding " << func.name()
                     << " along loop variable " << loop_var << "\n"
                     << "Region provided:\n";

            string prefix = func.name() + ".s" + int_to_string(func.updates().size()) + ".";
            for (int i = 0; i < func.dimensions(); i++) {
                // Look up the region required of this function's last stage
                string var = prefix + func.args()[i];
                internal_assert(scope.contains(var + ".min") && scope.contains(var + ".max"));
                Expr min_req = scope.get(var + ".min");
                Expr max_req = scope.get(var + ".max");
                min_req = expand_expr(min_req, scope);
                max_req = expand_expr(max_req, scope);

                debug(3) << func.args()[i] << ":" << min_req << ", " << max_req  << "\n";
                if (expr_depends_on_var(min_req, loop_var) ||
                    expr_depends_on_var(max_req, loop_var)) {
                    if (!dim.empty()) {
                        dim = "";
                        min_required = Expr();
                        max_required = Expr();
                        break;
                    } else {
                        dim = func.args()[i];
                        dim_idx = i;
                        min_required = min_req;
                        max_required = max_req;
                    }
                }
            }

            if (!min_required.defined()) {
                debug(3) << "Could not perform sliding window optimization of "
                         << func.name() << " over " << loop_var << " because either zero "
                         << "or many dimensions of the function dependended on the loop var\n";
                return;
            }

            // If the function is not pure in the given dimension, give up
            bool pure = true;
            for (size_t i = 0; i < func.updates().size(); i++) {
                const Variable *var = func.updates()[i].args[dim_idx].as<Variable>();
                if (!var) {
                    pure = false;
                } else if (var->name != dim) {
                    pure = false;
                }
            }
            if (!pure) {
                debug(3) << "Could not performance sliding window optimization of "
                         << func.name() << " over " << loop_var << " because the function "
                         << "scatters along the related axis.\n";
                return;
            }

            bool can_slide_up = false;
            bool can_slide_down = false;

            MonotonicResult monotonic_min = is_monotonic(min_required, loop_var);
            MonotonicResult monotonic_max = is_monotonic(max_required, loop_var);

            if (monotonic_min == MonotonicIncreasing || monotonic_min == Constant) {
                can_slide_up = true;
            }

            if (monotonic_max == MonotonicDecreasing || monotonic_max == Constant) {
                can_slide_down = true;
            }


            if (!can_slide_up && !can_slide_down) {
                debug(3) << "Not sliding " << func.name()
                         << " over dimension " << dim
                         << " along loop variable " << loop_var
                         << " because I couldn't prove it moved monotonically along that dimension\n"
                         << "Min is " << min_required << "\n"
                         << "Max is " << max_required << "\n";
                return;
            }

            // Ok, we've isolated a function, a dimension to slide
            // along, and loop variable to slide over.
            debug(3) << "Sliding " << func.name()
                     << " over dimension " << dim
                     << " along loop variable " << loop_var << "\n";

            Expr loop_var_expr = Variable::make(Int(32), loop_var);

            Expr prev_max_plus_one = substitute(loop_var, loop_var_expr - 1, max_required) + 1;
            Expr prev_min_minus_one = substitute(loop_var, loop_var_expr - 1, min_required) - 1;

            Expr new_min, new_max;
            if (can_slide_up) {
                new_min = select(loop_var_expr == loop_min, min_required, prev_max_plus_one);
                new_max = max_required;
            } else {
                new_min = min_required;
                new_max = select(loop_var_expr == loop_min, max_required, prev_min_minus_one);
            }

            Expr early_stages_min_required = new_min;
            Expr early_stages_max_required = new_max;

            debug(3) << "Sliding " << func.name() << ", " << dim << "\n"
                     << "Pushing min up from " << min_required << " to " << new_min << "\n"
                     << "Shrinking max from " << max_required << " to " << new_max << "\n";

            // Now redefine the appropriate regions required
            if (can_slide_up) {
                replacements[prefix + dim + ".min"] = new_min;
            } else {
                replacements[prefix + dim + ".max"] = new_max;
            }

            for (size_t i = 0; i < func.updates().size(); i++) {
                string n = func.name() + ".s" + int_to_string(i) + "." + dim;
                replacements[n + ".min"] = Variable::make(Int(32), prefix + dim + ".min");
                replacements[n + ".max"] = Variable::make(Int(32), prefix + dim + ".max");
            }

            // Ok, we have a new min/max required and we're going to
            // rewrite all the lets that define bounds required. Now
            // we need to additionally expand the bounds required of
            // the last stage to cover values produced by stages
            // before the last one. Because, e.g., an intermediate
            // stage may be unrolled, expanding its bounds provided.
            if (op->update.defined()) {
                Box b = box_provided(op->produce, func.name());
                merge_boxes(b, box_provided(op->update, func.name()));
                if (can_slide_up) {
                    string n = prefix + dim + ".min";
                    Expr var = Variable::make(Int(32), n);
                    stmt = LetStmt::make(n, min(var, b[dim_idx].min), stmt);
                } else {
                    string n = prefix + dim + ".max";
                    Expr var = Variable::make(Int(32), n);
                    stmt = LetStmt::make(n, max(var, b[dim_idx].max), stmt);
                }
            }


        }
    }

    void visit(const For *op) {
        // It's not safe to enter an inner loop whose bounds depend on
        // the var we're sliding over.
        Expr min = expand_expr(op->min, scope);
        Expr extent = expand_expr(op->extent, scope);
        if (is_monotonic(min, loop_var) != Constant ||
            is_monotonic(extent, loop_var) != Constant) {
            stmt = op;
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const LetStmt *op) {
        scope.push(op->name, simplify(expand_expr(op->value, scope)));
        Stmt new_body = mutate(op->body);

        Expr value = op->value;

        map<string, Expr>::iterator iter = replacements.find(op->name);
        if (iter != replacements.end()) {
            value = iter->second;
            replacements.erase(iter);
        }

        if (new_body.same_as(op->body) && value.same_as(op->value)) {
            stmt = op;
        } else {
            stmt = LetStmt::make(op->name, value, new_body);
        }
        scope.pop(op->name);
    }

public:
    SlidingWindowOnFunctionAndLoop(Function f, string v, Expr v_min) : func(f), loop_var(v), loop_min(v_min) {}
};

// Perform sliding window optimization for a particular function
class SlidingWindowOnFunction : public IRMutator {
    Function func;

    using IRMutator::visit;

    void visit(const For *op) {
        debug(3) << " Doing sliding window analysis over loop: " << op->name << "\n";

        Stmt new_body = op->body;

        new_body = mutate(new_body);

        if (op->for_type == For::Serial || op->for_type == For::Unrolled) {
            new_body = SlidingWindowOnFunctionAndLoop(func, op->name, op->min).mutate(new_body);
        }

        if (new_body.same_as(op->body)) {
            stmt = op;
        } else {
            stmt = For::make(op->name, op->min, op->extent, op->for_type, new_body);
        }
    }

public:
    SlidingWindowOnFunction(Function f) : func(f) {}
};

// Perform sliding window optimization for all functions
class SlidingWindow : public IRMutator {
    const map<string, Function> &env;

    using IRMutator::visit;

    void visit(const Realize *op) {
        // Find the args for this function
        map<string, Function>::const_iterator iter = env.find(op->name);

        // If it's not in the environment it's some anonymous
        // realization that we should skip (e.g. an inlined reduction)
        if (iter == env.end()) {
            IRMutator::visit(op);
            return;
        }

        // If the Function in question has the same compute_at level
        // as its store_at level, skip it.
        const Schedule &sched = iter->second.schedule();
        if (sched.compute_level() == sched.store_level()) {
            IRMutator::visit(op);
            return;
        }

        Stmt new_body = op->body;

        debug(3) << "Doing sliding window analysis on realization of " << op->name << "\n";

        new_body = SlidingWindowOnFunction(iter->second).mutate(new_body);

        new_body = mutate(new_body);

        if (new_body.same_as(op->body)) {
            stmt = op;
        } else {
            stmt = Realize::make(op->name, op->types, op->bounds, op->condition, new_body);
        }
    }
public:
    SlidingWindow(const map<string, Function> &e) : env(e) {}

};

Stmt sliding_window(Stmt s, const map<string, Function> &env) {
    return SlidingWindow(env).mutate(s);
}

}
}
