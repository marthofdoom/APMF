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
