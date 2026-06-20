# HALion VST Preset Metadata

This note documents the metadata table used for HALion preset MediaBay fields.
The same keys can be passed as the fourth argument to HALion's Lua
`savePreset(filename, layer, plugin, attr)` function, and they can be edited
after preset generation with halionbridge's VSTPreset metadata CSV command.

```lua
local attr = {
    MediaAuthor = "Imported Library",
    MediaLibraryManufacturerName = "Library Manufacturer",
    MediaLibraryName = "Source Bank 0",
    MediaComment = "source_bank=0; source_program=0; source_class=kit",
    MediaRating = 3,
    MusicalArticulations = "Staccato",
    MusicalCategory = "Drum&Perc",
    MusicalInstrument = "Drum&Perc|Drumset",
    MusicalMoods = "Energetic",
    MusicalProperties = "Kit",
    MusicalStyle = "Electronic",
    MusicalSubStyle = "Electronic|Breakbeat",
    VST3UnitTypePath = "program/layer",
}
```

HALion accepts these keys as MediaBay attributes when saving a layer or program
preset. The VST3 SDK defines the standard preset keys and predefined vocabularies
for `MusicalInstrument`, `MusicalStyle`, and `MusicalCharacter`. HALion's
MediaBay layer adds authoring rules, Library Creator behavior, and the exact
Lua `savePreset` field names.

## CSV Workflow

Export metadata from an existing preset directory:

```bash
halionbridge vstpreset-metadata export \
  --input-directory /path/to/presets \
  --metadata-csv /path/to/metadata.csv \
  --recursive
```

Edit the CSV in a spreadsheet or script, then apply it to copied presets:

```bash
halionbridge vstpreset-metadata apply \
  --input-directory /path/to/presets \
  --metadata-csv /path/to/metadata.csv \
  --output-directory /path/to/metadata-updated-presets \
  --recursive
```

The CSV file is UTF-8. Export writes UTF-8 text, and apply reads CSV path,
preset-name, and metadata cells as UTF-8. When editing in spreadsheet software,
save as UTF-8 CSV so names with curly quotes, accents, and other non-ASCII text
round trip into filenames and metadata correctly.

`filename_path` is the source match key. It must be a safe relative path below
the input directory and uses forward slashes, for example
`bank_000/000_Planet_Earth.vstpreset`. `target_preset_name` is the output preset
filename stem. It is written in the same relative directory as `filename_path`,
so `target_preset_name=Planet Earth` produces
`bank_000/Planet Earth.vstpreset`. If `target_preset_name` is omitted or empty,
apply preserves the source filename. Apply removes characters that are not
portable across Windows and macOS from `target_preset_name` before writing the
file, for example `WhY Me ?` becomes `WhY Me.vstpreset` and `SPICEBOY:-)`
becomes `SPICEBOY-).vstpreset`. Apply mode is strict: every scanned `.vstpreset`
must have one matching CSV row, every CSV row must match a scanned preset, target
names must still contain a usable filename after sanitization, and renamed output
paths must be unique after sanitization. The input directory is never modified.

The command rewrites only the VST3 preset `Info` metadata chunk. HALion program
state and other preset chunks are preserved byte-for-byte in the output files.
When a metadata column is present in the CSV, an empty cell removes that
attribute and a non-empty cell sets it. Metadata columns omitted from the CSV
leave existing attributes unchanged. Existing CSV files are refused during
export unless `--overwrite` is supplied. Successful export/apply runs print an
`Info:` summary with the number of processed `.vstpreset` files and the output
CSV or preset directory path.

## General Rules

- Use these attributes for user-visible MediaBay browsing and search, not for
  private import bookkeeping.
- Preserve source-bank bookkeeping in `MediaComment`.
  Do not force source bank/class codes into musical taxonomy fields.
- Use exact spelling and the pipe separator `|` for hierarchical values.
- Use fewer accurate musical labels rather than many approximate labels.


## Field Reference

### `MediaAuthor`

Free-text author/designer/company name shown as the MediaBay **Author**
attribute.

Fill this with the organization, sound designer, converter source, or collection
owner that should be credited for the preset. HALion's MediaBay guideline says
Author is a common attribute that should be set manually for each preset.

