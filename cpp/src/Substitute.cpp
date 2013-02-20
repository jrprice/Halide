#include "Substitute.h"

using std::string;

namespace Halide { 
namespace Internal {

class Substitute : public IRMutator {
public:
    Substitute(string v, Expr r) : 
        var(v), replacement(r) {
    }

protected:
    string var;
    Expr replacement;

    void visit(const Variable *v) {
        if (v->name == var) expr = replacement;
        else expr = v;
    }   
};

Expr substitute(string name, Expr replacement, Expr expr) {
    Substitute s(name, replacement);
    return s.mutate(expr);
}

Stmt substitute(string name, Expr replacement, Stmt stmt) {
    Substitute s(name, replacement);
    return s.mutate(stmt);
}

}
}