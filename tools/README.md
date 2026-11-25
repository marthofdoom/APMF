# tools

- `verified_addresses/` generates `native/VerifiedAddresses.h` from the unpacked executables (see `Docs/VERIFIED-ADDRESSES.md`).
- `esscheck.py <save.ess> <FFxxxxxx> ...` is the offline acceptance check of INVARIANTS #19: it lists every change form of a save that holds the RefID of a cast proxy id (the ids `APMF.log` prints in `[castproxy] save sweep`). A clean save prints `CLEAN`. Needs Python 3 with `lz4`.
- `weapon_enchant/weapon_enchant_test.cpp` is the host test of `native/core/WeaponEnchant.h` (a weapon hit's enchantment against the ch.8 gate). Build line in the file.
- `move_order/move_order_test.cpp` is the host test of the strict movement order's decision core and of the attack refusal while it travels (`native/core/MoveOrderLogic.h`, `MoveSuppressLogic.h`). Build line in the file.
- `fire_once/fire_once_test.cpp` is the host test of the ch.8b fire-once ledger (`native/core/FireOnce.h`, ABI v47). Build line in the file.
- `hold_activity/hold_activity_test.cpp` is the host test of the held engagement watchdog's activity reader (`native/core/HoldActivity.h`). Build line in the file.
- `row_equip/row_equip_test.cpp` is the host test of the row equip admission's decision (`native/core/RowEquipLogic.h`) and of `native/core/LogDedupe.h`. Build line in the file.
