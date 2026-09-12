// Selects the reference-table edge representation.
//
//   default                -> exact reverse adjacency (ref_table_reverse.hpp)
//                              ~40 B/edge, O(1) incoming walk for promotion
//   AARC_CONSERVATIVE=1    -> forward-only multiset (ref_table_forward.hpp)
//                              ~8 B/edge; promotion uses may-components that
//                              tolerate over-promotion (see local_component.hpp)
//
// The rest of the graph layer (engine, cycle detector) uses `outs()` which both
// representations provide; scheme-specific promotion lives in runtime.hpp.

#pragma once

#ifdef AARC_CONSERVATIVE
#include "ref_table_forward.hpp"
#else
#include "ref_table_reverse.hpp"
#endif
