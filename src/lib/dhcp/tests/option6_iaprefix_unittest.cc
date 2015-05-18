// Copyright (C) 2011-2015 Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <config.h>

#include <dhcp/dhcp6.h>
#include <dhcp/option.h>
#include <dhcp/option_int.h>
#include <dhcp/option6_iaprefix.h>
#include <util/buffer.h>

#include <boost/scoped_ptr.hpp>
#include <gtest/gtest.h>

#include <iostream>
#include <sstream>

#include <arpa/inet.h>

using namespace std;
using namespace isc;
using namespace isc::dhcp;
using namespace isc::util;
using namespace isc::asiolink;

namespace {
class Option6IAPrefixTest : public ::testing::Test {
public:
    Option6IAPrefixTest() : buf_(255), out_buf_(255) {
        for (int i = 0; i < 255; i++) {
            buf_[i] = 255 - i;
        }
    }

    /// @brief creates on-wire representation of IAPREFIX option
    ///
    /// buf_ field is set up to have IAPREFIX with preferred=1000,
    /// valid=3000000000 and prefix being 2001:db8:1:0:afaf:0:dead:beef/77
    void setExampleBuffer() {
        for (int i = 0; i < 255; i++) {
            buf_[i] = 0;
        }

        buf_[ 0] = 0x00;
        buf_[ 1] = 0x00;
        buf_[ 2] = 0x03;
        buf_[ 3] = 0xe8; // preferred lifetime = 1000

        buf_[ 4]  = 0xb2;
        buf_[ 5] = 0xd0;
        buf_[ 6] = 0x5e;
        buf_[ 7] = 0x00; // valid lifetime = 3,000,000,000

        buf_[ 8] = 77; // Prefix length = 77

        buf_[ 9] = 0x20;
        buf_[10] = 0x01;
        buf_[11] = 0x0d;
        buf_[12] = 0xb8;
        buf_[13] = 0x00;
        buf_[14] = 0x01;
        buf_[17] = 0xaf;
        buf_[18] = 0xaf;
        buf_[21] = 0xde;
        buf_[22] = 0xad;
        buf_[23] = 0xbe;
        buf_[24] = 0xef; // 2001:db8:1:0:afaf:0:dead:beef
    }


    /// @brief Checks whether specified IAPREFIX option meets expected values
    ///
    /// To be used with option generated by setExampleBuffer
    ///
    /// @param opt IAPREFIX option being tested
    /// @param expected_type expected option type
    /// @param expected_length Expected length of the prefix.
    /// @param expected_address Expected prefix value.
    void checkOption(Option6IAPrefix& opt, const uint16_t expected_type,
                     const uint8_t expected_length,
                     const IOAddress& expected_address) {

        // Check if all fields have expected values
        EXPECT_EQ(Option::V6, opt.getUniverse());
        EXPECT_EQ(expected_type, opt.getType());
        EXPECT_EQ(expected_address, opt.getAddress());
        EXPECT_EQ(1000, opt.getPreferred());
        EXPECT_EQ(3000000000U, opt.getValid());
        // uint8_t is often represented as a character type (char). Convert it
        // to integer so as it is logged as a numeric value instead.
        EXPECT_EQ(static_cast<int>(expected_length),
                  static_cast<int>(opt.getLength()));

        // 4 bytes header + 25 bytes content
        EXPECT_EQ(Option::OPTION6_HDR_LEN + Option6IAPrefix::OPTION6_IAPREFIX_LEN,
                  opt.len());
    }

    /// @brief Checks whether content of output buffer is correct
    ///
    /// Output buffer is expected to be filled with an option matchin
    /// buf_ content as defined in setExampleBuffer().
    ///
    /// @param expected_type expected option type
    void checkOutputBuffer(uint16_t expected_type) {
        // Check if pack worked properly:
        const uint8_t* out = static_cast<const uint8_t*>(out_buf_.getData());

        // - if option type is correct
        EXPECT_EQ(expected_type, out[0]*256 + out[1]);

        // - if option length is correct
        EXPECT_EQ(25, out[2]*256 + out[3]);

        // - if option content is correct
        EXPECT_EQ(0, memcmp(out + 4, &buf_[0], 25));
    }

