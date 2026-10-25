# M184's fast path is gated on `slen <= ASNIX_REGEX_CUSTOM_MAX_SPAN`
# (2000, the pre-existing M68 recursion-safety cap) -- a subject longer
# than that must keep using the unmodified glibc-regexec path
# regardless of how many times this exact pattern has already been
# seen (verified/trusted or not). "a" x 2500 is a whole-string match
# for `a*` either way; the point is that asnix doesn't crash/diverge
# when forced to repeatedly fall back for an over-cap subject.
let long = builtins.concatStringsSep "" (builtins.genList (_: "a") 2500);
in [ (builtins.match "a*" long) (builtins.match "a*" long) ]
