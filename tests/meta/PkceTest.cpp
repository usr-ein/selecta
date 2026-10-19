// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/Pkce.h"

#include <QSet>

#include <gtest/gtest.h>

using namespace selecta::meta;

// RFC 7636 Appendix B.
TEST(Pkce, Rfc7636AppendixBVector)
{
    const QByteArray verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    EXPECT_TRUE(pkce::isValidVerifier(verifier));
    EXPECT_EQ(pkce::challengeS256(verifier), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST(Pkce, ChallengeIsUnpaddedBase64Url)
{
    for (int i = 0; i < 50; ++i) {
        const QByteArray challenge = pkce::challengeS256(pkce::makeVerifier());
        EXPECT_EQ(challenge.size(), 43); // 32 bytes of SHA-256, base64url without '='
        EXPECT_FALSE(challenge.contains('='));
        EXPECT_FALSE(challenge.contains('+'));
        EXPECT_FALSE(challenge.contains('/'));
    }
}

TEST(Pkce, VerifierUsesTheUnreservedSetAndAValidLength)
{
    const QByteArray verifier = pkce::makeVerifier();
    EXPECT_EQ(verifier.size(), pkce::kDefaultVerifierLength);
    EXPECT_TRUE(pkce::isValidVerifier(verifier));
}

TEST(Pkce, VerifierLengthIsClampedToTheRfcRange)
{
    EXPECT_EQ(pkce::makeVerifier(1).size(), pkce::kMinVerifierLength);
    EXPECT_EQ(pkce::makeVerifier(43).size(), 43);
    EXPECT_EQ(pkce::makeVerifier(128).size(), 128);
    EXPECT_EQ(pkce::makeVerifier(10'000).size(), pkce::kMaxVerifierLength);
}

TEST(Pkce, InvalidVerifiersAreRecognised)
{
    EXPECT_FALSE(pkce::isValidVerifier(QByteArray(42, 'a')));  // too short
    EXPECT_FALSE(pkce::isValidVerifier(QByteArray(129, 'a'))); // too long
    QByteArray withSpace(50, 'a');
    withSpace[10] = ' ';
    EXPECT_FALSE(pkce::isValidVerifier(withSpace));
    QByteArray withPlus(50, 'a');
    withPlus[0] = '+';
    EXPECT_FALSE(pkce::isValidVerifier(withPlus));
}

TEST(Pkce, VerifiersAndStatesDontRepeat)
{
    QSet<QByteArray> verifiers;
    QSet<QByteArray> states;
    for (int i = 0; i < 200; ++i) {
        verifiers.insert(pkce::makeVerifier());
        states.insert(pkce::makeState());
    }
    EXPECT_EQ(verifiers.size(), 200);
    EXPECT_EQ(states.size(), 200);
}

TEST(Pkce, StateIs128BitsOfBase64Url)
{
    const QByteArray state = pkce::makeState();
    EXPECT_EQ(state.size(), 22); // 16 bytes → 22 base64url characters without padding
    EXPECT_EQ(QByteArray::fromBase64(state, QByteArray::Base64UrlEncoding).size(), 16);
}

TEST(Pkce, ConstantTimeEquals)
{
    EXPECT_TRUE(pkce::constantTimeEquals("abc", "abc"));
    EXPECT_FALSE(pkce::constantTimeEquals("abc", "abd"));
    EXPECT_FALSE(pkce::constantTimeEquals("abc", "abcd"));
    EXPECT_FALSE(pkce::constantTimeEquals("", "a"));
    EXPECT_TRUE(pkce::constantTimeEquals("", ""));
}
