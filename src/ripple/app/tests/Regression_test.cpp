//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.
    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <BeastConfig.h>
#include <ripple/test/jtx.h>
#include <ripple/app/tx/apply.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/JsonFields.h>

namespace ripple {
namespace test {

struct Regression_test : public beast::unit_test::suite
{
    // OfferCreate, then OfferCreate with cancel
    void testOffer1()
    {
        using namespace jtx;
        Env env(*this);
        auto const gw = Account("gw");
        auto const USD = gw["USD"];
        env.fund(XRP(10000), "alice", gw);
        env(offer("alice", USD(10), XRP(10)), require(owners("alice", 1)));
        env(offer("alice", USD(20), XRP(10)), json(R"raw(
                { "OfferSequence" : 2 }
            )raw"), require(owners("alice", 1)));
    }

    void testLowBalanceDestroy()
    {
        testcase("Account balance < fee destroys correct amount of XRP");
        using namespace jtx;
        Env env(*this);
        env.memoize("alice");

        // The low balance scenario can not deterministically
        // be reproduced against an open ledger. Make a local
        // closed ledger and work with it directly.
        auto closed = std::make_shared<Ledger>(
            create_genesis, env.app().config(), env.app().family());
        auto expectedDrops = SYSTEM_CURRENCY_START;
        expect(closed->info().drops == expectedDrops);

        auto const aliceXRP = 400;
        auto const aliceAmount = XRP(aliceXRP);

        auto next = std::make_shared<Ledger>(
            *closed,
            env.app().timeKeeper().closeTime());
        {
            // Fund alice
            auto const jt = env.jt(pay(env.master, "alice", aliceAmount));
            OpenView accum(&*next);

            auto const result = ripple::apply(env.app(),
                accum, *jt.stx, tapNONE, env.journal);
            expect(result.first == tesSUCCESS);
            expect(result.second);

            accum.apply(*next);
        }
        expectedDrops -= next->fees().base;
        expect(next->info().drops == expectedDrops);
        {
            auto const sle = next->read(
                keylet::account(Account("alice").id()));
            expect(sle, "sle");
            auto balance = sle->getFieldAmount(sfBalance);

            expect(balance == aliceAmount );
        }

        {
            // Specify the seq manually since the env's open ledger
            // doesn't know about this account.
            auto const jt = env.jt(noop("alice"), fee(expectedDrops),
                seq(1));

            OpenView accum(&*next);

            auto const result = ripple::apply(env.app(),
                accum, *jt.stx, tapNONE, env.journal);
            expect(result.first == tecINSUFF_FEE);
            expect(result.second);

            accum.apply(*next);
        }
        {
            auto const sle = next->read(
                keylet::account(Account("alice").id()));
            expect(sle, "sle");
            auto balance = sle->getFieldAmount(sfBalance);

            expect(balance == XRP(0));
        }
        expectedDrops -= aliceXRP * dropsPerXRP<int>::value;
        expect(next->info().drops == expectedDrops,
            "next->info().drops == expectedDrops");
    }

    void testSecp256r1key ()
    {
        testcase("Signing with a secp256r1 key should fail gracefully");
        using namespace jtx;
        Env env(*this);

        // Test case we'll use.
        auto test256r1key = [&env] (Account const& acct)
        {
            auto const baseFee = env.current()->fees().base;
            std::uint32_t const acctSeq = env.seq (acct);
            Json::Value jsonNoop = env.json (
                noop (acct), fee(baseFee), seq(acctSeq), sig(acct));
            JTx jt = env.jt (jsonNoop);
            jt.fill_sig = false;

            // Random secp256r1 public key generated by
            // https://kjur.github.io/jsrsasign/sample-ecdsa.html
            std::string const secp256r1PubKey =
                "045d02995ec24988d9a2ae06a3733aa35ba0741e87527"
                "ed12909b60bd458052c944b24cbf5893c3e5be321774e"
                "5082e11c034b765861d0effbde87423f8476bb2c";

            // Set the key in the JSON.
            jt.jv["SigningPubKey"] = secp256r1PubKey;

            // Set the same key in the STTx.
            auto secp256r1Sig = std::make_unique<STTx>(*(jt.stx));
            auto pubKeyBlob = strUnHex (secp256r1PubKey);
            assert (pubKeyBlob.second); // Hex for public key must be valid
            secp256r1Sig->setFieldVL
                (sfSigningPubKey, std::move(pubKeyBlob.first));
            jt.stx.reset (secp256r1Sig.release());

            env (jt, ter (temINVALID));
        };

        Account const alice {"alice", KeyType::secp256k1};
        Account const becky {"becky", KeyType::ed25519};

        env.fund(XRP(10000), alice, becky);

        test256r1key (alice);
        test256r1key (becky);
    }

    void testFeeEscalationAutofill()
    {
        testcase("Autofilled fee should use the escalated fee");
        using namespace jtx;
        Env env(*this, []()
            {
                auto p = std::make_unique<Config>();
                setupConfigForUnitTests(*p);
                auto& section = p->section("transaction_queue");
                section.set("minimum_txn_in_ledger_standalone", "3");
                return p;
            }(),
            features(featureFeeEscalation));
        Env_ss envs(env);

        auto const alice = Account("alice");
        env.fund(XRP(100000), alice);

        auto params = Json::Value(Json::objectValue);
        // Max fee = 50k drops
        params[jss::fee_mult_max] = 5000;
        std::vector<int> const
            expectedFees({ 10, 10, 8889, 13889, 20000 });

        // We should be able to submit 5 transactions within
        // our fee limit.
        for (int i = 0; i < 5; ++i)
        {
            envs(noop(alice), fee(none), seq(none))(params);

            auto tx = env.tx();
            if (expect(tx))
            {
                expect(tx->getAccountID(sfAccount) == alice.id());
                expect(tx->getTxnType() == ttACCOUNT_SET);
                auto const fee = tx->getFieldAmount(sfFee);
                expect(fee == drops(expectedFees[i]));
            }
        }
    }

    void run() override
    {
        testOffer1();
        testLowBalanceDestroy();
        testSecp256r1key();
        testFeeEscalationAutofill();
    }
};

BEAST_DEFINE_TESTSUITE(Regression,app,ripple);

} // test
} // ripple
