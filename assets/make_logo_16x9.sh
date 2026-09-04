#!/usr/bin/env bash
# 16:9 logo for APMF / Harbinger (1920x1080) - the banner's beacon motif and
# house style (dark ground, Reaper amber glow, gold P052 type) recomposed for a
# 16:9 canvas: the beacon as a centered hero in the upper third above stacked,
# centered typography. Use for splash / Nexus header / video thumbnail. Sibling
# of make_banner.sh.
set -euo pipefail
cd "$(dirname "$0")"
W=1920; H=1080
P052=/usr/share/fonts/opentype/urw-base35/P052-Roman.otf
P052I=/usr/share/fonts/opentype/urw-base35/P052-Italic.otf

# Beacon, centered horizontally, upper third (hero). ~1.9x the banner beacon.
CX=960; CY=320
SLT=182
RAIL=98

# 1) Base: near-black vertical gradient + a soft amber radial glow.
magick -size ${W}x${H} gradient:'#16181d-#08090b' \
  \( -size ${W}x${H} radial-gradient:'#2b1e12-#000000' -evaluate multiply 0.9 \) \
  -compose screen -composite base.png

# 2) The beacon: blurred amber glow underlayer, translucent lens, faint flanking
#    monolith rails, crisp white-hot core slit, bright central glint. Same recipe
#    as the banner, scaled.
magick base.png \
  \( -clone 0 -fill '#e8963a' -stroke none \
     -draw "roundrectangle $((CX-28)),$((CY-SLT)) $((CX+28)),$((CY+SLT)) 28,28" -blur 0x26 \) \
  -compose screen -composite \
  -stroke none -fill 'rgba(224,123,42,0.30)' \
     -draw "roundrectangle $((CX-26)),$((CY-SLT)) $((CX+26)),$((CY+SLT)) 26,26" \
  -fill none -stroke 'rgba(233,200,126,0.28)' -strokewidth 3.0 \
     -draw "line $((CX-RAIL)),$((CY-SLT-16)) $((CX-RAIL)),$((CY+SLT+16))" \
     -draw "line $((CX+RAIL)),$((CY-SLT-16)) $((CX+RAIL)),$((CY+SLT+16))" \
  -stroke none -fill 'rgba(255,240,216,0.92)' \
     -draw "roundrectangle $((CX-9)),$((CY-SLT+34)) $((CX+9)),$((CY+SLT-34)) 9,9" \
  -fill 'rgba(255,247,235,0.98)' -draw "circle $CX,$CY $CX,$((CY-28))" \
  -fill 'rgba(232,150,58,0.55)' -draw "circle $CX,$CY $CX,$((CY-58))" \
  beacon.png

# 3) Typography - centered stack below the beacon.
magick beacon.png -gravity north \
  -font "$P052" \
  -fill '#9a8a5e' -pointsize 44 -kerning 22 -annotate +0+600 "m a r t h" \
  -fill '#eae1cb' -pointsize 134 -kerning 9 -annotate +0+658 "HARBINGER" \
  -fill '#d68a3c' -pointsize 38 -kerning 40 -annotate +8+820 "A P M F" \
  logo-16x9.png
rm -f base.png beacon.png
echo "wrote logo-16x9.png (${W}x${H})"
