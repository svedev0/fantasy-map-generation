# fantasy-map-generation

A small, dependency-free C++ program that procedurally generates a 2D fantasy
map. A shaded-relief island rendered to a BMP image. Terrain is built from
multi-octave Perlin noise, warped and shaped into an island, then rendered with
a hypsometric (elevation) color ramp, hillshading, depth-shaded water, and an
outlined coastline.

## Build

Run the build script:

```bat
.\build.bat
```

## Run

```
mapgen [options]
```

With no arguments it generates a map using the default values.

| Option                         | Default | Meaning                                                     |
|--------------------------------|---------|-------------------------------------------------------------|
| `-w`,  `--width <int>`         | 512     | Map width in pixels                                         |
| `-h`,  `--height <int>`        | 512     | Map height in pixels                                        |
| `-s`,  `--seed <int>`          | 7       | Random seed (`0` for a random seed)                         |
| `-f`,  `--falloff <double>`    | 1.4     | Radial-falloff strength (`0` disables edge-to-edge terrain) |
| `-sl`, `--sea-level <double>`  | 0.52    | Land/sea threshold in `[0, 1]`                              |
| `-o`,  `--octaves <int>`       | 6       | Number of fBm octaves                                       |
| `-b`,  `--base-res <int>`      | 4       | fBm base lattice resolution                                 |
| `-l`,  `--lacunarity <double>` | 2.0     | Per-octave frequency multiplier                             |
| `-g`,  `--gain <double>`       | 0.5     | Per-octave amplitude multiplier                             |

The output file is named `map_<width>x<height>_<seed>.bmp`.

Note: Non-square maps are supported.

## Data model

Every 2D field (noise layers, the heightmap, hillshade) is a `Grid`: a single
contiguous `std::vector<double>` in row-major order. `Grid::operator[](y)`
returns a pointer to the start of row `y`, so code reads naturally as
`grid[y][x]` while the storage stays flat. One allocation, cache-friendly, and
easy for the compiler to vectorise.

## The pipeline

Simplified diagram:

```
seed
  ▼
rng
  ▼
base heightmap (fBm - multi-octave Perlin)
  ▼
domain-warp offset fields (additional fBm fields)
  ▼
warp-resample (interpolation)
  ▼
radial falloff (shape landmass)
  ▼
normalise
  ▼
land/sea mask
  ▼
hillshade (relief lighting)
  ▼
hypsometric land shading (colour)
  ▼
depth-based sea shading (colour)
  ▼
black outlined coastline
  ▼
RGB pixel buffer
  ▼
writeBmp
  ▼
.bmp file
```

### 1. Random numbers - `Rng`

