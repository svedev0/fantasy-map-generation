#define _USE_MATH_DEFINES

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <tuple>
#include <string>
#include <random>

#include "rng.h"
#include "bmp.h"

using namespace std;

/**
 * Row-major 2D grid backed by a single contiguous buffer. operator[] returns a
 * pointer to the start of a row, so callers keep the familiar grid[y][x]
 * syntax even though the storage stays flat.
 */
struct Grid {
	int width = 0;
	int height = 0;
	vector<double> data;

	Grid() = default;
	Grid(int rows, int cols, double value) :
		width(cols),
		height(rows),
		data(static_cast<size_t>(rows) * cols, value) {}

	double* operator[](int y) {
		return data.data() + static_cast<size_t>(y) * width;
	}

	const double* operator[](int y) const {
		return data.data() + static_cast<size_t>(y) * width;
	}
};

struct GenerationOptions {
	int width = 512; // Width of the heightmap in pixels
	int height = 512; // Height of the heightmap in pixels
	uint32_t seed = 7; // Random seed for terrain generation
	bool radialFalloff = true; // Whether to push edges underwater to make an island
	double falloffStrength = 1.4; // How aggressively to push the edges underwater
	double seaLevel = 0.52; // Sea level threshold `[0, 1]`
	int fbmOctaves = 6; // Number of fBm octaves
	int fbmBaseRes = 4; // Base resolution for fBm
	double fbmLacunarity = 2.0; // Frequency multiplier for fBm octaves
	double fbmGain = 0.5; // Amplitude multiplier for fBm octaves
};

// Elevation breakpoints and their target colors along the hypsometric ramp
const vector<double> HYPS_STOPS = {0.0, 0.35, 0.55, 0.78, 1.0};
const vector<double> HYPS_R = {56.0, 140.0, 214.0, 176.0, 150.0};
const vector<double> HYPS_G = {132.0, 170.0, 204.0, 120.0, 52.0};
const vector<double> HYPS_B = {68.0, 80.0, 120.0, 72.0, 42.0};

// Grid helpers ---------------------------------------------------------------

/**
 * Allocate a `height` * `width` grid of doubles, initialised to `value`.
 * @param height Number of rows
 * @param width Number of columns
 * @param value Initial value for each cell
 * @return Grid object with the requested dimensions and initial value
 */
Grid newGrid(int height, int width, double value) {
	return Grid(height, width, value);
}

// Noise ----------------------------------------------------------------------

/**
 * Fade function for Perlin noise. Smoothly interpolates between `0` and `1`.
 * @param t Fractional position in `[0, 1]`
 * @return Smoothed value in `[0, 1]`
 */
double perlinFade(double t) {
	// Perlin's quintic fade curve (smootherstep): 6t^5 - 15t^4 + 10t^3
	return t * t * t * (t * (t * 6 - 15) + 10);
}

/**
 * Add one octave of tileable-ish 2D Perlin noise, scaled by `amplitude`, into
 * `out`. Cell sizes are fractional, so `resY`/`resX` need not divide
 * `height`/`width` evenly.
 * @param out Output grid to accumulate noise into
 * @param amplitude Amplitude of this octave
 * @param height Number of rows in the grid
 * @param width Number of columns in the grid
 * @param resY Number of lattice cells along y
 * @param resX Number of lattice cells along x
 * @param rng Random number generator
 */
