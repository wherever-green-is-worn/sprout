/**
 * @file enumservice_test.cpp UT for Sprout ENUM service.
 *
 * Copyright (C) 2013  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by post at
 * Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 */

///
///----------------------------------------------------------------------------

#include <string>
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <json/reader.h>

#include "utils.h"
#include "sas.h"
#include "enumservice.h"
#include "fakednsresolver.hpp"
#include "fakelogger.hpp"
#include "test_utils.hpp"

using namespace std;

/// Fixture for EnumServiceTest.
class EnumServiceTest : public ::testing::Test
{
  FakeLogger _log;

  EnumServiceTest()
  {
    Log::setLoggingLevel(99);
    FakeDNSResolver::reset();
    FakeDNSResolverFactory::_expected_server.s_addr = htonl(0x7f000001);
  }

  virtual ~EnumServiceTest()
  {
  }
};

class JSONEnumServiceTest : public EnumServiceTest {};
class DNSEnumServiceTest : public EnumServiceTest {};

/// A single test case.
class ET
{
public:
  ET(string in, string out) :
    _in(in),
    _out(out)
  {
  }

  void test(EnumService& enum_)
  {
    SCOPED_TRACE(_in);
    string ret = enum_.lookup_uri_from_user(_in, 0);
    EXPECT_EQ(_out, ret);
  }

private:
  string _in; //^ input
  string _out; //^ expected output
};


TEST_F(JSONEnumServiceTest, SimpleTests)
{
  JSONEnumService enum_(string(UT_DIR).append("/test_enum.json"));

  ET("+15108580271", "sip:+15108580271@ut.cw-ngv.com"   ).test(enum_);
  ET("+15108580277", "sip:+15108580277@utext.cw-ngv.com").test(enum_);
  ET("",             ""                                 ).test(enum_);
  ET("214+4324",     "sip:2144324@198.147.226.2"        ).test(enum_);
  ET("6505551234",   "sip:6505551234@ut-int.cw-ngv.com" ).test(enum_);
  ET("+16108580277", "sip:+16108580277@198.147.226.2"   ).test(enum_);
}

TEST_F(JSONEnumServiceTest, NoMatch)
{
  JSONEnumService enum_(string(UT_DIR).append("/test_enum_no_match.json"));
  ET("1234567890", "").test(enum_);
}

TEST_F(JSONEnumServiceTest, ParseError)
{
  JSONEnumService enum_(string(UT_DIR).append("/test_enum_parse_error.json"));
  EXPECT_TRUE(_log.contains("Failed to read ENUM configuration data"));
  ET("+15108580271", "").test(enum_);
}

TEST_F(JSONEnumServiceTest, MissingParts)
{
  JSONEnumService enum_(string(UT_DIR).append("/test_enum_missing_parts.json"));
  EXPECT_TRUE(_log.contains("Badly formed ENUM number block"));
  ET("+15108580271", "").test(enum_);
  ET("+15108580272", "").test(enum_);
  ET("+15108580273", "").test(enum_);
  ET("+15108580274", "sip:+15108580274@ut.cw-ngv.com").test(enum_);
}

TEST_F(JSONEnumServiceTest, MissingBlock)
{
  JSONEnumService enum_(string(UT_DIR).append("/test_enum_missing_block.json"));
  EXPECT_TRUE(_log.contains("Badly formed ENUM configuration data - missing number_blocks object"));
  ET("+15108580271", "").test(enum_);
}

TEST_F(JSONEnumServiceTest, MissingFile)
{
  JSONEnumService enum_(string(UT_DIR).append("/NONEXISTENT_FILE.json"));
  EXPECT_TRUE(_log.contains("Failed to read ENUM configuration data"));
  ET("+15108580271", "").test(enum_);
}

TEST_F(JSONEnumServiceTest, Regex)
{
  JSONEnumService enum_(string(UT_DIR).append("/test_enum_regex.json"));
  ET("5108580271", "sip:5108580271@ut.cw-ngv.com").test(enum_);
  ET("+15108580271", "sip:5108580271@ut.cw-ngv.com").test(enum_);
  ET("01115108580271", "sip:5108580271@ut.cw-ngv.com").test(enum_);
  ET("5108580272", "sip:5108580272@ut.cw-ngv.com").test(enum_);
}

