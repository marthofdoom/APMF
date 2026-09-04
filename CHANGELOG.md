## v0.9.0 -- First release

Harbinger (APMF) is a control layer for the AI that drives your NPCs. When more than one mod wants to steer the same actor, Harbinger decides which mod owns which part of it at any moment, so they run together instead of overwriting each other. It arbitrates per facet, not per actor, and withholds only the exact competing input while everything else keeps running.

This is the first public release. It ships as the framework marth's Follower Overhaul 2.0 is built on, and it is open for other mod authors to build against (see INTEGRATION.md and CHANNEL-MAP.md in the repo). It is deep engine work and it is new: some facets are proven in the field, others are built and ready but a mod author would be the first to run them, and the docs are honest about which is which.

- Requires SKSE64 and Address Library for SKSE Plugins. Anniversary Edition (1.6.x) only. No MCM, nothing to configure.