void addPerlinOctave(
	Grid& out,
	double amplitude,
	int height,
	int width,
	int resY,
	int resX,
	Rng& rng) {
	// Fractional cell sizes so the lattice need not divide the grid evenly
	double cellH = static_cast<double>(height) / resY;
	double cellW = static_cast<double>(width) / resX;

	// Random unit gradient vector at each lattice node, stored as (gx, gy)
	// pairs
	Grid gradX = newGrid(resY + 1, resX + 1, 0.0);
	Grid gradY = newGrid(resY + 1, resX + 1, 0.0);
	for (int nodeY = 0; nodeY <= resY; nodeY++) {
		for (int nodeX = 0; nodeX <= resX; nodeX++) {
			double angle = rng.uniform(0.0, 2.0 * M_PI);
			gradX[nodeY][nodeX] = cos(angle); // Unit vector x component
			gradY[nodeY][nodeX] = sin(angle); // Unit vector y component
		}
	}

	double root2 = sqrt(2.0);

	// Per-column terms depend only on x, so compute them once up front instead
	// of recomputing (divide + fade) for every row. The min() guards against a
	// floating-point boundary landing exactly on resX.
	vector<int> cellXs(width);
	vector<double> fracXs(width);
	vector<double> weightXs(width);
	for (int x = 0; x < width; x++) {
		int cellX = min(static_cast<int>(x / cellW), resX - 1);
		double fracX = x / cellW - cellX;
		cellXs[x] = cellX;
		fracXs[x] = fracX;
		weightXs[x] = perlinFade(fracX);
	}

	for (int y = 0; y < height; y++) {
		int cellY = min(
			static_cast<int>(y / cellH),
			resY - 1); // Lattice cell index along y
		double fracY = y / cellH - cellY; // Fractional position in cell, [0, 1)
		double weightY = perlinFade(fracY); // Smoothed blend weight along y

		for (int x = 0; x < width; x++) {
			int cellX = cellXs[x];
			double fracX = fracXs[x];
			double weightX = weightXs[x];

			// Dot each of the four surrounding corner gradients with the
			// offset from that corner to the pixel
			double dot00 = (
				fracY * gradX[cellY][cellX] +
				fracX * gradY[cellY][cellX]
			);
			double dot10 = (
				(fracY - 1) * gradX[cellY + 1][cellX] +
				fracX * gradY[cellY + 1][cellX]
			);
			double dot01 = (
				fracY * gradX[cellY][cellX + 1] +
				(fracX - 1) * gradY[cellY][cellX + 1]
			);
			double dot11 = (
				(fracY - 1) * gradX[cellY + 1][cellX + 1] +
				(fracX - 1) * gradY[cellY + 1][cellX + 1]
			);

			// Interpolate along y, then x
			double interp0 = dot00 * (1 - weightY) + dot10 * weightY;
			double interp1 = dot01 * (1 - weightY) + dot11 * weightY;

			// sqrt(2.0) rescales the result back to roughly [-1, 1]
			double value = root2 * (interp0 * (1 - weightX) + interp1 * weightX);
			out[y][x] += amplitude * value;
		}
	}
}

/**
 * Fractional Brownian motion (fBm) noise. Combines multiple octaves of Perlin
 * noise.
 * @param height Number of rows in the output grid
 * @param width Number of columns in the output grid
 * @param rng Random number generator
 * @param octaves Number of Perlin noise octaves to combine
 * @param baseRes Base resolution of the first octave (number of lattice cells)
 * @param lacunarity Frequency multiplier for each successive octave
 * @param gain Amplitude multiplier for each successive octave
 * @return Grid of unnormalised fBm noise, roughly `[-2, 2]` with default gain
 */
Grid fbm(
	int height,
	int width,
	Rng& rng,
	int octaves,
	int baseRes,
	double lacunarity,
	double gain) {
	Grid noise = newGrid(height, width, 0.0);
	double amplitude = 1.0;
	int resolution = baseRes;

	for (int octave = 0; octave < octaves; octave++) {
		addPerlinOctave(noise, amplitude, height, width, resolution, resolution, rng);
		amplitude *= gain;
		resolution = static_cast<int>(resolution * lacunarity);
	}

	return noise;
}

// Terrain --------------------------------------------------------------------

/**
 * Sample a field at fractional coords with bilinear interpolation. Keeps the
 * field continuous so warped coastlines don't gain single-pixel cliffs across
 * the sea threshold.
 * @param field 2D grid of values to sample
 * @param sampleY Fractional y coordinate to sample
 * @param sampleX Fractional x coordinate to sample
 * @return Interpolated value at the sample coordinates
 */