    OptionBuffer buf_;
    OutputBuffer out_buf_;
};

// Tests if a received option is parsed correctly. For the prefix length between
// 0 and 128 the non-significant bits should be set to 0.
TEST_F(Option6IAPrefixTest, parseShort) {

    setExampleBuffer();

    // Create an option (unpack content)
    boost::scoped_ptr<Option6IAPrefix> opt;
    ASSERT_NO_THROW(opt.reset(new Option6IAPrefix(D6O_IAPREFIX, buf_.begin(),
                                                  buf_.begin() + 25)));
    ASSERT_TRUE(opt);

    // Pack this option
    opt->pack(out_buf_);
    EXPECT_EQ(29, out_buf_.getLength());

    // The non-significant bits (above 77) of the received prefix should be
    // set to zero.
    checkOption(*opt, D6O_IAPREFIX, 77, IOAddress("2001:db8:1:0:afa8::"));

    // Set non-significant bits in the reference buffer to 0, so as the buffer
    // can be directly compared with the option buffer.
    buf_[18] = 0xa8;
    buf_.insert(buf_.begin() + 19, 5, 0);
    checkOutputBuffer(D6O_IAPREFIX);

    // Check that option can be disposed safely
    EXPECT_NO_THROW(opt.reset());
}

// Tests if a received option holding prefix of 128 bits is parsed correctly.
TEST_F(Option6IAPrefixTest, parseLong) {

    setExampleBuffer();
    // Set prefix length to the maximal value.
    buf_[8] = 128;

    // Create an option (unpack content)
    boost::scoped_ptr<Option6IAPrefix> opt;
    ASSERT_NO_THROW(opt.reset(new Option6IAPrefix(D6O_IAPREFIX, buf_.begin(),
                                                  buf_.begin() + 25)));
    ASSERT_TRUE(opt);

    // Pack this option
    opt->pack(out_buf_);
    EXPECT_EQ(29, out_buf_.getLength());

    checkOption(*opt, D6O_IAPREFIX, 128,
                IOAddress("2001:db8:1:0:afaf:0:dead:beef"));

    checkOutputBuffer(D6O_IAPREFIX);

    // Check that option can be disposed safely
    EXPECT_NO_THROW(opt.reset());
}

// Check that the prefix having length of zero is represented as a "::".
TEST_F(Option6IAPrefixTest, parseZero) {
    setExampleBuffer();
    // Set prefix length to 0.
    buf_[8] = 0;

    // Create an option (unpack content)
    boost::scoped_ptr<Option6IAPrefix> opt;
    ASSERT_NO_THROW(opt.reset(new Option6IAPrefix(D6O_IAPREFIX, buf_.begin(),
                                                  buf_.begin() + 25)));
    ASSERT_TRUE(opt);

    // Pack this option
    opt->pack(out_buf_);
    EXPECT_EQ(29, out_buf_.getLength());

    checkOption(*opt, D6O_IAPREFIX, 0, IOAddress("::"));

    // Fill the address in the reference buffer with zeros.
    buf_.insert(buf_.begin() + 9, 16, 0);
    checkOutputBuffer(D6O_IAPREFIX);

    // Check that option can be disposed safely
    EXPECT_NO_THROW(opt.reset());
}


// Checks whether a new option can be built correctly
TEST_F(Option6IAPrefixTest, build) {

    boost::scoped_ptr<Option6IAPrefix> opt;
    setExampleBuffer();

    ASSERT_NO_THROW(opt.reset(new Option6IAPrefix(12345,
                    IOAddress("2001:db8:1:0:afaf:0:dead:beef"), 77,
                                                  1000, 3000000000u)));
    ASSERT_TRUE(opt);

    checkOption(*opt, 12345, 77, IOAddress("2001:db8:1:0:afaf:0:dead:beef"));

    // Check if we can build it properly
    EXPECT_NO_THROW(opt->pack(out_buf_));
    EXPECT_EQ(29, out_buf_.getLength());
    checkOutputBuffer(12345);

    // Check that option can be disposed safely
    EXPECT_NO_THROW(opt.reset());
}

// Checks negative cases
TEST_F(Option6IAPrefixTest, negative) {

    // Truncated option (at least 25 bytes is needed)
    EXPECT_THROW(Option6IAPrefix(D6O_IAPREFIX, buf_.begin(), buf_.begin() + 24),
                 OutOfRange);

    // Empty option
    EXPECT_THROW(Option6IAPrefix(D6O_IAPREFIX, buf_.begin(), buf_.begin()),
                 OutOfRange);

    // This is for IPv6 prefixes only
    EXPECT_THROW(Option6IAPrefix(12345, IOAddress("192.0.2.1"), 77, 1000, 2000),
                 BadValue);

    // Prefix length can't be larger than 128
    EXPECT_THROW(Option6IAPrefix(12345, IOAddress("192.0.2.1"),
                                 255, 1000, 2000),
                 BadValue);
}

// Checks if the option is converted to textual format correctly.
TEST_F(Option6IAPrefixTest, toText) {
    // Create option without suboptions.
    Option6IAPrefix opt(D6O_IAPREFIX, IOAddress("2001:db8:1::"), 64, 300, 400);
    EXPECT_EQ("type=00026(IAPREFIX), len=00025: prefix=2001:db8:1::/64,"
              " preferred-lft=300, valid-lft=400",
              opt.toText());

    // Add suboptions and make sure they are printed.
    opt.addOption(OptionPtr(new OptionUint32(Option::V6, 123, 234)));
    opt.addOption(OptionPtr(new OptionUint32(Option::V6, 222, 333)));

    EXPECT_EQ("type=00026(IAPREFIX), len=00041: prefix=2001:db8:1::/64,"
              " preferred-lft=300, valid-lft=400,\noptions:\n"
              "  type=00123, len=00004: 234 (uint32)\n"
              "  type=00222, len=00004: 333 (uint32)",
              opt.toText());
}

}
