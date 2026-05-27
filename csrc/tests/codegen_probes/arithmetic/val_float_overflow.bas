' GREEN regression probe -- VAL("<huge-number>") on edge / overflow
' float literals must match Python byte-for-byte at the asm + binary
' stages. Confirmed at probe-authoring time across 1e40, -1e40,
' 3.5e38 (just past Spectrum float max), and 1.7e308 (well past).
'
' Anchors: src/symbols/val.py / src/symbols/strings.py VAL handling
' and Python's float() ValueError -> 0.0 conversion fallback.
'
' Closes stage-06 (VAL float-overflow residual). The residual was
' silently closed by earlier structural fixes -- no isolated VAL
' patch landed; the probe locks the behavior in for regression.

LET a = VAL("1e40")
PRINT a
