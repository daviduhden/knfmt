#include <benchmark/benchmark.h>

extern "C" {

#include "util.h"
}

/*
 * Google Benchmark harness for colwidth().  The input is deliberately dominated
 * by tabs and long runs of 'a' so the measurement covers tab expansion and
 * wrapping behavior rather than short-string setup.
 */
static void
BM_colwidth(benchmark::State &state)
{
	const char str[] = "\t\t"
			   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

	for (auto _ : state)
		colwidth(str, sizeof(str) - 1, 1);
}
BENCHMARK(BM_colwidth);

BENCHMARK_MAIN();