TEST_F(JSONEnumServiceTest, BadRegex)
{
  JSONEnumService enum_(string(UT_DIR).append("/test_enum_bad_regex.json"));
  // Unfortunately the logs here are hard to parse, so we just look for at least one instance of the
  // "badly formed regular expression" log, followed by a JSON expression for each of the bad number blocks.
  EXPECT_TRUE(_log.contains("Badly formed regular expression in ENUM number block"));
  EXPECT_TRUE(_log.contains("\"prefix\" : \"+15108580273\""));
  EXPECT_TRUE(_log.contains("\"prefix\" : \"+15108580274\""));
  EXPECT_TRUE(_log.contains("\"prefix\" : \"+15108580275\""));
  // First entry is valid to confirm basic regular expression is valid.
  ET("+15108580271", "sip:+15108580271@ut.cw-ngv.com").test(enum_);
  // Second entry is technically invalid but it works in the obvious way and it's easier to permit than to add code to reject.
  ET("+15108580272", "sip:+15108580272@ut.cw-ngv.com").test(enum_);
  // Remaining are not - they should fail.
  ET("+15108580273", "").test(enum_);
  ET("+15108580274", "").test(enum_);
  ET("+15108580275", "").test(enum_);
}

struct ares_naptr_reply basic_naptr_reply[] = {
  {NULL, (unsigned char*)"u", (unsigned char*)"e2u+sip", (unsigned char*)"!(^.*$)!sip:\\1@ut.cw-ngv.com!", ".", 1, 1}
};

TEST_F(DNSEnumServiceTest, BasicTest)
{
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)basic_naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1234", "sip:1234@ut.cw-ngv.com").test(enum_);
}

TEST_F(DNSEnumServiceTest, BlankTest)
{
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("", "").test(enum_);
  EXPECT_EQ(FakeDNSResolver::_num_calls, 0);
}

TEST_F(DNSEnumServiceTest, PlusPrefixTest)
{
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)basic_naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("+1234", "sip:+1234@ut.cw-ngv.com").test(enum_);
}

TEST_F(DNSEnumServiceTest, ArbitraryPunctuationTest)
{
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)basic_naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1-2.3(4)", "sip:1234@ut.cw-ngv.com").test(enum_);
}