double bilinearSample(const Grid& field, double sampleY, double sampleX) {
	int yFloor = static_cast<int>(floor(sampleY));
	int xFloor = static_cast<int>(floor(sampleX));
	int yCeil = yFloor + 1;
	int xCeil = xFloor + 1;
	if (yCeil > field.height - 1) {
		yCeil = field.height - 1;
	}
	if (xCeil > field.width - 1) {
		xCeil = field.width - 1;
	}

	double fracY = sampleY - yFloor;
	double fracX = sampleX - xFloor;

	return (
		field[yFloor][xFloor] * (1 - fracY) * (1 - fracX) +
		field[yFloor][xCeil] * (1 - fracY) * fracX +
		field[yCeil][xFloor] * fracY * (1 - fracX) +
		field[yCeil][xCeil] * fracY * fracX
	);
}

/**
 * Generate a heightmap with fBm Perlin noise, domain warp, and radial falloff.
 * @param opts Generation options
 * @return Generated heightmap
 */
Grid generateHeightmap(const GenerationOptions& opts) {
	Rng rng(opts.seed);
	int width = opts.width;
	int height = opts.height;

	// Base terrain: multi-octave fBm noise
	Grid base = fbm(height, width, rng, opts.fbmOctaves, opts.fbmBaseRes, opts.fbmLacunarity, opts.fbmGain);

	// Domain warp offset fields. Amplitude keyed to the shorter axis so the
	// warp displacement (in pixels) stays isotropic on non-square maps.
	double warpAmplitude = 0.08 * min(width, height);
	Grid warpX = fbm(height, width, rng, 4, opts.fbmBaseRes, opts.fbmLacunarity, opts.fbmGain);
	Grid warpY = fbm(height, width, rng, 4, opts.fbmBaseRes, opts.fbmLacunarity, opts.fbmGain);

	// Resample the base through the warp offsets and apply the radial falloff
	// in a single pass, tracking the min/max needed for the normalisation
	// below. Each axis is normalised by its own half-extent, so the falloff is
	// an ellipse inscribed in the rectangle.
	double centerX = width / 2.0;
	double centerY = height / 2.0;
	Grid heightmap = newGrid(height, width, 0.0);
	double hmin = numeric_limits<double>::infinity();
	double hmax = -numeric_limits<double>::infinity();
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			double sampleY = y + warpY[y][x] * warpAmplitude;
			double sampleX = x + warpX[y][x] * warpAmplitude;
			// Clamp sample coordinates into the grid
			if (sampleY < 0) {
				sampleY = 0.0;
			}
			if (sampleY > height - 1) {
				sampleY = static_cast<double>(height - 1);
			}
			if (sampleX < 0) {
				sampleX = 0.0;
			}
			if (sampleX > width - 1) {
				sampleX = static_cast<double>(width - 1);
			}

			double elev = bilinearSample(base, sampleY, sampleX);

			// Radial falloff -> ocean at edges
			if (opts.radialFalloff) {
				double nodeY = (y - centerY) / centerY;
				double nodeX = (x - centerX) / centerX;
				double radius = hypot(nodeY, nodeX);
				elev -= opts.falloffStrength * radius * radius;
				elev += 0.35; // Adjust to keep some land
			}

			heightmap[y][x] = elev;
			if (elev < hmin) {
				hmin = elev;
			}
			if (elev > hmax) {
				hmax = elev;
			}
		}
	}

	// Normalise to [0, 1] so seaLevel thresholding is meaningful
	double span = hmax - hmin;
	if (span == 0) {
		span = 1.0; // Avoid division by zero
	}

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			heightmap[y][x] = (heightmap[y][x] - hmin) / span;
		}
	}

	return heightmap;
}

// Render ---------------------------------------------------------------------

/**
 * Piecewise-linear lookup. Interpolate a scalar over a ramp defined by
 * ascending `stops` and matching `values`. Clamps outside the ends.
 * @param stops Ascending breakpoints in `[0, 1]`
 * @param values Values at each breakpoint
 * @param elev Input value to interpolate, in `[0, 1]`
 * @return Interpolated value
 */
double interpolateRamp(
	const vector<double>& stops, const vector<double>& values, double elev) {
	if (elev <= stops.front()) {
		return values.front();
	}
	if (elev >= stops.back()) {
		return values.back();
	}
	for (size_t i = 1; i < stops.size(); i++) {
		if (elev <= stops[i]) {
			double span = stops[i] - stops[i - 1];
			double local = (elev - stops[i - 1]) / span;
			return values[i - 1] + local * (values[i] - values[i - 1]);
		}
	}
	return values.back(); // Should not reach here
}

