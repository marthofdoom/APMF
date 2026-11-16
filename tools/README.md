# tools

- `verified_addresses/` generates `native/VerifiedAddresses.h` from the unpacked executables (see `Docs/VERIFIED-ADDRESSES.md`).
- `esscheck.py <save.ess> <FFxxxxxx> ...` is the offline acceptance check of INVARIANTS #19: it lists every change form of a save that holds the RefID of a cast proxy id (the ids `APMF.log` prints in `[castproxy] save sweep`). A clean save prints `CLEAN`. Needs Python 3 with `lz4`.
