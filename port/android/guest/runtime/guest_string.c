/*
GUEST_STRING.C

memcmp, eight bytes at a time. musl's (src/string/memcmp.c) compares one
byte a step, and the game and the platform layer compare a lot: state
shadows, shader keys and tag names (the Switch's profile put 4% of the
game thread's time in it). AArch64 loads unaligned words at full speed.
The result is musl's, the difference of the first bytes that differ.
*/

#include <stddef.h>
#include <stdint.h>

static uint64_t load64(const unsigned char *bytes)
{
	uint64_t value;

	__builtin_memcpy(&value, bytes, sizeof(value));
	return value;
}

static int byte_difference(uint64_t a, uint64_t b)
{
	/* (little-endian: the first byte that differs is the lowest) */
	int shift = __builtin_ctzll(a ^ b) & ~7;

	return (int)((a >> shift) & 0xff) - (int)((b >> shift) & 0xff);
}

int memcmp(const void *first, const void *second, size_t size)
{
	const unsigned char *a = first, *b = second;

	while (size >= 8)
	{
		uint64_t x = load64(a), y = load64(b);

		if (x != y)
			return byte_difference(x, y);
		a += 8;
		b += 8;
		size -= 8;
	}
	for (; size; size--, a++, b++)
	{
		if (*a != *b)
			return *a - *b;
	}
	return 0;
}
