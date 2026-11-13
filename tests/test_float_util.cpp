// util/float_util.h: numbers from text with fast_float
#include "util/float_util.h"

#include <gtest/gtest.h>

#include <clocale>

TEST(FloatUtil, ParsesWholeNumbers)
{
	EXPECT_EQ(util::ParseFloat("1.5"), 1.5f);
	EXPECT_EQ(util::ParseFloat("-19309.25"), -19309.25f);
	EXPECT_EQ(util::ParseFloat("  +3e2 \r\n"), 300.f);
	EXPECT_EQ(util::ParseDouble("0.56666666666666665"), 0.56666666666666665);
	EXPECT_EQ(util::ParseFloat("7"), 7.f);
}

TEST(FloatUtil, RejectsWhatIsNotANumber)
{
	EXPECT_FALSE(util::ParseFloat(""));
	EXPECT_FALSE(util::ParseFloat("   "));
	EXPECT_FALSE(util::ParseFloat("abc"));
	EXPECT_FALSE(util::ParseFloat("1.5x"));		// The whole text, not a prefix
	EXPECT_FALSE(util::ParseFloat("1 2"));
	EXPECT_EQ(util::ParseFloatOr("nope", 4.f), 4.f);
}

TEST(FloatUtil, IgnoresTheLocale)
{
	// Where the decimal point is a comma, strtof stops at the '.'; the parser doesn't look at the locale
	const char* was = std::setlocale(LC_NUMERIC, nullptr);
	const std::string saved = was ? was : "C";
	if (!std::setlocale(LC_NUMERIC, "de-DE") && !std::setlocale(LC_NUMERIC, "German")) GTEST_SKIP() << "no German locale";
	EXPECT_EQ(util::ParseFloat("1.5"), 1.5f);
	EXPECT_FALSE(util::ParseFloat("1,5"));
	std::setlocale(LC_NUMERIC, saved.c_str());
}
