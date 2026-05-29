#include <chrono>
#include <limits>

#include "Random.h"

Random::Random(int seed)
{
	this->init(seed);
}

Random::Random()
{
	this->init();
}

void Random::init(int seed)
{
	this->generator = std::default_random_engine(seed);
	this->integerDistribution = std::uniform_int_distribution<int>(0, std::numeric_limits<int>::max());
	this->realDistribution = std::uniform_real_distribution<double>(0.0, 1.0);
}

void Random::init()
{
	const auto currentTime = std::chrono::high_resolution_clock::now();
	const int64_t microsecondsSinceEpoch = std::chrono::duration_cast<std::chrono::microseconds>(currentTime.time_since_epoch()).count();
	const int seed = static_cast<int>(microsecondsSinceEpoch % std::numeric_limits<int>::max());
	this->init(seed);
}

int Random::next()
{
	return this->integerDistribution(this->generator);
}

int Random::next(int exclusiveMax)
{
	return this->next() % exclusiveMax;
}

bool Random::nextBool()
{
	return (this->next() % 2) == 0;
}

double Random::nextReal()
{
	return this->realDistribution(this->generator);
}

// ArenaRandom

ArenaRandom::ArenaRandom(uint32_t seed)
{
	state1 = static_cast<uint16_t>(seed | 1);
	state2 = static_cast<uint16_t>(seed >> 16);
}

ArenaRandom::ArenaRandom()
	: ArenaRandom(ArenaRandom::DEFAULT_SEED) { }

uint32_t ArenaRandom::getSeed() const
{
	return
		static_cast<uint32_t>(state1) | (static_cast<uint32_t>(state2) << 16);
}

uint16_t ArenaRandom::next()
{
	const uint32_t mul = static_cast<uint32_t>(state1) * 0x2D;

	state2 = static_cast<uint16_t>(state2 * 0x2D + state1 * 0x6D + (mul >> 16));

	state1 = static_cast<uint16_t>(mul);

	return state2;
}

void ArenaRandom::srand(uint32_t seed)
{
	state1 = static_cast<uint16_t>(seed | 1);
	state2 = static_cast<uint16_t>(seed >> 16);
}
