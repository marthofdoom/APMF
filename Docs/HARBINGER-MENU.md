# The Harbinger menu page

Harbinger adds one page to the **SKSE Menu Framework** (3.x). It shows who is driving each AI facet of the NPCs in your game, who wins, and lets you change a mod's priority live. It is optional. Without the framework Harbinger runs from `Data/SKSE/Plugins/APMF_Sources.ini` and logs one line saying the page is unavailable.

**Where it is.** Open the SKSE Menu Framework menu in game, then the section `Harbinger`, item `Sources`.

**What you see, top to bottom**

1. **Alerts line.** Green "No alerts" when every write was traced to a named source. Otherwise a collapsible line "N alert(s)". Each alert is a change to an AI facet that Harbinger could not trace to a mod. Those changes are always allowed and never denied.
2. **Hide idle sources.** A checkbox. It only changes the view and is never saved. Idle sources are ones not seen in the last 3 game loads.
3. **One collapsible header per facet:** Targets, Packages, Equip, Detection, Casting, Combat actions, Combat entry and approach, Actor values (speed, disposition), Core (the order the page renders them). Below the census, "Loaded SKSE plugins not seen writing yet" lists every SKSE plugin in the process that has not yet written a facet. Under each header sits a checkbox, "Unknown sources can be overridden" (off by default, saved to `[UnknownSources]` in `APMF_Sources.ini`): when ticked, a change Harbinger cannot trace ranks at the Unattributed priority instead of simply being allowed. A header is closed unless that facet has an alert. Its title shows the current winner and its priority, the number of sources and any alerts.
4. **A table inside each header**, one row per source, in fixed columns:
   - **Source:** the mod, plugin or built in name. The winner is green, idle sources are grey and listed last, "Unattributed" is amber.
   - **Kind:** `client` (a mod that asks Harbinger), `hook` (another mod's hook DLL found on one of Harbinger's seats), `plugin`, `engine`, `direct` (Papyrus and console), `loaded` (an SKSE plugin found in the process at startup that has not been seen writing yet; it takes its real kind on its first write), or a combination such as `hook+client`.
   - **Hits:** how many times the source wrote the facet.
   - **Priority (0 = off):** a slider from 0 to 100. Moving it applies at once and is saved to `APMF_Sources.ini`. "Unattributed" has no slider: it is always allowed.
5. **Seat census.** A collapsed header at the bottom. It lists the engine seats where another mod's hook sits inside Harbinger's or on top of it, and whether Harbinger is still outermost.

**Details on hover.** Hover a source name, or focus its slider with a gamepad, to see its counters (hits, overruled, stood), when it last wrote the facet and which seats it hooks.

**How priorities work.** A claim from one source beats another source's change on the same facet only with a higher priority. Defaults: other mods 20, mods that ask Harbinger 50, Papyrus and console 75. Setting a priority to 0 turns that source off for that facet. See `Docs/INI-REFERENCE.md` for the file format.

**Gamepad.** The page works with the framework's controller navigation. The focused slider shows the same details tooltip.

**Threading note.** The page only reads counters and queues slider changes. The change is applied and written to the file on the game's main thread.

**Which facet header covers which intent.** Each header on the page is one sources facet. The reference for each intent is in [FACETS.md](FACETS.md), and [Appendix A](FACETS.md#appendix-a-sources-and-priorities-per-facet) says what Harbinger enforces on each.

| Header | Intents and seats | Reference |
|---|---|---|
| Targets | ch.6 combat target, ch.20 target pin | [ch.6](FACETS.md#ch6-combat-target-kintent_combattarget), [ch.20](FACETS.md#ch20-target-pin-kintent_targetpin) |
| Packages | ch.9 offer package, ch.19 travel, the Spectator hold-off | [ch.9](FACETS.md#ch9-offer-package-kintent_offerpackage), [ch.19](FACETS.md#ch19-travel-kintent_travel) |
| Equip | ch.15 equipment, ch.17 equip authority, the hand block | [ch.15](FACETS.md#ch15-equipment-kintent_equipment), [ch.17](FACETS.md#ch17-equip-authority-kintent_equipauthority) |
| Detection | ch.16 detection, the actor-value watch | [ch.16](FACETS.md#ch16-detection-kintent_detection) |
| Casting | ch.8 select spell, ch.8b cast, ch.25 cast guard | [ch.8](FACETS.md#ch8-select-spell-kintent_selectspell), [ch.8b](FACETS.md#ch8b-cast-kintent_cast), [ch.25](FACETS.md#ch25-cast-guard-kintent_castguard) |
| Combat actions | ch.7 combat action, ch.23 pursuit leash | [ch.7](FACETS.md#ch7-combat-action-kintent_combataction), [ch.23](FACETS.md#ch23-pursuit-leash-kintent_pursuitleash) |
| Combat entry and approach | ch.21 combat entry, ch.22 re-entry deny, ch.24 combat approach | [ch.21](FACETS.md#ch21-combat-entry-kintent_combatentry), [ch.22](FACETS.md#ch22-combat-re-entry-deny-kintent_combatreentrydeny), [ch.24](FACETS.md#ch24-combat-approach-kintent_combatapproach) |
| Actor values (speed, disposition) | ch.1a gait, ch.11 disposition | [ch.1a](FACETS.md#ch1a-gait-kintent_gait), [ch.11](FACETS.md#ch11-disposition-kintent_disposition) |
| Core | ch.1, 3, 4, 5, 10, 12, 14 and the engine update seat | [FACETS.md](FACETS.md#facet-index) |

The seat census and the other seats that only name who changes a value are described under [Observe-only facets](FACETS.md#observe-only-facets-no-claim-api).
