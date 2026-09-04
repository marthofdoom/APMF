#!/usr/bin/env bash
# Nexus banner for APMF / Harbinger (1300x372).
# Motif: a single vertical beacon / eye-slit burning on near-black, the
# directing intelligence that decides who drives an NPC (a Mass Effect
# Harbinger nod). House style matches the sibling mods (dark ground, gold P052
# type), but this is a FRAMEWORK not an overhaul: no character motif, a colder
# sentinel beacon and a Reaper amber accent instead of the overhauls' warm
# crimson/amethyst/green.
set -euo pipefail
cd "$(dirname "$0")"
W=1300; H=372
P052=/usr/share/fonts/opentype/urw-base35/P052-Roman.otf
P052I=/usr/share/fonts/opentype/urw-base35/P052-Italic.otf

# The beacon: a tall vertical slit, sat on the right.
CX=1096; CY=186
SLT=96                 # slit half-height
RAIL=52                # flanking monolith rail offset

# 1) Base: near-black vertical gradient + a soft amber radial glow.
magick -size ${W}x${H} gradient:'#16181d-#08090b' \
  \( -size ${W}x${H} radial-gradient:'#2b1e12-#000000' -evaluate multiply 0.9 \) \
  -compose screen -composite base.png

# 2) The beacon: a heavily blurred amber glow underlayer, a translucent amber
#    lens, two faint flanking monolith rails, then a crisp white-hot core slit
#    and a bright central glint.
magick base.png \
  \( -clone 0 -fill '#e8963a' -stroke none \
     -draw "roundrectangle $((CX-15)),$((CY-SLT)) $((CX+15)),$((CY+SLT)) 15,15" -blur 0x18 \) \
  -compose screen -composite \
  -stroke none -fill 'rgba(224,123,42,0.30)' \
     -draw "roundrectangle $((CX-14)),$((CY-SLT)) $((CX+14)),$((CY+SLT)) 14,14" \
  -fill none -stroke 'rgba(233,200,126,0.28)' -strokewidth 2.0 \
     -draw "line $((CX-RAIL)),$((CY-SLT-8)) $((CX-RAIL)),$((CY+SLT+8))" \
     -draw "line $((CX+RAIL)),$((CY-SLT-8)) $((CX+RAIL)),$((CY+SLT+8))" \
  -stroke none -fill 'rgba(255,240,216,0.92)' \
     -draw "roundrectangle $((CX-5)),$((CY-SLT+18)) $((CX+5)),$((CY+SLT-18)) 5,5" \
  -fill 'rgba(255,247,235,0.98)' -draw "circle $CX,$CY $CX,$((CY-15))" \
  -fill 'rgba(232,150,58,0.55)' -draw "circle $CX,$CY $CX,$((CY-30))" \
  beacon.png

# 3) Typography (left-aligned so it clears the beacon).
magick beacon.png \
  -font "$P052" -gravity northwest \
  -fill '#9a8a5e' -pointsize 30 -kerning 14 -annotate +72+52 "m a r t h" \
  -fill '#eae1cb' -pointsize 88 -kerning 6 -annotate +68+88 "HARBINGER" \
  -fill '#d68a3c' -pointsize 26 -kerning 26 -annotate +76+208 "A P M F" \
  nexus-banner.png
rm -f base.png beacon.png
echo "wrote nexus-banner.png (${W}x${H})"