TEST_F(DNSEnumServiceTest, NonTerminalRuleTest)
{
  struct ares_naptr_reply naptr_reply[] = {{NULL, (unsigned char*)"", (unsigned char*)"e2u+sip", (unsigned char*)"!1234!5678!", ".", 1, 1}};
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)naptr_reply));
  FakeDNSResolver::_database.insert(std::make_pair(std::string("8.7.6.5.e164.arpa"), (struct ares_naptr_reply*)basic_naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1234", "sip:1234@ut.cw-ngv.com").test(enum_);
  EXPECT_EQ(FakeDNSResolver::_num_calls, 2);
}

TEST_F(DNSEnumServiceTest, MultipleRuleTest)
{
  struct ares_naptr_reply naptr_reply[] = {
    {&naptr_reply[1], (unsigned char*)"u", (unsigned char*)"e2u+sip", (unsigned char*)"!(1234)!sip:\\1@ut.cw-ngv.com!", ".", 1, 1},
    {NULL, (unsigned char*)"u", (unsigned char*)"e2u+sip", (unsigned char*)"!(5678)!sip:\\1@ut2.cw-ngv.com!", ".", 1, 1}
  };
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)naptr_reply));
  FakeDNSResolver::_database.insert(std::make_pair(std::string("8.7.6.5.e164.arpa"), (struct ares_naptr_reply*)naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1234", "sip:1234@ut.cw-ngv.com").test(enum_);
  ET("5678", "sip:5678@ut2.cw-ngv.com").test(enum_);
  EXPECT_EQ(FakeDNSResolver::_num_calls, 2);
}

TEST_F(DNSEnumServiceTest, OrderPriorityTest)
{
  struct ares_naptr_reply naptr_reply[] = {
    {&naptr_reply[1], (unsigned char*)"u", (unsigned char*)"e2u+sip", (unsigned char*)"!(^.*$)!sip:\\1@ut3.cw-ngv.com!", ".", 2, 1},
    {&naptr_reply[2], (unsigned char*)"u", (unsigned char*)"e2u+sip", (unsigned char*)"!(^.*$)!sip:\\1@ut2.cw-ngv.com!", ".", 1, 2},
    {NULL, (unsigned char*)"u", (unsigned char*)"e2u+sip", (unsigned char*)"!(^.*$)!sip:\\1@ut.cw-ngv.com!", ".", 1, 1},
  };
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1234", "sip:1234@ut.cw-ngv.com").test(enum_);
}

TEST_F(DNSEnumServiceTest, NoResponseTest)
{
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1234", "").test(enum_);
  EXPECT_EQ(FakeDNSResolver::_num_calls, 1);
}

TEST_F(DNSEnumServiceTest, IncompleteRegexpTest)
{
  struct ares_naptr_reply naptr_reply[] = {{NULL, (unsigned char*)"u", (unsigned char*)"e2u+sip", (unsigned char*)"!1234", ".", 1, 1}};
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1234", "").test(enum_);
  EXPECT_EQ(FakeDNSResolver::_num_calls, 1);
}

TEST_F(DNSEnumServiceTest, InvalidRegexpTest)
{
  struct ares_naptr_reply naptr_reply[] = {{NULL, (unsigned char*)"u", (unsigned char*)"e2u+sip", (unsigned char*)"!(!!", ".", 1, 1}};
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1234", "").test(enum_);
  EXPECT_EQ(FakeDNSResolver::_num_calls, 1);
}

TEST_F(DNSEnumServiceTest, InvalidFlagsTest)
{
  struct ares_naptr_reply naptr_reply[] = {{NULL, (unsigned char*)"foo", (unsigned char*)"e2u+sip", (unsigned char*)"!(^.*$)!sip:\\1@ut.cw-ngv.com!", ".", 1, 1}};
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1234", "").test(enum_);
  EXPECT_EQ(FakeDNSResolver::_num_calls, 1);
}

TEST_F(DNSEnumServiceTest, PstnSipTypeTest)
{
  struct ares_naptr_reply naptr_reply[] = {{NULL, (unsigned char*)"u", (unsigned char*)"e2u+pstn:sip", (unsigned char*)"!(^.*$)!sip:\\1@ut.cw-ngv.com!", ".", 1, 1}};
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1234", "sip:1234@ut.cw-ngv.com").test(enum_);
}

TEST_F(DNSEnumServiceTest, InvalidTypeTest)
{
  struct ares_naptr_reply naptr_reply[] = {{NULL, (unsigned char*)"u", (unsigned char*)"e2u+tel", (unsigned char*)"!(^.*$)!tel:\\1@ut.cw-ngv.com!", ".", 1, 1}};
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1234", "").test(enum_);
}

TEST_F(DNSEnumServiceTest, NoMatchTest)
{
  struct ares_naptr_reply naptr_reply[] = {{NULL, (unsigned char*)"u", (unsigned char*)"e2u+sip", (unsigned char*)"!5678!!", ".", 1, 1}};
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1234", "").test(enum_);
  EXPECT_EQ(FakeDNSResolver::_num_calls, 1);
}

TEST_F(DNSEnumServiceTest, LoopingRuleTest)
{
  struct ares_naptr_reply naptr_reply[] = {{NULL, (unsigned char*)"", (unsigned char*)"e2u+sip", (unsigned char*)"!(^.*$)!\\1!", ".", 1, 1}};
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa"), (struct ares_naptr_reply*)naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa", new FakeDNSResolverFactory());
  ET("1234", "").test(enum_);
  EXPECT_EQ(FakeDNSResolver::_num_calls, 5);
}

TEST_F(DNSEnumServiceTest, DifferentServerTest)
{
  FakeDNSResolverFactory::_expected_server.s_addr = htonl(0x01020304);
  DNSEnumService enum_("1.2.3.4", ".e164.arpa", new FakeDNSResolverFactory());
}

TEST_F(DNSEnumServiceTest, InvalidServerTest)
{
  DNSEnumService enum_("foobar", ".e164.arpa", new FakeDNSResolverFactory());
}

TEST_F(DNSEnumServiceTest, DifferentSuffixTest)
{
  FakeDNSResolver::_database.insert(std::make_pair(std::string("4.3.2.1.e164.arpa.cw-ngv.com"), (struct ares_naptr_reply*)basic_naptr_reply));
  DNSEnumService enum_("127.0.0.1", ".e164.arpa.cw-ngv.com", new FakeDNSResolverFactory());
  ET("1234", "sip:1234@ut.cw-ngv.com").test(enum_);
}