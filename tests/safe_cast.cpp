/* Copyright 2009-2016 Francesco Biscani (bluescarni@gmail.com)

This file is part of the Piranha library.

The Piranha library is free software; you can redistribute it and/or modify
it under the terms of either:

  * the GNU Lesser General Public License as published by the Free
    Software Foundation; either version 3 of the License, or (at your
    option) any later version.

or

  * the GNU General Public License as published by the Free Software
    Foundation; either version 3 of the License, or (at your option) any
    later version.

or both in parallel, as here.

The Piranha library is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received copies of the GNU General Public License and the
GNU Lesser General Public License along with the Piranha library.  If not,
see https://www.gnu.org/licenses/. */

#include "../src/safe_cast.hpp"

#define BOOST_TEST_MODULE safe_cast_test
#include <boost/test/unit_test.hpp>

#include <boost/fusion/sequence.hpp>
#include <climits>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "../src/config.hpp"
#include "../src/init.hpp"

struct foo {
};

// Struct without copy ctor.
struct foo_nc {
    foo_nc(const foo_nc &) = delete;
};

struct conv1 {
};
struct conv2 {
};
struct conv3 {
};
struct conv4 {
};

namespace piranha
{

// Good specialisation.
template <>
struct safe_cast_impl<conv2, conv1> {
    conv2 operator()(const conv1 &) const;
};

// Bad specialisation.
template <>
struct safe_cast_impl<conv3, conv1> {
    int operator()(const conv1 &) const;
};

// Bad specialisation.
template <>
struct safe_cast_impl<conv4, conv1, void> {
    conv4 operator()(conv1 &) const;
};
}

using namespace piranha;

BOOST_AUTO_TEST_CASE(safe_cast_main_test)
{
    init();
    BOOST_CHECK((has_safe_cast<int, int>::value));
    BOOST_CHECK((has_safe_cast<int, int &>::value));
    BOOST_CHECK((has_safe_cast<const int, int &>::value));
    BOOST_CHECK((has_safe_cast<const int &, int &&>::value));
    BOOST_CHECK((has_safe_cast<int &, int &&>::value));
    BOOST_CHECK((!has_safe_cast<int &, void>::value));
    BOOST_CHECK((!has_safe_cast<void, int &&>::value));
    BOOST_CHECK((!has_safe_cast<void, void>::value));

    BOOST_CHECK((has_safe_cast<int, char>::value));
    BOOST_CHECK((has_safe_cast<char, unsigned long long>::value));
    BOOST_CHECK((!has_safe_cast<int, std::string>::value));
    BOOST_CHECK((has_safe_cast<std::string, std::string>::value));
    BOOST_CHECK((has_safe_cast<foo, foo>::value));
    BOOST_CHECK((!has_safe_cast<foo_nc, foo_nc>::value));
    BOOST_CHECK((!has_safe_cast<double, int>::value));
    // Check with custom specialisations.
    BOOST_CHECK((has_safe_cast<conv2, conv1>::value));
    BOOST_CHECK((!has_safe_cast<conv3, conv1>::value));
    BOOST_CHECK((!has_safe_cast<conv4, conv1>::value));
    // Some simple tests.
    BOOST_CHECK_EQUAL(safe_cast<int>(4l), 4);
    BOOST_CHECK_EQUAL(safe_cast<unsigned>(4l), 4u);
    BOOST_CHECK_EQUAL(safe_cast<short>(-4ll), -4);
    // Out of bounds.
    BOOST_CHECK_THROW(safe_cast<unsigned>(-1), safe_cast_failure);
    if (CHAR_BIT == 8) {
        BOOST_CHECK_THROW(safe_cast<unsigned char>(300), safe_cast_failure);
        BOOST_CHECK_THROW(safe_cast<unsigned char>(300ull), safe_cast_failure);
    }
}