/**
 * Standard elevation relief tint (green lowlands -> red peaks).
 * @param elev Normalised land elevation in `[0, 1]`
 * @return Tuple of (R, G, B) color values in `[0, 255]`
 */
tuple<double, double, double> hypsometricColor(double elev) {
	double r = interpolateRamp(HYPS_STOPS, HYPS_R, elev);
	double g = interpolateRamp(HYPS_STOPS, HYPS_G, elev);
	double b = interpolateRamp(HYPS_STOPS, HYPS_B, elev);
	return make_tuple(r, g, b);
}

/**
 * Grayscale hillshade for 3D relief. Gradients use central differences
 * interior and one-sided at the borders.
 * @param surface 2D grid of elevation values
 * @param azimuth Compass direction of the light source in degrees
 * @param altitude Angle of the light source above the horizon in degrees
 * @param zFactor Vertical exaggeration factor
 * @return Grid of hillshade values in `[0, 1]`
 */
Grid hillshade(
	const Grid& surface, double azimuth, double altitude, double zFactor) {
	int width = surface.width;
	int height = surface.height;

	double azRad = M_PI * (360.0 - azimuth + 90.0) / 180.0;
	double altRad = M_PI * altitude / 180.0;
	double sinAlt = sin(altRad);
	double cosAlt = cos(altRad);
	double cosAz = cos(azRad);
	double sinAz = sin(azRad);

	Grid shade = newGrid(height, width, 0.0);

	for (int y = 0; y < height; y++) {
		// One-sided difference at the top/bottom edges, central otherwise
		int yLow = max(y - 1, 0);
		int yHigh = min(y + 1, height - 1);
		int yDiv = yHigh - yLow;

		for (int x = 0; x < width; x++) {
			int xLow = max(x - 1, 0);
			int xHigh = min(x + 1, width - 1);
			int xDiv = xHigh - xLow;

			// Height gradient with vertical exaggeration folded in
			double gy = (surface[yHigh][x] - surface[yLow][x]) * zFactor / yDiv;
			double gx = (surface[y][xHigh] - surface[y][xLow]) * zFactor / xDiv;

			// Closed form of the Lambert term. Algebraically identical to:
			//   sinAlt*cos(slope) + cosAlt*sin(slope)*cos(azRad - aspect)
			// with slope = atan(hypot(gx, gy)) and aspect = atan2(-gy, gx),
			// but with the trig eliminated: cos(slope) = 1/sqrt(1+gx^2+gy^2),
			// sin(slope)*cos(azRad - aspect) = (cosAz*gx - sinAz*gy)/sqrt(...).
			double denom = sqrt(1.0 + gx * gx + gy * gy);
			double value = (sinAlt + cosAlt * (cosAz * gx - sinAz * gy)) / denom;
			double clamped = clamp(value, 0.0, 1.0);
			shade[y][x] = clamped;
		}
	}

	return shade;
}

/**
 * Land/sea mask (`1` = land) as a flat row-major buffer. Computed once by the
 * caller and shared, so the heightmap is thresholded only a single time.
 * @param heightmap 2D grid of normalised elevation values
 * @param seaLevel Threshold for land/sea separation
 * @return Flat vector of land/sea mask values in row-major order
 */
vector<uint8_t> computeLandMask(const Grid& heightmap, double seaLevel) {
	int width = heightmap.width;
	int height = heightmap.height;

	vector<uint8_t> land(static_cast<size_t>(width) * height, 0);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			size_t idx = static_cast<size_t>(y) * width + x;
			land[idx] = heightmap[y][x] > seaLevel;
		}
	}
	return land;
}

/**
 * Render a heightmap to an RGB pixel buffer.
 * @param heightmap 2D grid of normalised elevation values
 * @param land Flat vector of land/sea mask values in row-major order
 * @param seaLevel Threshold for land/sea separation
 * @return Flat vector of RGB pixel values in row-major order
 */