Good values:

```lua
MediaAuthor = "Imported Library"
MediaAuthor = "Acme Sound Design"
MediaAuthor = "Jane Doe"
```

### `MediaLibraryName`

Free-text library or product name shown as the MediaBay **Library Name**
attribute.

For packaged VST Sound libraries, HALion's Library Creator normally fills
Library Name from the library **Long Name** property. When using `savePreset`
directly, this field can be supplied in the `attr` table.

Good values:

```lua
MediaLibraryName = "World Percussion Collection"
```

### `MediaLibraryManufacturerName`

Free-text library manufacturer shown as the MediaBay **Library Manufacturer**
attribute.

HALion distinguishes this from `MediaAuthor`. Use `MediaAuthor` for the sound
designer, converter source, or preset author. Use `MediaLibraryManufacturerName`
for the company or person that should appear as the library manufacturer. When
building VST Sound libraries with HALion's Library Creator, this corresponds to
the Library Creator **Manufacturer** property.

Good values:

```lua
MediaLibraryManufacturerName = "Acme Instruments"
MediaLibraryManufacturerName = "Jane Doe Soundware"
```

### `MediaComment`

Free-text comment shown as the MediaBay **Comment** attribute.

Use this for human-readable notes or import provenance that does not belong in
MediaBay taxonomy fields. It is the best place for source bank/program/class
bookkeeping if that information must travel with the preset.

Good values:

```lua
MediaComment = "source_bank=0; source_program=0; source_class=kit"
MediaComment = "Imported from source bank 0, program 0."
```

Keep it short and stable. If the data must be machine-readable, prefer a simple
key-value convention and keep the original data in a source CSV manifest.

### `MediaRating`

Integer MediaBay rating. HALion's guideline recommends `3` as a neutral
starting point so users can raise or lower the rating later.

Good values:

```lua
MediaRating = 3
```

### `MusicalCategory`

HALion MediaBay category root for the sound. HALion documents this as a string
with the value shape `"Category"`.

The closest standard vocabulary is the root part of the VST3 SDK
`MusicalInstrument` list. Examples include:

```text
Accordion
Bass
Brass
Chromatic Perc
Drum&Perc
Ethnic
Guitar/Plucked
Keyboard
Musical FX
Organ
Piano
Sound FX
Strings
Synth Lead
Synth Pad
Synth Comp
Vocal
Woodwinds
```

For a drum kit:

```lua
MusicalCategory = "Drum&Perc"
```

Do not put non-standard source class codes such as `kit`, `pr1`, `pr4`, `wnd`, or `cmb` here.
Map them only after their musical meaning is known.

### `MusicalInstrument`

VST3/MediaBay instrument category, usually a root category plus subcategory in
the form `"Category|Subcategory"`.

