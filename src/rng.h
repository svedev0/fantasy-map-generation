#pragma once

#include <cstdint>

/**
 * Minimal xorshift64 pseudo-random number generator.
 */
class Rng {
public:
	// Fractional bits of the golden ratio
	static constexpr uint64_t golden_ratio = 0x9E3779B97F4A7C15ull;

	/**
	 * Constructs a new Rng with the given seed. The seed is scrambled with the
	 * golden ratio to avoid low-entropy seeds and the all-zero fixed point of
	 * xorshift.
	 * @param seed The seed to initialise the PRNG state
	 * @return A new Rng instance
	 */
	explicit Rng(uint64_t seed) : state(seed ^ golden_ratio) {
		// xorshift has an all-zero fixed point, steer away from it
		if (state == 0) {
			state = golden_ratio;
		}
	}

	/**
	 * Generates a new 64-bit unsigned integer from the PRNG.
	 * @return A 64-bit unsigned integer
	 */
	uint64_t nextU64() {
		// 64-bit unsigned overflow wraps automatically
		uint64_t x = state;
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		state = x;
		return x;
	}

	/**
	 * Generates a uniform double in the range [0, 1). The top 53 bits feed a
	 * double's mantissa.
	 * @return A double in the range [0, 1)
	 */
	double nextDouble() {
		uint64_t next = nextU64();
		return (next >> 11) * (1.0 / (1ull << 53));
	}

	/**
	 * Generates a uniform double in the range [low, high).
	 * @param low The lower bound of the range
	 * @param high The upper bound of the range
	 * @return A double in the range [low, high)
	 */
	double uniform(double low, double high) {
		return low + (high - low) * nextDouble();
	}

private:
	uint64_t state;
};
