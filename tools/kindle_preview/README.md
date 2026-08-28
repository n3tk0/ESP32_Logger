# Kindle dashboard preview

Renders `GET /kindle` without a device, a board, or a network — the picture in
[`docs/KINDLE_DASHBOARD.md`](../../docs/KINDLE_DASHBOARD.md) comes from here.

```bash
python3 tools/kindle_preview/preview.py hourly bg calm 600   # -> preview.html
node    tools/kindle_preview/shot.mjs                        # -> kindle.png
cp tools/kindle_preview/kindle.png docs/images/kindle-dashboard.png
```

Arguments, all optional and positional: outlook (`hourly` | `daily`), language
(`en` | `bg`), weather (`calm` | `cold`), `KINDLE_PAGE_W` (600, 536, 1072 …),
`warn`, and a clock style.

`warn` draws the low-battery badge the firmware only draws when an ESP-NOW node
is actually running down; it is off by default, so the default picture is the
ordinary page rather than an alarmed one. The sixth argument is one of `plain`
(default), `boxed`, `ruled` or `dated` — the runtime clock styles from
**Settings → E-ink dashboard**. Pass `-` for an argument you want to skip.

```bash
python3 tools/kindle_preview/preview.py hourly bg calm 600 warn
python3 tools/kindle_preview/preview.py hourly bg calm 600 -    boxed
```

The screenshot script takes an output path and a viewport, and prints the
rendered page height against the budget — the number to watch, since a page over
800 scrolls on a device with no scrollbar to say so. The badge is a float and
costs no height; 796 with it and 796 without is the check that it stays that
way.

Needs `playwright` and a Chromium; set `CHROMIUM_PATH` if yours is not at
`/opt/pw-browsers/chromium`.

## What it is evidence of, and what it is not

**The stylesheet is extracted from the firmware**, not kept as a copy:
`preview.py` reads the `KD_S` / `KD_N` calls out of `src/web/KindleDashboard.cpp`
and replays them through the same `kdPx()` rounding. Sizes, greys and spacing
therefore cannot drift from the code, and the script refuses to run if the
extraction yields less than 1500 characters — the emitter having changed shape
should fail loudly rather than quietly render the wrong page.

The clock-style overrides are extracted the same way, out of `kdSkinCss()` in
`src/web/KindleSkin.h`. Each style claims to keep the block at the height the
design fixed, and a claim like that is worth nothing unless the thing being
measured is what the device actually emits.

**The markup and the strings are this script's own**, and that half can drift.
It has: an earlier render showed a chart key describing a hatched band the
firmware had stopped drawing, so the picture was right and the device was
wrong. After touching the page's HTML, re-render and read the two side by side.

The readings are synthetic and seeded (`random.seed(7)`), so the same arguments
give the same picture and a diff of the PNG means a real change.