#if 0
struct safe_cast_int_float_tester {
    template <typename T>
    void operator()(const T &)
    {
        typedef mp_integer<T::value> int_type;
        // Check casts between integral types.
        BOOST_CHECK((has_safe_cast<int_type, int>::value));
        BOOST_CHECK((has_safe_cast<int_type, unsigned long>::value));
        BOOST_CHECK((has_safe_cast<char, int_type>::value));
        BOOST_CHECK((has_safe_cast<long long, int_type>::value));
        BOOST_CHECK_EQUAL(safe_cast<int_type>(3u), 3u);
        BOOST_CHECK_EQUAL(safe_cast<int_type>(-3l), -3);
        BOOST_CHECK_EQUAL(safe_cast<unsigned>(int_type(3)), 3u);
        BOOST_CHECK_EQUAL(safe_cast<short>(int_type(-3)), -3);
        BOOST_CHECK_THROW(safe_cast<int>(int_type{std::numeric_limits<int>::max()} + 1), std::invalid_argument);
        BOOST_CHECK_THROW(safe_cast<int>(int_type{std::numeric_limits<int>::min()} - 1), std::invalid_argument);
        BOOST_CHECK_THROW(safe_cast<unsigned>(int_type{std::numeric_limits<unsigned>::max()} + 1),
                          std::invalid_argument);
        BOOST_CHECK_THROW(safe_cast<unsigned>(int_type{-1}), std::invalid_argument);
        // Float to mp_integer.
        BOOST_CHECK((has_safe_cast<int_type, float>::value));
        BOOST_CHECK((has_safe_cast<int_type, double>::value));
        BOOST_CHECK((!has_safe_cast<float, int_type>::value));
        BOOST_CHECK((!has_safe_cast<double, int_type>::value));
        BOOST_CHECK_EQUAL(safe_cast<int_type>(3.), 3);
        BOOST_CHECK_EQUAL(safe_cast<int_type>(-3.f), -3);
        BOOST_CHECK_THROW(safe_cast<int_type>(1. / std::numeric_limits<double>::radix), std::invalid_argument);
        BOOST_CHECK_THROW(safe_cast<int_type>(1.f / std::numeric_limits<float>::radix), std::invalid_argument);
        BOOST_CHECK_THROW(
            safe_cast<int_type>((1. + std::numeric_limits<double>::radix) / std::numeric_limits<double>::radix),
            std::invalid_argument);
        BOOST_CHECK_THROW(safe_cast<int_type>(-1. / std::numeric_limits<double>::radix), std::invalid_argument);
        BOOST_CHECK_THROW(safe_cast<int_type>(-1.f / std::numeric_limits<float>::radix), std::invalid_argument);
        BOOST_CHECK_THROW(
            safe_cast<int_type>(-(1. + std::numeric_limits<double>::radix) / std::numeric_limits<double>::radix),
            std::invalid_argument);
        if (std::numeric_limits<double>::has_infinity && std::numeric_limits<double>::has_quiet_NaN) {
            BOOST_CHECK_THROW(safe_cast<int_type>(std::numeric_limits<double>::infinity()), std::invalid_argument);
            BOOST_CHECK_THROW(safe_cast<int_type>(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
        }
    }
};
#endif

BOOST_AUTO_TEST_CASE(safe_cast_int_float_test)
{
    // boost::mpl::for_each<size_types>(safe_cast_int_float_tester());
    // Casts from floating point to C++ ints.
    safe_cast<int>(std::numeric_limits<double>::quiet_NaN());



    BOOST_CHECK((has_safe_cast<int, double>::value));
    BOOST_CHECK((has_safe_cast<char, float>::value));
    BOOST_CHECK((!has_safe_cast<double, int>::value));
    BOOST_CHECK((!has_safe_cast<float, char>::value));
    BOOST_CHECK_EQUAL(safe_cast<int>(2.), 2);
    BOOST_CHECK_EQUAL(safe_cast<int>(-2.), -2);
    BOOST_CHECK_THROW(safe_cast<int>(1. / std::numeric_limits<double>::radix), std::invalid_argument);
    BOOST_CHECK_THROW(safe_cast<int>(1.f / std::numeric_limits<float>::radix), std::invalid_argument);
    BOOST_CHECK_THROW(safe_cast<int>((1. + std::numeric_limits<double>::radix) / std::numeric_limits<double>::radix),
                      std::invalid_argument);
    BOOST_CHECK_THROW(safe_cast<int>(-1. / std::numeric_limits<double>::radix), std::invalid_argument);
    BOOST_CHECK_THROW(safe_cast<int>(-1.f / std::numeric_limits<float>::radix), std::invalid_argument);
    BOOST_CHECK_THROW(safe_cast<int>(-(1. + std::numeric_limits<double>::radix) / std::numeric_limits<double>::radix),
                      std::invalid_argument);
    if (std::numeric_limits<double>::has_infinity && std::numeric_limits<double>::has_quiet_NaN) {
        BOOST_CHECK_THROW(safe_cast<int>(std::numeric_limits<double>::infinity()), std::invalid_argument);
        BOOST_CHECK_THROW(safe_cast<long>(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    }
}