vector<uint8_t> renderToPixels(
	const Grid& heightmap, const vector<uint8_t>& land, double seaLevel) {
	int width = heightmap.width;
	int height = heightmap.height;

	// Hillshade the true heightmap (seabed included) so relief shows under
	// water as well as on land. One pass over the real surface also keeps the
	// water continuous across the coastline instead of flattening the sea to a
	// plane.
	Grid hshade = hillshade(heightmap, 315.0, 45.0, 120.0);

	// generateHeightmap normalises the heightmap to [0, 1], so the land
	// maximum is exactly 1.0 and the deepest water is exactly 0.0.
	double landSpan = 1.0 - seaLevel;
	if (landSpan == 0) {
		landSpan = 1.0; // Avoid division by zero
	}
	// Depth normaliser for water
	double seaSpan = seaLevel > 0.0 ? seaLevel : 1.0;

	// Pixel buffer, RGB, 3 bytes per pixel
	vector<uint8_t> buffer(static_cast<size_t>(width) * height * 3, 0);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			double r;
			double g;
			double b;
			if (land[static_cast<size_t>(y) * width + x]) {
				// Land: hypsometric tint, darkened by hillshade for 3D relief
				double elev = (heightmap[y][x] - seaLevel) / landSpan;
				double clamped = clamp(elev, 0.0, 1.0);
				tie(r, g, b) = hypsometricColor(clamped);
				double shade = 0.6 + 0.4 * hshade[y][x];
				r *= shade;
				g *= shade;
				b *= shade;
			} else {
				// Sea: base blue faded toward a darker blue with depth, then
				// shaded by the seabed hillshade to reveal underwater relief
				double depth = clamp((seaLevel - heightmap[y][x]) / seaSpan, 0.0, 1.0);
				double depthShade = 1.0 - 0.6 * depth;
				double shade = 0.6 + 0.4 * hshade[y][x];
				r = 46.0 * depthShade * shade;
				g = 88.0 * depthShade * shade;
				b = 133.0 * depthShade * shade;
			}

			size_t offset = (static_cast<size_t>(y) * width + x) * 3;
			buffer[offset] = static_cast<uint8_t>(r) & 0xFF;
			buffer[offset + 1] = static_cast<uint8_t>(g) & 0xFF;
			buffer[offset + 2] = static_cast<uint8_t>(b) & 0xFF;
		}
	}

	// Coastline: land cells with at least one non-land neighbor (wrapping per
	// axis at the edges). Paint them black.
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			if (!land[static_cast<size_t>(y) * width + x]) {
				continue;
			}

			bool up = land[static_cast<size_t>((y - 1 + height) % height) * width + x];
			bool down = land[static_cast<size_t>((y + 1) % height) * width + x];
			bool left = land[static_cast<size_t>(y) * width + (x - 1 + width) % width];
			bool right = land[static_cast<size_t>(y) * width + (x + 1) % width];
			if (!(up && down && left && right)) {
				size_t offset = (static_cast<size_t>(y) * width + x) * 3;
				buffer[offset] = 0;
				buffer[offset + 1] = 0;
				buffer[offset + 2] = 0;
			}
		}
	}

	return buffer;
}

// CLI ------------------------------------------------------------------------

void usage(const char* prog) {
	printf(
		"Usage: %s [options]\n"
		"  -w,  --width <int>          Heightmap width (default 512)\n"
		"  -h,  --height <int>         Heightmap height (default 512)\n"
		"  -s,  --seed <int>           Random seed (default 7, 0 for random)\n"
		"  -f,  --falloff <double>     Radial falloff (default 1.4, 0 to disable)\n"
		"  -sl, --sea-level <double>   Sea level [0,1] (default 0.52)\n"
		"  -o,  --octaves <int>        fBm octaves (default 6)\n"
		"  -b,  --base-res <int>       fBm base resolution (default 4)\n"
		"  -l,  --lacunarity <double>  fBm lacunarity (default 2.0)\n"
		"  -g,  --gain <double>        fBm gain (default 0.5)\n",
		prog);
	exit(1);
}

