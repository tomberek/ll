#include "ll_resolve.h"

#include "value_util.h"

Value ll_resolve_indirection(Value v) {
    for (;;) {
        if (asnix_value_is_cell(v)) {
            v = asnix_resolve_cell_chain(v);
            continue;
        }
        if (asnix_value_is_lazyapp(v)) {
            v = asnix_resolve_lazyapp(v);
            continue;
        }
        if (asnix_value_is_thunkmap(v)) {
            v = asnix_resolve_thunkmap(v);
            continue;
        }
        return v;
    }
}
