# Kenney sprite library

39,713 individual sprite PNGs (64.4 MB) from [Kenney](https://kenney.nl)'s
2D and UI asset packs (CC0 — see ../LICENSE.txt). Excluded: spritesheets,
tilemaps, previews, vector sources, the Planets pack, and anything ≥20 KB —
files that big are textures and backgrounds (skyboxes, light masks,
parchment), not composable sprites, and they were half the bytes. Every
file here is a single drawable sprite, ready for
`surfer.image(open(path,'rb').read())`.

`index.tsv` maps every file to a description and pixel size, one row per
sprite, tab-separated:

```
path                                         description                                          size   bytes
ui/UI Pack - Sci-fi/Blue/bar_round_gloss_large_m.png  UI Assets, UI Pack - Sci-fi, Blue, Bar Round Gloss Large M  24x24  126
```

The point: a program (or an LLM) can grep the descriptions to find art —
"red spaceship", "glossy blue button", "explosion" — and load the file
by path, without ever opening images to look at them.

Regenerate from a Kenney source tree with:

```
python3 tools/kenney_index.py ~/outside/kenney
```