long parseInt(string_view str, const char* prog) {
	char buf[32];
	if (str.empty() || str.size() >= sizeof(buf)) {
		usage(prog);
	}
	memcpy(buf, str.data(), str.size());
	buf[str.size()] = '\0';

	char* end = nullptr;
	long out = strtol(buf, &end, 10);
	if (end != buf + str.size()) {
		usage(prog);
	}
	return out;
}

double parseDouble(string_view str, const char* prog) {
	char buf[64];
	if (str.empty() || str.size() >= sizeof(buf)) {
		usage(prog);
	}
	memcpy(buf, str.data(), str.size());
	buf[str.size()] = '\0';

	char* end = nullptr;
	double out = strtod(buf, &end);
	if (end != buf + str.size()) {
		usage(prog);
	}
	return out;
}

GenerationOptions parseArgs(int argc, char** argv) {
	GenerationOptions opts;
	vector<string_view> args(argv + 1, argv + argc);

	for (size_t i = 0; i < args.size(); i++) {
		string_view arg = args[i];
		if (arg == "--help") {
			usage(argv[0]);
		}
		if (++i >= args.size()) {
			usage(argv[0]);
		}

		string_view val = args[i];
		if (arg == "-w" || arg == "--width") {
			opts.width = (int)parseInt(val, argv[0]);
		} else if (arg == "-h" || arg == "--height") {
			opts.height = (int)parseInt(val, argv[0]);
		} else if (arg == "-s" || arg == "--seed") {
			opts.seed = (uint32_t)parseInt(val, argv[0]);
		} else if (arg == "-f" || arg == "--falloff") {
			opts.falloffStrength = parseDouble(val, argv[0]);
			opts.radialFalloff = opts.falloffStrength != 0.0;
		} else if (arg == "-sl" || arg == "--sea-level") {
			opts.seaLevel = parseDouble(val, argv[0]);
		} else if (arg == "-o" || arg == "--octaves") {
			opts.fbmOctaves = (int)parseInt(val, argv[0]);
		} else if (arg == "-b" || arg == "--base-res") {
			opts.fbmBaseRes = (int)parseInt(val, argv[0]);
		} else if (arg == "-l" || arg == "--lacunarity") {
			opts.fbmLacunarity = parseDouble(val, argv[0]);
		} else if (arg == "-g" || arg == "--gain") {
			opts.fbmGain = parseDouble(val, argv[0]);
		} else {
			printf("Unknown arg: %.*s\n", (int)arg.size(), arg.data());
			usage(argv[0]);
		}
	}

	if (opts.width <= 0 || opts.height <= 0) {
		usage(argv[0]);
	}
	if (opts.seaLevel < 0.0 || opts.seaLevel > 1.0) {
		usage(argv[0]);
	}
	if (opts.fbmOctaves < 1 || opts.fbmBaseRes < 1) {
		usage(argv[0]);
	}

	if (opts.seed == 0) {
		mt19937 gen(random_device{}());
		uniform_int_distribution<uint32_t> dist;
		uint32_t randomSeed = dist(gen);
		opts.seed = randomSeed;
	}

	return opts;
}

// Main -----------------------------------------------------------------------

int main(int argc, char** argv) {
	GenerationOptions opts = parseArgs(argc, argv);

	string dimensionsStr = to_string(opts.width) + "x" + to_string(opts.height);
	string seedStr = to_string(opts.seed);
	string outputPath = "maps\\map_" + dimensionsStr + "_" + seedStr + ".bmp";

	// Generate terrain
	Grid heightmap = generateHeightmap(opts);

	// Compute land/sea mask
	vector<uint8_t> land = computeLandMask(heightmap, opts.seaLevel);

	// Calculate land cover fraction
	int landCount = 0;
	for (uint8_t cell : land) {
		landCount += cell;
	}
	size_t totalCells = static_cast<size_t>(opts.width) * opts.height;
	double landFraction = static_cast<double>(landCount) / totalCells;

	// Render to pixel buffer and write to BMP
	vector<uint8_t> buffer = renderToPixels(heightmap, land, opts.seaLevel);
	writeBmp(outputPath, buffer, opts.width, opts.height);

	printf("Seed: %u\n", opts.seed);
	printf("Land: %.2f%%\n", landFraction * 100);
	return 0;
}
