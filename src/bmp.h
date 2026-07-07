#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

/**
 * Write a 24-bit BMP file to disk. The input RGB buffer is assumed to be in
 * row-major order, 3 bytes per pixel, top-to-bottom.
 * @param path Output file path
 * @param rgb Input pixel buffer
 * @param width Image width in pixels
 * @param height Image height in pixels
 */
void writeBmp(const std::string& path, const std::vector<uint8_t>& rgb, int width, int height) {
	int rowStride = (width * 3 + 3) & ~3; // Pad each row up to 4 bytes
	uint32_t pixelBytes = static_cast<uint32_t>(rowStride) * height;
	const int headerSize = 54;
	uint32_t fileSize = headerSize + pixelBytes;
	uint32_t uw = static_cast<uint32_t>(width);
	uint32_t uh = static_cast<uint32_t>(-height); // Negative = top-down rows

	uint8_t header[headerSize] = {0};
	header[0] = 'B';
	header[1] = 'M';
	header[2] = fileSize;
	header[3] = fileSize >> 8;
	header[4] = fileSize >> 16;
	header[5] = fileSize >> 24;
	header[10] = headerSize; // Offset to pixel data
	header[14] = 40; // DIB header size
	header[18] = uw;
	header[19] = uw >> 8;
	header[20] = uw >> 16;
	header[21] = uw >> 24;
	header[22] = uh;
	header[23] = uh >> 8;
	header[24] = uh >> 16;
	header[25] = uh >> 24;
	header[26] = 1; // Planes
	header[28] = 24; // Bits per pixel

	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(header), headerSize);

	std::vector<uint8_t> row(rowStride, 0); // Trailing padding stays zero
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int src = (y * width + x) * 3;
			row[x * 3 + 0] = rgb[src + 2]; // B
			row[x * 3 + 1] = rgb[src + 1]; // G
			row[x * 3 + 2] = rgb[src + 0]; // R
		}
		out.write(reinterpret_cast<const char*>(row.data()), rowStride);
	}
}