This is the standard VST3 preset key `MusicalInstrument`. The VST3 SDK provides
the predefined value list (also found in the tab `doc-MusicalInstrument` in the 
LibreOffice/LibreCalc file `preset-metadata-template.ods`. HALion's MediaBay UI 
shows subcategory tags together with their category in this same `Category|Subcategory` 
form.

Good values:

```lua
MusicalInstrument = "Drum&Perc|Drumset"
MusicalInstrument = "Drum&Perc|Percussion"
MusicalInstrument = "Keyboard|E. Piano"
MusicalInstrument = "Woodwinds|Flute"
MusicalInstrument = "Ethnic|African"
MusicalInstrument = "Chromatic Perc|Mallett"
```

For a preset named `Planet Earth` with source class `kit`, the conservative
mapping is:

```lua
MusicalCategory = "Drum&Perc"
MusicalInstrument = "Drum&Perc|Drumset"
```

Use the most specific true musical instrument category. If the source category is
ambiguous, leave this blank in the CSV until manual classification is done.

### `MusicalProperties`

HALion MediaBay **Properties** labels. HALion documents the value shape as a
pipe-separated list: `"Property1|Property2|..."`.

Properties describe how the preset sounds acoustically, without making an
emotional judgment. HALion's guideline recommends precise, sparse labels and
explicitly says arpeggiated or sequenced behavior should generally be expressed
as a property rather than by forcing the preset into `Synth Lead|Arpeggio`.

The closest VST3 SDK standard vocabulary is `MusicalCharacter`, whose predefined
values include:

```text
Mono
Poly
Split
Layer
Glide
Glissando
Major
Minor
Single
Ensemble
Acoustic
Electric
Analog
Digital
Vintage
Modern
Old
New
Clean
Distorted
Dry
Processed
Harmonic
Dissonant
Clear
Noisy
Thin
Rich
Dark
Bright
Cold
Warm
Metallic
Wooden
Glass
Plastic
Percussive
Soft
Fast
Slow
Short
Long
Attack
Release
Decay
Sustain
Fast Attack
Slow Attack
Short Release
Long Release
Static
Moving
Loop
One Shot
```

HALion's MediaBay guideline also explicitly names `Arpeggio` and `Sequenced` as
Properties labels for presets that contain arpeggiated or sequenced behavior. H
So custom labels are therefore valid HALion MediaBay choices, even though they are
not present in the VST3 SDK `MusicalCharacter` constants. The HALion factory library
uses all kinds of labels not found in the VST3 SDK such as `Low`, `Vintage`, 
`Shiny`, `Noisy`, `Wobbling`. However, HALion comes with a set of predefined 
MusicalProperties, so it's probably a good idea not to find new attributes for 
already existing ones.

Good values:

```lua
MusicalProperties = "Percussive|Acoustic"
MusicalProperties = "Soft|Bright|Rich"
MusicalProperties = "Moving|Warm"
MusicalProperties = "Arpeggio"
```

### `MusicalMoods`

Pipe-separated MediaBay mood labels. Moods describe the emotional character of
the sound.

Good values:

```lua
MusicalMoods = "Energetic"
MusicalMoods = "Dark|Aggressive"
```

### `MusicalArticulations`

Pipe-separated MediaBay articulation labels. Use these for how a sound or
instrument is played.

Good values:

```lua
MusicalArticulations = "Staccato"
MusicalArticulations = "Legato|Sustain"
```

### `MusicalStyle`

MediaBay musical style root.

Good values:

```lua
MusicalStyle = "Electronic"
MusicalStyle = "Pop"
```

### `MusicalSubStyle`

Pipe-shaped style/substyle value. HALion can derive the root style when the
substyle is set first in the UI; in CSV form, write the full hierarchy.

Good values:

```lua
MusicalSubStyle = "Electronic|Breakbeat"
MusicalSubStyle = "Rock|Alternative"
```

For a strict standard-derived mapping, represent a kit through:

```lua
MusicalCategory = "Drum&Perc"
MusicalInstrument = "Drum&Perc|Drumset"
```

and use properties such as `Percussive`, `Acoustic`, `Electronic`, or other
verified labels only when they describe the sound.

### `VST3UnitTypePath`

HALion-specific type-path metadata that tells MediaBay whether the saved preset
is a program or a layer/program hierarchy.

HALion documents only these values:

```text
program
program/layer
```

Use:

```lua
VST3UnitTypePath = "program"
```

for a full program preset, and:

```lua
VST3UnitTypePath = "program/layer"
```

for a layer preset saved inside the program/layer unit path. The example uses
`program/layer`, which is appropriate for halionbridge-generated layer presets.

Do not use this field for source paths, filenames, or category paths.

## Example Mapping

For the source row:

```text
bank=0, program=0, class=kit, name=Planet Earth
```

a conservative MediaBay mapping is:

```lua
local attr = {
    MediaAuthor = "Imported Library",
    MediaLibraryManufacturerName = "Library Manufacturer",
    MediaLibraryName = "Source Bank 0",
    MediaComment = "source_bank=0; source_program=0; source_class=kit",
    MediaRating = 3,
    MusicalCategory = "Drum&Perc",
    MusicalInstrument = "Drum&Perc|Drumset",
    MusicalProperties = "Percussive",
    VST3UnitTypePath = "program/layer",
}
```

The source class `kit` is preserved in `MediaComment`. It is reflected musically
through `Drum&Perc|Drumset`. Alternatively, when presets are converted from a specific
source where the organization is relevant, the attributes bank, program and class could
also be encoded in the name although this is probably not strictly as intended in HALion.
