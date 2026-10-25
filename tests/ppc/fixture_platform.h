#pragma once
// The lifted fixture's platform (fixture_platform.cpp).
#include <cstdint>
#include <vector>

// What the fixture must print: the oracle's output.
void SetExpectedOutput(std::vector<uint8_t> expected);
