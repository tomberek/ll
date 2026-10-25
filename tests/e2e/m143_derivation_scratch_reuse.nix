/* M143: forcing `top`'s attributes recursively forces `mid1`/`mid2`
 * (each itself a `derivation {}` call with TWO distinct input
 * derivations), which in turn recursively force `x1`/`x2`/`x3`. This
 * exercises `asnix_prim_derivation`'s depth-indexed scratch-buffer
 * pool (M143) three ways at once: real recursion depth (x* are depth
 * 2, mid* are depth 1, top is depth 0), SIBLING reuse of the SAME
 * depth-1 frame across two sequential, non-nested calls (mid1 fully
 * returns before mid2 is ever forced) with DIFFERENT (non-overlapping
 * then partially-overlapping: x2 is shared) group contents each time,
 * and a >1 `ngroups` case (two distinct input drvs per mid*) so the
 * pool's `group_paths`/`group_outs`/`group_order` arrays are actually
 * exercised past index 0. */
let
  mkLeaf = n: derivation { name = n; system = "x86_64-linux"; builder = "/bin/sh"; };
  x1 = mkLeaf "x1";
  x2 = mkLeaf "x2";
  x3 = mkLeaf "x3";
  mid1 = derivation { name = "mid1"; system = "x86_64-linux"; builder = "/bin/sh"; a = x1; b = x2; };
  mid2 = derivation { name = "mid2"; system = "x86_64-linux"; builder = "/bin/sh"; a = x2; b = x3; };
  top = derivation { name = "top"; system = "x86_64-linux"; builder = "/bin/sh"; a = mid1; b = mid2; };
in {
  x1OutPath = x1.outPath;
  x2OutPath = x2.outPath;
  x3OutPath = x3.outPath;
  mid1OutPath = mid1.outPath;
  mid2OutPath = mid2.outPath;
  topDrvPath = top.drvPath;
  topOutPath = top.outPath;
}
