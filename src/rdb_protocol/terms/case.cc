// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "rdb_protocol/terms/terms.hpp"

#include "rdb_protocol/error.hpp"
#include "rdb_protocol/op.hpp"

namespace ql {

class case_term_t : public op_term_t {
public:
    case_term_t(compile_env_t *env, const protob_t<const Term> &term,
                const char *name, int (*f)(int))
        : op_term_t(env, term, argspec_t(1)), name_(name), f_(f) { }
private:
    virtual counted_t<val_t> eval_impl(scope_env_t *env, args_t *args, eval_flags_t) const {
        std::string s = args->arg(env, 0)->as_str().to_std();
        std::transform(s.begin(), s.end(), s.begin(), f_);
        return new_val(make_counted<const datum_t>(std::move(s)));
    }
    virtual const char *name() const { return name_; }

    virtual bool op_is_deterministic() const { return true; }

    virtual par_level_t par_level() const {
        return params_par_level();
    }

    const char *const name_;
    int (*const f_)(int);
};

counted_t<term_t> make_upcase_term(compile_env_t *env,
                                   const protob_t<const Term> &term) {
    return make_counted<case_term_t>(env, term, "upcase", ::toupper);
}
counted_t<term_t> make_downcase_term(compile_env_t *env,
                                     const protob_t<const Term> &term) {
    return make_counted<case_term_t>(env, term, "downcase", ::tolower);
}

}
