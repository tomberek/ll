# A `with` whose value would error if forced, but is never consulted
# because every name in the body resolves lexically -- must not force it.
with 5; 1