The **xorshift64** generator mixes the seed with the 64-bit fractional bits of
the golden ratio (`0x9E3779B97F4A7C15`) to scatter low-entropy seeds, and
steers away from the all-zero state (xorshift's only fixed point). `nextU64()`
does the standard `13/7/17` shift-xor sequence. `nextDouble()` takes the top 53
bits and scales by `2⁻⁵³` to land in `[0, 1)`, then `uniform(lo, hi)` maps that
onto a range.

Because the algorithm is tiny and fully specified, generation is deterministic
for a given seed and the order in which noise layers draw from the stream is
also fixed (base terrain, then `warpX`, then `warpY`), so the same seed always
reproduces the same map.

### 2. One octave of Perlin noise - `addPerlinOctave`

Classic 2D gradient noise:

1. **Gradient lattice.** A coarse grid of `(resY+1) × (resX+1)` nodes each gets
   a random unit gradient vector `(cos θ, sin θ)` for a uniform angle
   `θ ∈ [0, 2π)`.
2. **Cell coordinates.** The pixel grid is divided into `resY × resX` cells.
   Cell sizes are computed as fractional values (`cellW = width / resX` as a
   `double`), so the lattice need not divide the image evenly (this is what
   lets arbitrary dimensions work). For each pixel, the code finds its cell
   index and its fractional position `frac ∈ [0, 1)` within the cell.
3. **Corner dot products.** For the four cell corners, it dots that corner's
   gradient with the vector from the corner to the pixel.
4. **Smooth interpolation.** The four dot products are blended bilinearly, but
   weighted by `perlinFade` (the quintic smootherstep curve
   `6t⁵ − 15t⁴ + 10t³`), which has zero first and second derivatives at the
   ends, removing the grid-aligned creasing that plain linear interpolation
   produces.
5. **Rescale & accumulate.** The result is multiplied by `√2` to bring it back
   to roughly `[-1, 1]`, then scaled by `amplitude` and added into the output
   grid.

### 3. Fractal noise - `fbm`

Fractional Brownian motion sums several octaves of Perlin noise at increasing
frequency and decreasing amplitude:

- Start at `amplitude = 1`, `resolution = baseRes` (4).
- Each octave: add one Perlin octave at the current resolution, then multiply
  amplitude by gain and resolution by lacunarity.

Six octaves at defaults use resolutions `4 -> 8 -> 16 -> 32 -> 64 -> 128`. Low
octaves give continents-scale shape. High octaves add fine coastal detail. The
summed result is unnormalised (roughly `[-2, 2]`). Normalisation happens later.

### 4. Heightmap assembly - `generateHeightmap`

This is where noise becomes terrain:

1. **Base terrain.** `base = fbm(...)` with the configured octaves.
2. **Domain warp.** Two more fBm fields, `warpX` and `warpY` (4 octaves each),
   are used to displace where each pixel samples the base:
   `sample = (x, y) + warp · amplitude`, then `base` is read back with
   `bilinearSample` at those fractional coordinates. This bends and folds the
   terrain so coastlines and ridgelines look organic rather than blobby. The
   warp amplitude is `0.08 · min(width, height)`, keyed to the shorter axis so
   the displacement (in pixels) is isotropic on non-square maps. Bilinear
   sampling keeps the field continuous, avoiding single-pixel cliffs at the
   shore.
3. **Radial falloff -> island.** If enabled, each pixel's height is reduced by
   `falloffStrength · r²` (then nudged up by a constant so the middle stays
   above water), where `r` is distance from center with each axis normalised by
   its own half-extent. That makes the falloff an ellipse inscribed in the
   rectangle. Edges sink underwater symmetrically on both axes, producing a
   centered island. Disable it for terrain that runs off all edges.
4. **Normalise.** The min and max are tracked during the warp pass (steps 2–3
   run in a single loop) and used to rescale the heightmap to exactly `[0, 1]`,
   so the sea-level threshold is meaningful.

### 5. Land/sea - `computeLandMask`

A flat `uint8_t` buffer where each cell is `1` if `height > seaLevel` (0.52),
else `0`. It's computed once and reused to avoid the heightmap getting
thresholded multiple times.

### 6. Rendering - `renderToPixels`

Produces the RGB pixel buffer.

**Hillshade** (`hillshade`) is computed once over the real heightmap (seabed
included) so relief shows under water as well as on land, and the shading stays
continuous across the coastline. It estimates the surface gradient with central
differences (one-sided at borders), exaggerated by `zFactor` (120), then
applies a Lambertian lighting model for a light at azimuth 315° / altitude 45°.
The lighting uses a closed-form of the standard slope/aspect formula:

```
shade = (sinAlt + cosAlt·(cosAz·gx − sinAz·gy)) / √(1 + gx² + gy²)
```

Each pixel is then colored:

- **Land** - a hypsometric tint from `hypsometricColor`. The elevation above
  sea level (renormalised to `[0, 1]`) is looked up on a color ramp that runs
  green lowlands -> yellow-green -> yellow -> brown -> red peaks
  (`interpolateRamp` over the `HYPS_*` tables). The color is then multiplied by
  `0.6 + 0.4·shade` so hillshading darkens slopes facing away from the light.
- **Sea** - a base blue `(46, 88, 133)` that darkens with depth. A `depth`
  value (0 at the shoreline, 1 at the deepest point) fades the color toward a
  darker blue (`1 − 0.6·depth`), and the same hillshade factor is applied so
  the underwater terrain relief is visible.

**Coastline.** A final pass paints black any land cell that has at least one
non-land neighbor (with per-axis wraparound at the edges), outlining every
shore.

### 7. Image output - `writeBmp`

A self-contained 24-bit BMP writer. It emits the 54-byte header, uses a
negative height so rows are stored top-to-bottom (matching the buffer), swaps
RGB->BGR (BMP's channel order), and pads each row to a 4-byte boundary.
