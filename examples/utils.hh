#pragma once
#include <charconv>
#include <random>
#include <string>
#include <system_error>
#include <getopt.h>

// - perform std::from_chars on `s`; all of `s` must be parsed
template <typename T>
inline std::errc from_str_chars(const std::string& s, T& value, int base = 10) {
    auto [next, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (next != s.data() + s.size()) {
        ec = std::errc::invalid_argument;
    }
    return ec;
}

// - perform std::from_chars on `s`; all of `s` must be parsed. return parsed
//   value, or throw exception if parsing fails
template <typename T>
inline T from_str_chars(const std::string& s, int base = 10) {
    T value;
    auto ec = from_str_chars(s, value, base);
    if (ec != std::errc()) {
        throw std::invalid_argument(std::make_error_code(ec).message());
    }
    return value;
}


// - construct a randomly-seeded generator
template <typename RNG>
inline RNG randomly_seeded() {
    std::random_device device;
    std::array<unsigned, RNG::state_size> seed_data;
    for (unsigned i = 0; i != RNG::state_size; ++i) {
        seed_data[i] = device();
    }
    std::seed_seq seq(seed_data.begin(), seed_data.end());
    return RNG(seq);
}


// - compute a `short_options` string corresponding to `longopts`
std::string short_options_for(const struct option* longopts);
