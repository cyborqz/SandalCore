// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_market_sales_tests.inc: buying, removing, the seller's mail and the expiry, through real sessions
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs them through aoc_tests::run; included from area_session_registered.inc)
//
//    the tax          the server's tax is the client's float formula (the cvtss2si instruction itself) for the sums 1, 100, 10,000, 2^24, 2^24 + 1 and the one under 2^31 at the rates
//                     0.1, 0.5 and 1; the window's rate and the sale's rate come from one function over one table
//    the goal         real recorded purchases (three buys, and a buy with full bags) and a removal, with only the guids rewritten: the request the tests send is the recorded request bit
//                     for bit, the answer carries the recorded fields and is the recorded answer bit for bit; the buyer pays the price and the tax the cart showed, the seller is paid by
//                     mail, the node's total grows by the tax, and a relog and a restart lose nothing and double nothing
//    the matrix       the refusals 3, 5, 8, 9, 12 and 13 change nothing on either side; two steps of one listing; a purchase and a removal; a purchase that is answered late
//    the logout       a buyer or a seller who leaves with a step in flight: the world finishes it and writes the bags, with and without it
//    the expiry       a listing past its time is mailed back to its seller; a purchase or a removal of it is answered 5 and sweeps it at once
//    the money        the seller takes the sale's money into the purse by the claim, by claim-all, once; a full purse refuses it; a claim that is refused gives it back
// ============================================================================
#include "market/market.h"
#include "market/market_tax.h"
#include "protocol/repl/market_rpc.h"

#include <chrono>
#include <set>
#include <thread>

#if defined(_M_X64) || defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace market_sales_test {

/// The recorded requests and answers of retail's Marketplace, one bunch of the PlayerController's channel each (the guids are the recording's: the tests rewrite them).
struct Recorded {
    const char* hex;
    size_t      bits;
};
constexpr Recorded kBuy1Request{"ee11847c441075f48e7d2f682ee49b6e26f471b51ff831424146bf93f8bda2c349eca415eaef00000000", 335};
constexpr Recorded kBuy1Answer{"ee09d07224f018a120a3df49fc5ed1e12476d20af57700000000", 207};
constexpr Recorded kBuy2Request{"ee11847c441075f48e7d2f682ee49b6e26f471b51f38d954d25547928d122ea35767dcbf60dd00000000", 335};
constexpr Recorded kBuy2Answer{"ee09d07224906c2ae9aa23c9460997d1ab33ee5fb06e00000000", 207};
constexpr Recorded kBuy3Request{"ee11847c441075f48e7d2f682ee49b6e26f471b51f387279ea000853e49e9557e26e4bd3ffe200000000", 335};
constexpr Recorded kBuy3Answer{"ee09d0722410b93c7500842972cfca2b71b7a5e97f7100000000", 207};
constexpr Recorded kFullRequest{"ee11847c44d0fc8a8ff3ea6870c0ca1e237207f3a67ca68b1822a6114a846c6ba46ec7ea3b4006000000", 335};
constexpr Recorded kFullAnswer{"160ad0122530d3450c11d3082542b63552b763f51d200300004006", 212};
constexpr Recorded kRemovalRequest{"e6108c5c40709a6b14a59aa9fc75a68a4fb7689d3620fd9897f40d903c559ebfe0234e188031", 302};
/// The data answer of a recorded Marketplace window before and after its first buy, as retail sent it: three partial bunches of the PlayerController's channel, the pieces one after the
/// other are the answer.  The first and the last piece of the two answers are byte for byte the same; the first buy changed two bits of the middle piece.
constexpr Recorded kDataFirstPiece{"2e8c0988731c26f071d698c3e269b960a0200af72b6029e202409e955d09b41800c0e3ac3187c5d372c1404114ee57c052840200008051195bd8599d1b5918c077a2a0cc3ed10e1eb74916e977610bed310061d59df2142a0901000000000000000000000000008020360ade2f265318aab4595a26ca5d45402c329c08306f0400000000000000000000000000000000416c14bc5f4ca6305469b3b44c94bb8a805864381160de08000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000c002000080569adb184892dbd91b1d4000000000004059d703790a5540ee0ceacb4a958175000000000000805c1d914be0725a905c1d914b84925a10f2acec4aa0c500001e678d392c9e960b060aa270bf029622140000008ccad8c2ceeadcc8c200a4d5d4f9429e74952e57d20e1713dcb0060030f8e090a75011380200000000000000000000000000041a42ecf526518c9ebf4e5ebe0453eb01448cc5a185792300000000000000000000000000000000083484d8eb4da2183d7f9dbc7c09a6d60388188b430bf346000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000c000000a6d8c2e8ca00020000000000caba1ec853a8027267505f56aa0c9e4c01000000000026405b7f22b9d38226405b7f42b6d48290676557022d0600f0386bcc61f1b45c30501085fb15b014a10000006054c6167656e746160670592cdc4419748eafaa1b9229d98305790c405875a73c850a450000000000000000000000000000a00dd1c507aac8a49b5483fcd2b2ce1e0ff053208212cc1b01000000000000000000000000000000401ba28b0f54914937a906f9a5659d3d1ee0a7400425983702000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000b0000000a095e6360692e476f6460710000000000050d6f5409e4215903b83fab25265901a00000000000080ba78148105a31684ba781481eeaa16843c2bbb1268310080c759630e8ba7e582818228dcaf80a50805000000a332b6b0b33a37b2300061c3fa4bd4a41fcf7e2ca15da41325986300c2aa3be529541e02000000000000000000000000000015d114a4074d72257eacd4a25722ea9a80023d3d3960de08000000000000000000000000000000002aa229480f9ae44afc58a945af44d43501057a7a72c0bc110000000000000000000000000000000000000000000000000000000000000000000000000000", 7616};
constexpr Recorded kDataBeforeMiddle{"000000000000000000008005000000ad34b7319024b7b3373a80000000000080b2ae07f214aa80dc19d497952a83d40000000000008077906f533218b5a077906f537a57b520e459d995408b01003cce1a73583c2d170c1444e17e052c45280000001895b1859dd5b991850108d76ded531b9d6bd3a51aeec608f8e316b6b26d421a03004011000000000000000000000000000030283b3182538ae8c8b115a3cdaa06c704e8250e1c0bf346000000000000000000000000000000006050766204a714d191632b469b550d8e09d04b1c3816e68d00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000340000000cbdc1c195c98124b99dbdd10104000000000094753d90a75005e4cea0beac5419f00500000000000048e57c8fa6c1a80549e57c8fe6bbaa0521cfcaae045a0c00e071d698c3e269b960a0200af72b602942010000c0a88c2decacce8d2c0cc0f5448b75e109bda5be1dfe5059686eb2b0956d13d21800008a0000000000000000000000000000000958028efc9035ad91d620ae0cf6dd194089f5a83e9837020000000000000000000000000000000012b0041cf9216b5a23ad415c19ecbb338012eb517d306f04000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000a001000060e80d0eae4c0e24c9edec8d0e200000000000a0aceb813c852a207706f565a5ca0032000000000000c03059feac0d462dc83059feacdf552d0879567625d06200008fb3c61c164fcb05030551b85f014b110a00000046656c6167756e646100c8e5a903204c917b565e89bb2d4dff8b7fadb09890c600004c040000000000000000000000000000382aff3f6881f2a2739c9403b5318a2a00de53b51ac6bc11000000000000000000000000000000007054fe7fd002e545e73829076a63145500bca76a358c7923000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000b000000526f756768205275627900010000000000655d0fe4295401b933a82f2b55064100000000000000b2ff660f6c346a41b2ff660ffcb26a41c8b3b22b81160300789c35e6b0785a2e182888c2fd0a588a50000000302a630b3bab73230b03384628c8e87712bf577438899db442fdfd6b85c58434060040220000000000000000000000000000405092111b555446a5e2007a781a873806688613d630e68d0000000000000000000000000000000080a0242336aaa88c4a", 7616};
constexpr Recorded kDataAfterMiddle{"000000000000000000008005000000ad34b7319024b7b3373a80000000000080b2ae07f214aa80dc19d497952a83d40000000000008077906f533218b5a077906f537a57b520e459d995408b01003cce1a73583c2d170c1444e17e052c45280000001895b1859dd5b991850108d76ded531b9d6bd3a51aeec608f8e316b6b26d421a03004011000000000000000000000000000030283b3182538ae8c8b115a3cdaa06c704e8250e1c0bf346000000000000000000000000000000006050766204a714d191632b469b550d8e09d04b1c3816e68d00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000340000000cbdc1c195c98124b99dbdd10104000000000094753d90a75005e4cea0beac5419f00500000000000048e57c8fa6c1a80549e57c8fe6bbaa0521cfcaae045a0c00e071d698c3e269b960a0200af72b602942010000c0a88c2decacce8d2c0cc0f5448b75e109bda5be1dfe5059686eb2b0956d13d21800008a0000000000000000000000000000000958028efc9035ad91d620ae0cf6dd194089f5a83e9837020000000000000000000000000000000012b0041cf9216b5a23ad415c19ecbb338012eb517d306f04000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000a001000060e80d0eae4c0e24c9edec8d0e200000000000a0aceb813c852a207706f565a5ca0032000000000000c03059feac0d462dc83059feacdf552d0879567625d06200008fb3c61c164fcb05030551b85f014b110a00000046656c6167756e646100c8e5a903204c917b565e89bb2d4dff8b7fadb09890c600004c040000000000000000000000000000382aff3f6881f2a2739c9403b5318a2a00de53b51ac6bc11000000000000000000000000000000007054fe7fd002e545e73829076a63145500bca76a358c7923000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000b000000526f756768205275627900010000000000655d0fe4295401b933a82f2b55064100000000000000b2ff660f6c346a41b2ff660ffcb26a41c8b3b22b81160300789c35e6b0785a2e182888c2fd0a588a50000000302a630b3bab73230b03384628c8e87712bf577438899db442fdfd6b85c58434060020220000000000000000000000000000405092111b555446a5e2007a781a873806688613d630e68d0000000000000000000000000000000080a0242336aaa88c4a", 7616};
constexpr Recorded kDataLastPiece{"c501f4f0340e710cd00c27ac61cc1b0100000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000058000000907aab3b430391aa13cb0308000000000028eb7a204fa10ac89d417d59a932080200000000000030697e576ba3510b32697e57eb97550b429e955d09b41800c0e3ac3187c5d372c1404114ee57c052840200008051195bd8599d1b591880033c74d571520138f3da422ced904cf42681410b36f4142a01010000000000000000000000000080a18a1e9f87a3d134f3834cc52306e935c089468374316f040000000000000000000000000000000043153d3e0f47a369e607998a470cd26b80138d06e962de08000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000800800008054d9581a5c990e8853da191a9d5c59985d995c53d819880c1208d39bdb99d8db1d4000000000004059d703790a5540ee0ceacb4a95017c92000000000000e1c1f99d1c8d5a10e1c1f99dc0ac5a10f2acec4aa0c500001e678d392c9e960b060aa270bf029622140000008ccad8c2ceeadcc8c20010f1b26ac6923068c1337e3759307849ef080c5ab0a1a75009080000000000000000000000000000ace888b6c82c393de4d0da5d2e20ea680360150da08b79230000000000000000000000000000000058d1116d9159727ac8a1b5bb5c40d4d106c02a1a4017f346000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000003e000000a4cac6d2e0ca74409cd2ced0e8e4cac2eccae440629040a6c6cae0e8cae400020000000000caba1ec853a8027267505f56aa0c1098020000000000ae1b5b9c0769d482ae1b5b9c2766d58290676557022d0600f0386bcc61f1b45c30501085fb15b014a10000006054c6167656e7461606200a83e2f1abe444276e1b1218b55dd6304760d0820d3d854a400000000000000000000000000000e043e27bd87ca9259f1661e4b6f75dc212305521975bcc1b01000000000000000000000000000000c087c4f7b0f9524b3e2dc2c86defbb842560aa422eb798370200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000090010000205536960657a603e29476864627571666572607d21436560610000000000050d6f5409e4215903b83fab25265006a18000000000040df55b10c49a31644df55b10c32ab1604", 7455};
constexpr Recorded kRemovalAnswer{"e608d85220907ecc4bfa06489e2acf5ff011270cc018", 174};

inline const std::string kSeller = "0123456789abcdef0123456789abcdef";
/// A silver a unit.
inline constexpr int64_t kUnitPrice = 100;
/// The id of a second player.
inline const std::string kSecondId = "aabbccddeeff00112233445566778899";

}  // namespace market_sales_test

using namespace market_sales_test;

// ---- the requests and the recorded bunches ------------------------------------------------------------------------------------------

/// ServerRequestMarketplaceItemPurchase as the Buy panel sends it: the market, the listing and the units.
static BunchWriter purchase_request(const repl::GuidValue& market, const repl::GuidValue& listing, int32_t quantity) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRequestMarketplaceItemPurchase");
    if (!rpc) return BunchWriter();
    repl::RpcParams params;
    const bool ok = put_guid(*rpc, params, "MarketplaceActorGuid", market) && put_guid(*rpc, params, "MarketplaceItemGuid", listing) &&
                    repl::set_rpc_param(*rpc, params, "Quantity", repl::Value::i64(quantity));
    return ok ? actor_rpc(pc, "ServerRequestMarketplaceItemPurchase", params) : BunchWriter();
}

/// The first call of the PlayerController in a bunch's bits (`server_form` false for the client's, true for the server's).
static std::optional<repl::RpcCall> call_in(const BunchWriter& w, bool server_form) {
    const auto blocks = repl::read_content_blocks(w.data(), w.bit_pos(), 0, server_form);
    if (!blocks) return std::nullopt;
    for (const auto& b : *blocks) {
        if (!b.header.is_actor || b.header.has_rep_layout) continue;
        size_t p = b.payload_pos;
        auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), w.data(), b.payload_pos + b.payload_bits, p);
        if (calls && !calls->empty()) return std::move(calls->front());
    }
    return std::nullopt;
}

/// A recorded bunch of the client (`server_form` false) or of the server, as the first call of the PlayerController it carries.
static std::optional<repl::RpcCall> recorded_call(const Recorded& r, bool server_form);

static bool same_bits(const BunchWriter& a, const BunchWriter& b) {
    bool same = a.bit_pos() == b.bit_pos();
    for (size_t i = 0; same && i < a.bit_pos(); ++i) same = ((a.data()[i / 8] >> (i % 8)) & 1) == ((b.data()[i / 8] >> (i % 8)) & 1);
    return same;
}

/// Whether two parameter sets of `rpc` write the same bits (a default value is not sent, so a decoded answer and a built one that names the default are the same on the wire).
static bool params_write_same_bits(const schema::RpcSchema& rpc, const repl::RpcParams& a, const repl::RpcParams& b) {
    BunchWriter x, y;
    bool failed = false;
    repl::write_rpc_params(rpc, a, x, {}, &failed);
    repl::write_rpc_params(rpc, b, y, {}, &failed);
    return !failed && same_bits(x, y);
}

static BunchWriter recorded_writer(const Recorded& r) {
    const std::vector<uint8_t> bytes = recorded_bytes(r.hex);
    BunchWriter w;
    w.write_bit_range(bytes.data(), 0, r.bits);
    return w;
}

static std::optional<repl::RpcCall> recorded_call(const Recorded& r, bool server_form) { return call_in(recorded_writer(r), server_form); }

/// The pieces of a recorded chain of partial bunches joined: their bits, one piece after the other, are the one bunch the client read.
static BunchWriter recorded_chain(std::initializer_list<Recorded> pieces) {
    BunchWriter w;
    for (const Recorded& r : pieces) {
        const std::vector<uint8_t> bytes = recorded_bytes(r.hex);
        w.write_bit_range(bytes.data(), 0, r.bits);
    }
    return w;
}

static const repl::RpcCall* purchase_answer(const std::vector<repl::RpcCall>& calls) { return call_named(calls, "ClientMarketplaceItemPurchaseResponse"); }
static const repl::RpcCall* removal_answer(const std::vector<repl::RpcCall>& calls) { return call_named(calls, "ClientMarketplaceItemRemovalResponse"); }

static repl::GuidValue answer_listing(const repl::RpcCall& c) {
    return {word_of(c, "MarketplaceItemGuid.A").value_or(0), word_of(c, "MarketplaceItemGuid.B").value_or(0), word_of(c, "MarketplaceItemGuid.C").value_or(0),
            word_of(c, "MarketplaceItemGuid.D").value_or(0)};
}

static repl::GuidValue guid_of_hex(const std::string& hex) { return items::guid_of(hex).value_or(repl::GuidValue{}); }

/// A harness that has joined, phased in and opened the Marketplace at the Manager.
static void ready_to_trade(MarketHarness& h) {
    h.play();
    h.run(milliseconds(1500));
    h.open_market(h.manager);
}

static svc::Character buyer_record() { return with_start_purse(cyb_record()); }

static svc::Character second_record() {
    svc::Character c = with_start_purse(cyb_record());
    c.id = kSecondId;
    c.name = "second";
    return c;
}

static int32_t cached_quantity(MarketHarness& h, const std::string& id) {
    const ::aoc::worldd::market::Listing* l = h.fx.world->market_listings()->find(guid_of_hex(id));
    return l ? l->item.quantity : -1;
}

/// What the player's bags and purse hold, to see that a refusal changed nothing.
struct BuyerState {
    std::vector<svc::InventoryItem> items;
    int64_t                         copper = 0;
    bool operator==(const BuyerState&) const = default;
};
static BuyerState state_of(MarketHarness& h) { return BuyerState{h.s->inventory().items(), h.s->wallet().copper()}; }

/// Ticks the world until `done` (a step of an ended session is finished by the world's tick) or the time is up.
static bool tick_world_until(MarketHarness& h, const std::function<bool()>& done, int milliseconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    for (;;) {
        h.fx.world->tick_world(world_time_at(h.t));
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// ---- the tax ----------------------------------------------------------------------------------------------------------------------------

/// The client's RoundToInt on the machine's own instruction: cvtss2si(F + F + 0.5) >> 1.
static int64_t client_round_to_int(int64_t sum, float percent, int32_t flat) {
#if defined(_M_X64) || defined(__SSE__)
    const float f = static_cast<float>(sum) * percent + static_cast<float>(flat);
    return static_cast<int64_t>(_mm_cvt_ss2si(_mm_set_ss(f + f + 0.5f)) >> 1);
#else
    (void)sum;
    (void)percent;
    (void)flat;
    return -1;
#endif
}

static void test_market_tax_is_the_clients_float_formula() {
    std::printf("the Marketplace: the sales tax is the client's float formula\n");
    namespace mk = ::aoc::worldd::market;
    // the retail buys: 100 was paid as 110, 10,000 as 11,000 and 1 as 1 (at the rate 0.1)
    const mk::TaxRow tenth{0.1f, 0, true}, half{0.5f, 0, true}, whole{1.0f, 0, true};
    const auto c100 = mk::charge_for(100, 1, tenth), c10000 = mk::charge_for(10000, 1, tenth), c1 = mk::charge_for(1, 1, tenth);
    CHECK(c100 && c100->tax == 10 && c100->total == 110, "100 is paid as 110 at 10 percent");
    CHECK(c10000 && c10000->tax == 1000 && c10000->total == 11000, "10,000 is paid as 11,000 at 10 percent");
    CHECK(c1 && c1->tax == 0 && c1->total == 1, "1 is paid as 1 at 10 percent");
    const int64_t sums[] = {1, 100, 10000, int64_t{1} << 24, (int64_t{1} << 24) + 1, (int64_t{1} << 31) - 1};
    const float rates[] = {0.1f, 0.5f, 1.0f};
    bool all = true;
    for (const int64_t sum : sums) {
        for (const float rate : rates) {
            const std::optional<int64_t> mine = mk::cart_tax(sum, rate, 0);
            const float f = static_cast<float>(sum) * rate;
            const bool fits = f + f + 0.5f < 2147483648.0f;
            if (!fits) {
                all = all && !mine;   // a value the 32-bit arithmetic cannot hold has no tax
                continue;
            }
            const int64_t theirs = client_round_to_int(sum, rate, 0);
            const bool same = mine && theirs >= 0 && *mine == theirs;
            if (!same) {
                std::printf("  sum %lld rate %g: server %lld, instruction %lld\n", static_cast<long long>(sum), static_cast<double>(rate), mine ? static_cast<long long>(*mine) : -1LL,
                            static_cast<long long>(theirs));
            }
            all = all && same;
        }
    }
    CHECK(all, "the server's tax is the instruction's for 1, 100, 10,000, 2^24, 2^24 + 1 and 2^31 - 1 at 0.1, 0.5 and 1");
    const auto exact = mk::cart_tax((int64_t{1} << 24) + 1, 0.5f, 0);
    CHECK(exact && *exact == (int64_t{1} << 23), "2^24 + 1 is not a single-precision number: the client sees 2^24");
    CHECK(!mk::charge_for(int64_t{1} << 31, 1, tenth) && !mk::charge_for((int64_t{1} << 31) - 1, 1, tenth), "a sum of 2^31, and a taxed sum of 2^31 or more, is no sale");
    CHECK(!mk::charge_for(INT64_MAX, 2, half) && !mk::charge_for(1, 0, half) && !mk::charge_for(0, 5, half) && !mk::charge_for(5, -1, half), "an overflow, no units and no price are no sale");
    CHECK(mk::charge_for(1'000'000'000, 1, whole).has_value() && !mk::charge_for(1'100'000'000, 1, whole), "a billion at the whole rate is 2 billion with its tax, a sale; 1.1 billion is 2.2 billion: no sale");
    const auto edge = mk::charge_for(1'000'000'000, 1, tenth);
    CHECK(edge && edge->total == 1'100'000'000, "a billion at 10 percent is a sale");
    CHECK(!mk::cart_tax(-1, 0.5f, 0), "a negative sum has no tax");

    // the rate: one function over the table the open sends
    for (const float rate : rates) {
        const repl::NodeTaxDetails table = mk::market_tax_table(kNode, rate);
        CHECK(mk::sale_rate(table) == rate, "the window's rate is the node's rate");
        const mk::TaxRow row = mk::sale_row(table);
        CHECK(row.found && row.percent == rate && row.flat == 0, "the sale charges the row the window shows");
        bool shops_untouched = true;
        const repl::NodeTaxDetails plain = vendor::default_table(kNode);
        for (size_t i = 0; i < table.entries.size() && i < plain.entries.size(); ++i) {
            if (table.entries[i].tag == mk::kMarketTaxTag) continue;
            shops_untouched = shops_untouched && table.entries[i] == plain.entries[i];
        }
        CHECK(shops_untouched && table.entries.size() == plain.entries.size(), "no other row of the table changes: the shops' tables stay as they are");
    }
    CHECK(mk::sale_rate(vendor::default_table(kNode)) == 0.5f, "the default table climbs to the root row at 0.5 (the unset MarketplaceTax row is skipped)");
}

// ---- the recorded requests and answers --------------------------------------------------------------------------------------------

static void test_market_recorded_requests_and_answers() {
    std::printf("the Marketplace: the recorded purchases and removal are the requests and answers the tests build\n");
    struct Pair {
        const Recorded* request;
        const Recorded* answer;
        int32_t         quantity;
        uint64_t        response;
    };
    const Pair purchases[] = {{&kBuy1Request, &kBuy1Answer, 1, 0}, {&kBuy2Request, &kBuy2Answer, 1, 0}, {&kBuy3Request, &kBuy3Answer, 1, 0}, {&kFullRequest, &kFullAnswer, 12, 12}};
    for (const Pair& p : purchases) {
        const std::optional<repl::RpcCall> req = recorded_call(*p.request, /*server_form=*/false);
        CHECK(req && req->decoded && req->rpc && std::string(req->rpc->name) == "ServerRequestMarketplaceItemPurchase", "the recorded purchase request reads");
        if (!req || !req->decoded) continue;
        const std::optional<repl::MarketPurchase> parsed = repl::parse_market_purchase(*req);
        CHECK(parsed && parsed->quantity == p.quantity, "it names the units the recording bought");
        if (!parsed) continue;
        CHECK(same_bits(purchase_request(parsed->market, parsed->listing, parsed->quantity), recorded_writer(*p.request)),
              "the request the tests build from its guids and units is the recorded one, bit for bit");
        const std::optional<repl::RpcCall> ans = recorded_call(*p.answer, /*server_form=*/true);
        CHECK(ans && ans->decoded && ans->rpc && std::string(ans->rpc->name) == "ClientMarketplaceItemPurchaseResponse", "the recorded answer reads");
        if (!ans || !ans->decoded) continue;
        CHECK(answer_listing(*ans) == parsed->listing, "the answer names the listing the request named");
        CHECK(number_of(*ans, "Quantity").value_or(-1) == p.quantity, "the answer carries the units the request asked for (a refusal too)");
        CHECK(enum_of(*ans, "Response").value_or(0) == p.response, "the answer's response is the recorded one");
        const std::optional<repl::RpcParams> built = repl::market_purchase_params(*ans->rpc, answer_listing(*ans), p.quantity, static_cast<uint8_t>(p.response));
        CHECK(built.has_value(), "the answer builds from the recorded values");
        if (!built) continue;
        CHECK(params_write_same_bits(*ans->rpc, *built, ans->params), "the built answer is the recorded one bit for bit (a response of 0 is not sent, in retail's or ours)");
        BunchWriter whole;
        bool failed = false;
        repl::write_rpc_params(*ans->rpc, *built, whole, {}, &failed);
        CHECK(!failed && whole.bit_pos() == ans->param_bits, "and it is as long as the recorded answer's parameters");
        const bool nonzero = p.response != 0;
        CHECK(!nonzero || *built == ans->params, "a response that is sent is held at the recorded place");
    }
    const std::optional<repl::RpcCall> req = recorded_call(kRemovalRequest, false);
    CHECK(req && req->decoded && std::string(req->rpc->name) == "ServerRequestMarketplaceItemRemoval", "the recorded removal request reads");
    if (!req || !req->decoded) return;
    const std::optional<repl::MarketRemoval> parsed = repl::parse_market_removal(*req);
    CHECK(parsed.has_value(), "it names a market and a listing");
    if (!parsed) return;
    CHECK(same_bits(removal_request(parsed->market, parsed->listing), recorded_writer(kRemovalRequest)), "the request the tests build is the recorded one, bit for bit");
    const std::optional<repl::RpcCall> ans = recorded_call(kRemovalAnswer, true);
    CHECK(ans && ans->decoded && std::string(ans->rpc->name) == "ClientMarketplaceItemRemovalResponse" && answer_listing(*ans) == parsed->listing, "the recorded answer names the listing");
    if (!ans || !ans->decoded) return;
    const std::optional<repl::RpcParams> built = repl::market_removal_params(*ans->rpc, answer_listing(*ans), 0);
    CHECK(built && params_write_same_bits(*ans->rpc, *built, ans->params), "the removal answer built from the recorded values is the recorded one, bit for bit");
}

// ---- the goal: buying all or part of a stack ----------------------------------------------------------------------------------------

/// The window's cart total for `sum` at the node's table: the price and the tax the client adds up from the table the open sent.
static int64_t cart_total(const repl::NodeTaxDetails& table, int64_t sum) {
    const ::aoc::worldd::market::TaxRow row = ::aoc::worldd::market::sale_row(table);
    return sum + ::aoc::worldd::market::cart_tax(sum, row.percent, row.flat).value_or(-1000000);
}

/// The three recorded buys, replayed with our guids: the buyer pays the price and the node's tax, the units arrive, the listing shrinks or leaves, the seller is mailed the full price and
/// the node's total grows by exactly the tax.  Then a relog and a restart.
static void test_market_goal_buy_all_or_part_of_a_stack() {
    std::printf("the Marketplace: the goal: a buyer buys all or part of a stack, the seller is paid by mail\n");
    MarketDb db;
    db.store_character(buyer_record());
    // the recorded buys are of one unit each: the first and the last listing hold more (a part of the stack), the middle one holds one (the whole stack)
    const std::string l1 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 5, kUnitPrice), l2 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 1, kUnitPrice),
                      l3 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 3, kUnitPrice);
    MarketHarness h(buyer_record(), &db);
    size_t from = h.got.size();
    ready_to_trade(h);
    const std::vector<repl::RpcCall> opened = pc_calls_since(h, from);
    const repl::RpcCall* open = call_named(opened, "ClientOpenMarketplaceUI");
    CHECK(open != nullptr, "the open sent the tax table");
    if (!open) return;
    const auto* table = repl::rpcf::as<repl::NodeTaxDetails>(repl::rpcf::value_of(*open->rpc, open->params, "NodeTaxDetails"));
    CHECK(table && ::aoc::worldd::market::sale_rate(*table) == 0.5f, "the window shows the node's rate, the game's default 0.5 until a mayor sets one");
    if (!table) return;
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const int64_t purse = h.s->wallet().copper();
    CHECK(purse == kStartPurse, "the buyer starts with the 21 gold start purse");
    const int64_t potions = potion_units(h.s->inventory());
    int64_t paid = 0;
    struct Step {
        const std::string* id;
        const Recorded*    request;
        const Recorded*    answer;
        int32_t            quantity;
    };
    const Step steps[] = {{&l1, &kBuy1Request, &kBuy1Answer, 1}, {&l2, &kBuy2Request, &kBuy2Answer, 1}, {&l3, &kBuy3Request, &kBuy3Answer, 1}};
    for (const Step& s : steps) {
        const std::optional<repl::RpcCall> rr = recorded_call(*s.request, false);
        const std::optional<repl::RpcCall> ra = recorded_call(*s.answer, true);
        CHECK(rr && ra && rr->decoded && ra->decoded, "the recorded request and answer read");
        if (!rr || !ra || !rr->decoded || !ra->decoded) return;
        const std::optional<repl::MarketPurchase> recorded = repl::parse_market_purchase(*rr);
        CHECK(recorded && recorded->quantity == s.quantity, "the recorded purchase buys the units of the step");
        const repl::GuidValue listing = guid_of_hex(*s.id);
        const int64_t total = cart_total(*table, kUnitPrice * s.quantity);
        const int64_t before = h.s->wallet().copper();
        from = h.got.size();
        h.client_request(purchase_request(market, listing, s.quantity), h.pc_ch);
        const std::vector<repl::RpcCall> calls = pc_calls_since(h, from);
        const repl::RpcCall* answer = purchase_answer(calls);
        CHECK(answer != nullptr, "the purchase is answered");
        if (!answer) return;
        CHECK(answer_listing(*answer) == listing, "the answer names the listing that was asked for");
        CHECK(number_of(*answer, "Quantity") == number_of(*ra, "Quantity") && enum_of(*answer, "Response").value_or(0) == enum_of(*ra, "Response").value_or(0),
              "the answer's fields are the recorded answer's: the units asked for, and the response 0");
        const std::optional<repl::RpcParams> mine = repl::market_purchase_params(*answer->rpc, listing, s.quantity, 0);
        CHECK(mine && params_write_same_bits(*answer->rpc, *mine, answer->params), "the answer is exactly the listing, the units and the response (nothing more, nothing less)");
        CHECK(h.s->wallet().copper() == before - total, "the buyer's purse lost the price plus the tax: the total the cart showed");
        paid += total;
        const repl::RpcCall* callout = currency_callout(calls);
        CHECK(callout != nullptr, "the money popup follows the purchase");
        CHECK(count_calls(calls, "ClientRecieveBatchedStorageEvents") == 1 && count_calls(calls, "ClientReceiveStorageEvent") == 0, "one batched bag event brings the units: retail's call");
    }
    CHECK(h.s->wallet().copper() == purse - paid && paid == 3 * 150, "the three buys cost 150 copper each: the price 100 and the tax of 50 percent");
    CHECK(potion_units(h.s->inventory()) == potions + 3, "the bags gained the 3 units");
    CHECK(cached_quantity(h, l1) == 4 && cached_quantity(h, l2) == -1 && cached_quantity(h, l3) == 2, "the first and third listings shrank to 4 and 2 units and the middle one, sold whole, left the window");
    CHECK(db.quantity_of(l1) == 4 && db.status_of(l1) == market_db::kListed && db.status_of(l2) == market_db::kSold && db.quantity_of(l3) == 2 && db.status_of(l3) == market_db::kListed,
          "the database agrees: the part sales left 4 and 2 units, the whole sale is SOLD");
    const std::vector<svc::Mail> sales = db.sale_mails(kSeller);
    CHECK(sales.size() == 3 && sales[0].money == 100 && sales[1].money == 100 && sales[2].money == 100 && sales[0].sender == "Marketplace",
          "the seller has one mail for each listing, with the full price attached (not the tax)");
    CHECK(db.collected_of(kNode) == 3 * 50, "the node's total grew by exactly the tax of each sale");
    CHECK(db.copper_of(kSeller) == 0, "the seller's purse did not change: the money comes by mail");
    CHECK(db.copper_of(buyer_record().id) == purse - paid, "the database holds the buyer's purse");
    // the seller's storefront as the window shows it now
    from = h.got.size();
    h.client_request(data_request(market, guid_of_hex(kSeller), true), h.pc_ch);
    const std::vector<repl::RpcCall> data_calls = pc_calls_since(h, from);
    const repl::RpcCall* data = call_named(data_calls, "ClientReceiveMarketplaceData");
    CHECK(data != nullptr, "the seller's storefront is answered");
    if (data) {
        const repl::ArrayValue* list = array_of(*data, "MarketplaceItemData");
        CHECK(list && list->elements.size() == 2, "it holds the two listings that are left");
        if (list && list->elements.size() == 2) {
            const auto* a = repl::rpcf::as<repl::ItemInstance>(member_of(*data, "MarketplaceItemData", list->elements[0], "MarketplaceItemData.Item"));
            const auto* b = repl::rpcf::as<repl::ItemInstance>(member_of(*data, "MarketplaceItemData", list->elements[1], "MarketplaceItemData.Item"));
            CHECK(a && b && a->quantity + b->quantity == 6 && a->record_id == static_cast<uint64_t>(fi::kPotion), "their stacks show the 4 and 2 units that are left");
        }
    }
    // the player logs out and in again, and the world restarts: nothing is lost and nothing is doubled
    const std::string id = buyer_record().id;
    h.s->finish("the client left");
    h.s.reset();
    CHECK(!h.fx.world->character_writes_pending(id), "nothing of the buyer's is waiting to be written");
    {
        MarketPeer peer(h, db.stored_character(buyer_record()));
        peer.play();
        CHECK(potion_units(peer.s->inventory()) == potions + 3 && peer.s->wallet().copper() == purse - paid, "after the relog the bags and the purse are what the purchases left");
        peer.s->finish("the client left");
        peer.s.reset();
    }
    MarketHarness restarted(db.stored_character(buyer_record()), &db);
    restarted.play();
    CHECK(restarted.fx.world->market_listings()->size() == 2 && cached_quantity(restarted, l1) == 4 && cached_quantity(restarted, l3) == 2, "the restarted world reads the two listings, with their 4 and 2 units");
    CHECK(potion_units(restarted.s->inventory()) == potions + 3 && restarted.s->wallet().copper() == purse - paid, "after the restart the bags and the purse are the same");
    CHECK(db.sale_mails(kSeller).size() == 3 && db.collected_of(kNode) == 150, "the seller's mails and the node's total are the same: nothing was paid twice");
    CHECK(restarted.fx.world->market_listings()->node_rate(kNode) == 0.5f, "the node's rate is read at the start: 0.5");
}

/// The recorded buy with full bags: the bags are full and the answer is 12, with the units the request asked for; no popup, no bag event, and nothing changes on either side.
static void test_market_goal_full_bags_refuse_the_buy() {
    std::printf("the Marketplace: the goal: with full bags the buy is refused with 12\n");
    MarketDb db;
    std::vector<svc::InventoryItem> full = kit();
    for (int slot = 2; slot < 70; ++slot) full.push_back(make_item(Bag, slot, fi::kSword2H, 5000 + static_cast<uint32_t>(slot)));
    for (svc::InventoryItem& it : full) {
        if (it.container == Bag && it.slot == 1) it.quantity = 20;   // a full stack: nothing joins it
    }
    const svc::Character record = with_start_purse(cyb_record(full));
    db.store_character(record);
    const std::string listing = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 60, kUnitPrice);
    MarketHarness h(record, &db);
    ready_to_trade(h);
    const BuyerState before = state_of(h);
    CHECK(h.s->inventory().first_free(Bag) < 0, "the item bag is full");
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const std::optional<repl::RpcCall> ra = recorded_call(kFullAnswer, true);
    CHECK(ra && ra->decoded, "the recorded answer reads");
    if (!ra || !ra->decoded) return;
    const size_t from = h.got.size();
    h.client_request(purchase_request(market, guid_of_hex(listing), 12), h.pc_ch);
    const std::vector<repl::RpcCall> calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = purchase_answer(calls);
    CHECK(answer != nullptr, "the purchase is answered");
    if (!answer) return;
    CHECK(enum_of(*answer, "Response").value_or(0) == 12 && enum_of(*ra, "Response").value_or(0) == 12, "the answer is 12, INVENTORY_FULL, as the recorded one is");
    CHECK(number_of(*answer, "Quantity").value_or(-1) == 12 && number_of(*ra, "Quantity").value_or(-2) == 12, "the answer carries the 12 units that were asked for, as the recorded one carries its request's");
    const std::optional<repl::RpcParams> built = repl::market_purchase_params(*answer->rpc, guid_of_hex(listing), 12, 12);
    CHECK(built && params_write_same_bits(*answer->rpc, *built, answer->params), "the answer holds the listing, the 12 units and the response 12");
    CHECK(currency_callout(calls) == nullptr && count_calls(calls, "ClientReceiveStorageEvent") == 0 && count_calls(calls, "ClientRecieveBatchedStorageEvents") == 0, "no money popup and no bag event follow a refusal");
    CHECK(state_of(h) == before, "the buyer's bags and purse are the same");
    CHECK(cached_quantity(h, listing) == 60 && db.quantity_of(listing) == 60 && db.status_of(listing) == market_db::kListed, "the listing is the same on both sides");
    CHECK(db.sale_mails(kSeller).empty() && db.collected_of(kNode) == 0 && db.buy_calls == 0, "nothing was stored: no mail, no tax, not even a call");
}

/// The recorded removal: the stack comes back in one batched bag event, no money moves, and the fee is not refunded.
static void test_market_goal_removal_gives_the_stack_back() {
    std::printf("the Marketplace: the goal: a seller removes a listing, the stack comes back and the fee is not refunded\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string listing = db.seed_listing(record.id, "cyb", kNode, fi::kPotion, 8, kUnitPrice);
    const std::string other = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 2, kUnitPrice);
    MarketHarness h(record, &db);
    ready_to_trade(h);
    const int64_t purse = h.s->wallet().copper();
    const int64_t potions = potion_units(h.s->inventory());
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const std::optional<repl::RpcCall> ra = recorded_call(kRemovalAnswer, true);
    CHECK(ra && ra->decoded, "the recorded answer reads");
    if (!ra || !ra->decoded) return;
    const size_t from = h.got.size();
    h.client_request(removal_request(market, guid_of_hex(listing)), h.pc_ch);
    const std::vector<repl::RpcCall> calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = removal_answer(calls);
    CHECK(answer != nullptr, "the removal is answered");
    if (!answer) return;
    CHECK(answer_listing(*answer) == guid_of_hex(listing) && enum_of(*answer, "Response").value_or(0) == enum_of(*ra, "Response").value_or(0), "the answer names the listing, with the recorded response 0");
    const std::optional<repl::RpcParams> built = repl::market_removal_params(*answer->rpc, guid_of_hex(listing), 0);
    CHECK(built && params_write_same_bits(*answer->rpc, *built, answer->params), "the answer holds exactly the listing and the response (as the recorded one does)");
    CHECK(count_calls(calls, "ClientRecieveBatchedStorageEvents") == 1 && count_calls(calls, "ClientReceiveStorageEvent") == 0 && currency_callout(calls) == nullptr, "one batched bag event and no money popup");
    CHECK(potion_units(h.s->inventory()) == potions + 8 && h.s->wallet().copper() == purse, "the 8 units are back in the bags and the purse is the same: nothing is refunded");
    CHECK(cached_quantity(h, listing) == -1 && db.status_of(listing) == market_db::kRemoved, "the listing is gone from the window and REMOVED in the database");
    CHECK(db.units_of(record.id, fi::kPotion) == potions + 8 && db.copper_of(record.id) == purse, "the database holds the bags with the stack in them, and the same purse");
    CHECK(db.sale_mails(record.id).empty() && db.collected_of(kNode) == 0, "no mail and no tax: a removal is no sale");
    // the same removal again, and another player's listing, are answered 5 and change nothing
    const BuyerState before = state_of(h);
    size_t at = h.got.size();
    h.client_request(removal_request(market, guid_of_hex(listing)), h.pc_ch);
    const std::vector<repl::RpcCall> again_calls = pc_calls_since(h, at);
    const repl::RpcCall* again = removal_answer(again_calls);
    CHECK(again && enum_of(*again, "Response").value_or(0) == 5, "a removal of a listing that is gone is answered 5");
    at = h.got.size();
    h.client_request(removal_request(market, guid_of_hex(other)), h.pc_ch);
    const std::vector<repl::RpcCall> theirs_calls = pc_calls_since(h, at);
    const repl::RpcCall* theirs = removal_answer(theirs_calls);
    CHECK(theirs && enum_of(*theirs, "Response").value_or(0) == 5, "a removal of another player's listing is answered 5, like a missing one");
    CHECK(state_of(h) == before && db.status_of(other) == market_db::kListed && cached_quantity(h, other) == 2 && db.remove_calls == 1, "nothing changed, and the database was asked once");
}

// ---- the refusals ------------------------------------------------------------------------------------------------------------------------

static void test_market_refusals_change_nothing() {
    std::printf("the Marketplace: the refusals 5, 8, 9, 13 change nothing on either side\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string ok = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
    const std::string mine = db.seed_listing(record.id, "cyb", kNode, fi::kPotion, 3, kUnitPrice);
    const std::string dear = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 100, 5'000'000);      // 500,000,000 copper: more than the purse holds
    const std::string huge = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 1000, 3'000'000);     // a sum of 3 billion: the client cannot show it
    const std::string elsewhere = db.seed_listing(kSeller, "Brannoc", kNode + 1, fi::kPotion, 5, kUnitPrice);
    MarketHarness h(record, &db);
    ready_to_trade(h);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const BuyerState before = state_of(h);
    const auto buy = [&](const std::string& id, int32_t quantity, uint64_t want, const char* what) {
        const size_t from = h.got.size();
        h.client_request(purchase_request(market, guid_of_hex(id), quantity), h.pc_ch);
        const std::vector<repl::RpcCall> calls = pc_calls_since(h, from);
        const repl::RpcCall* answer = purchase_answer(calls);
        CHECK(answer != nullptr && enum_of(*answer, "Response").value_or(99) == want, what);
        CHECK(answer && number_of(*answer, "Quantity").value_or(-1) == quantity, "... and it carries the units that were asked for");
        CHECK(currency_callout(calls) == nullptr && count_calls(calls, "ClientReceiveStorageEvent") == 0 && count_calls(calls, "ClientRecieveBatchedStorageEvents") == 0, "... with no money popup and no bag event");
        CHECK(state_of(h) == before, "... and the buyer's bags and purse are the same");
    };
    buy(std::string(32, 'e'), 1, 5, "a listing that is not there is answered 5");
    buy(elsewhere, 1, 5, "a listing of another market is answered 5");
    buy(ok, 0, 8, "no units are answered 8");
    buy(ok, 11, 8, "more units than the listing holds are answered 8");
    buy(ok, -3, 8, "negative units are answered 8");
    buy(mine, 1, 9, "the buyer's own listing is answered 9");
    buy(dear, 1, 13, "a price the purse cannot pay is answered 13");
    buy(huge, 1000, 13, "a sum the client's arithmetic cannot show is answered 13");
    CHECK(cached_quantity(h, ok) == 10 && cached_quantity(h, dear) == 100 && cached_quantity(h, mine) == 3 && db.buy_calls == 0, "the listings are the same and the database was not asked");
    // another market's guid is ignored: no answer at all; so is a purchase with no window open
    const repl::GuidValue other_market = ::aoc::worldd::market::marketplace_guid(kNode + 7);
    size_t from = h.got.size();
    h.client_request(purchase_request(other_market, guid_of_hex(ok), 1), h.pc_ch);
    CHECK(purchase_answer(pc_calls_since(h, from)) == nullptr && state_of(h) == before, "a purchase naming another market is ignored");
    MarketHarness closed(buyer_record(), &db);
    closed.play();
    closed.run(milliseconds(1500));
    from = closed.got.size();
    closed.client_request(purchase_request(market, guid_of_hex(ok), 1), closed.pc_ch);
    CHECK(purchase_answer(pc_calls_since(closed, from)) == nullptr && closed.s->wallet().copper() == kStartPurse && db.buy_calls == 0, "a purchase with no window open is ignored");
}

// ---- the world keeps one step in flight ------------------------------------------------------------------------------------------------

static void test_market_one_step_in_flight() {
    std::printf("the Marketplace: one step in flight for a listing and for a character\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string l1 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
    const std::string l2 = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
    db.hold_buy = true;
    MarketHarness h(record, &db);
    ready_to_trade(h);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const int64_t purse = h.s->wallet().copper();
    size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, purchase_request(market, guid_of_hex(l1), 4), true)});
    CHECK(db.wait_started(3000), "the purchase reached the database");
    for (int i = 0; i < 3; ++i) h.step();
    CHECK(purchase_answer(pc_calls_since(h, from)) == nullptr, "the client is told nothing before the database answers");
    CHECK(h.s->market_pending() == 1 && h.s->market_save_in_flight(), "the step is in flight");
    // the same purchase twice (a double submit) is answered 3 and never reaches the database; a purchase of another listing waits its turn behind the first (a cart's second line)
    from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, purchase_request(market, guid_of_hex(l1), 4), true)});
    h.client_sends({h.client.actor(h.pc_ch, purchase_request(market, guid_of_hex(l2), 1), true)});
    for (int i = 0; i < 4; ++i) h.step();
    size_t threes = 0;
    for (const repl::RpcCall& c : pc_calls_since(h, from)) {
        if (c.rpc && std::string(c.rpc->name) == "ClientMarketplaceItemPurchaseResponse" && enum_of(c, "Response").value_or(0) == 3) ++threes;
    }
    CHECK(threes == 1, "the same purchase again is answered 3");
    {
        std::lock_guard<std::mutex> lock(db.mu);
        CHECK(db.buy_calls == 1, "the database was asked once: the purchase of the other listing waits for the first one's answer");
    }
    CHECK(h.s->market_pending() == 1 && h.s->market_queued() == 1, "one step in flight and one waiting");
    db.release_all();
    h.settle();
    CHECK(h.s->market_pending() == 0 && h.s->market_queued() == 0 && !h.s->market_save_in_flight(), "both steps landed");
    CHECK(h.s->wallet().copper() == purse - 600 - 150 && potion_units(h.s->inventory()) == 5 + 4 + 1,
          "the first purchase is made once (400 and 200 tax paid, 4 units in the bags) and then the second one's unit (100 and 50 tax)");
    CHECK(db.buy_calls == 2 && cached_quantity(h, l1) == 6 && cached_quantity(h, l2) == 9, "two calls, and the listings show their 6 and 9 units");
}

/// A second buyer's purchase of the listing a first buyer's purchase is in flight for is answered 3; once the first lands the second is served from what is left.
static void test_market_two_buyers_of_one_listing() {
    std::printf("the Marketplace: two buyers of one listing: the second waits for the first\n");
    MarketDb db;
    db.store_character(buyer_record());
    db.store_character(second_record());
    const std::string listing = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
    db.hold_buy = true;
    MarketHarness h(buyer_record(), &db);
    ready_to_trade(h);
    MarketPeer peer(h, second_record());
    peer.play();
    peer.request(interact(market_component(h.fx.world->registry(), h.manager)), peer.pawn_ch);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    h.client_sends({h.client.actor(h.pc_ch, purchase_request(market, guid_of_hex(listing), 10), true)});
    CHECK(db.wait_started(3000), "the first buyer's purchase reached the database");
    size_t from = peer.got.size();
    peer.request(purchase_request(market, guid_of_hex(listing), 5), peer.pc_ch);
    const std::vector<repl::RpcCall> answer_calls = market_calls_since(peer.got, peer.pc_ch, from);
    const repl::RpcCall* answer = purchase_answer(answer_calls);
    CHECK(answer && enum_of(*answer, "Response").value_or(0) == 3, "the second buyer is answered 3 while the first's step is in flight");
    CHECK(peer.s->wallet().copper() == kStartPurse && db.buy_calls == 1, "nothing was taken from him and the database was asked once");
    db.release_all();
    h.settle();
    from = peer.got.size();
    peer.request(purchase_request(market, guid_of_hex(listing), 5), peer.pc_ch);
    const std::vector<repl::RpcCall> later_calls = market_calls_since(peer.got, peer.pc_ch, from);
    const repl::RpcCall* later = purchase_answer(later_calls);
    CHECK(later && enum_of(*later, "Response").value_or(0) == 5, "once the first buyer has all 10 units the second is answered 5: no longer available");
    CHECK(peer.s->wallet().copper() == kStartPurse && potion_units(peer.s->inventory()) == 5, "the second buyer paid nothing and holds what he held");
    CHECK(h.s->wallet().copper() == kStartPurse - 1500 && db.sale_mails(kSeller).size() == 1 && db.collected_of(kNode) == 500, "the first buyer paid 1000 and 500 tax, once");
}

/// The seller is online: a removal while a purchase of the same listing is in flight is answered 3; the seller is told of the sale's mail at once; a removal after it takes the
/// units that are left.
static void test_market_removal_while_a_purchase_is_in_flight() {
    std::printf("the Marketplace: a seller's removal while a part buy is unanswered\n");
    MarketDb db;
    const svc::Character seller = buyer_record();
    db.store_character(seller);
    db.store_character(second_record());
    const std::string listing = db.seed_listing(seller.id, "cyb", kNode, fi::kPotion, 10, kUnitPrice);
    db.hold_buy = true;
    MarketHarness h(seller, &db);
    ready_to_trade(h);
    MarketPeer buyer(h, second_record());
    buyer.play();
    buyer.request(interact(market_component(h.fx.world->registry(), h.manager)), buyer.pawn_ch);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    buyer.client_sends({buyer.client.actor(buyer.pc_ch, purchase_request(market, guid_of_hex(listing), 4), true)});
    CHECK(db.wait_started(3000), "the buyer's part buy reached the database");
    const int64_t potions = potion_units(h.s->inventory());
    size_t from = h.got.size();
    h.client_request(removal_request(market, guid_of_hex(listing)), h.pc_ch);
    const std::vector<repl::RpcCall> refused_calls = pc_calls_since(h, from);
    const repl::RpcCall* refused = removal_answer(refused_calls);
    CHECK(refused && enum_of(*refused, "Response").value_or(0) == 3, "the seller's removal is answered 3 while the part buy is unanswered");
    CHECK(potion_units(h.s->inventory()) == potions && db.remove_calls == 0, "nothing came back to the bags and the database was not asked");
    db.release_all();
    buyer.settle();
    for (int i = 0; i < 400 && h.s->mails().empty(); ++i) h.step();
    CHECK(cached_quantity(h, listing) == 6, "the window shows the 6 units that are left");
    CHECK(h.s->mails().size() == 1 && h.s->mails()[0].money == 400 && h.s->unread_mail_count() == 1, "the seller is told of the sale mail at once: one unread mail, with the 400 copper attached");
    from = h.got.size();
    h.client_request(removal_request(market, guid_of_hex(listing)), h.pc_ch);
    const std::vector<repl::RpcCall> done_calls = pc_calls_since(h, from);
    const repl::RpcCall* done = removal_answer(done_calls);
    CHECK(done && enum_of(*done, "Response").value_or(9) == 0, "a removal after the sale takes the units that are left");
    CHECK(potion_units(h.s->inventory()) == potions + 6 && db.status_of(listing) == market_db::kRemoved, "the 6 units are back in the bags, the listing is REMOVED");
    CHECK(db.sale_mails(seller.id).size() == 1 && db.sale_mails(seller.id)[0].money == 400, "the 400 copper of the 4 units that were sold stay in the seller's mail");
}

// ---- lost answers and a log-out in the middle --------------------------------------------------------------------------------------------

/// The database stores the purchase and its answer is lost: the world asks again with the same request and is answered "replayed": the bags change once, the money is paid once.
static void test_market_lost_answer_is_asked_again() {
    std::printf("the Marketplace: a purchase whose answer was lost is asked again and paid once\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string listing = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
    db.fail_buy_before = 1;
    db.lose_buy_answers = 1;
    MarketHarness h(record, &db);
    ready_to_trade(h);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const size_t from = h.got.size();
    h.client_request(purchase_request(market, guid_of_hex(listing), 3), h.pc_ch);
    const std::vector<repl::RpcCall> answer_calls = pc_calls_since(h, from);
    const repl::RpcCall* answer = purchase_answer(answer_calls);
    CHECK(answer && enum_of(*answer, "Response").value_or(9) == 0, "the purchase is answered 0 at last");
    CHECK(db.buy_calls == 3, "the database was asked three times: one that never arrived, one stored with its answer lost, and the one that was answered");
    bool one_id = db.buys.size() == 3;
    for (const svc::BuyRequest& r : db.buys) {
        one_id = one_id && r.purchase_id == db.buys.front().purchase_id && r.bags.items == db.buys.front().bags.items && r.bags.wallet == db.buys.front().bags.wallet;
    }
    CHECK(one_id, "every ask was the same request: the same id, the same bags");
    CHECK(h.s->wallet().copper() == kStartPurse - 450 && db.copper_of(record.id) == kStartPurse - 450, "the purse paid 300 and 150 tax once, here and in the database");
    CHECK(potion_units(h.s->inventory()) == 5 + 3 && db.units_of(record.id, fi::kPotion) == 5 + 3, "the 3 units are in the bags once");
    CHECK(db.sale_mails(kSeller).size() == 1 && db.sale_mails(kSeller)[0].money == 300 && db.collected_of(kNode) == 150, "the seller's mail and the node's total hold one sale");
    CHECK(cached_quantity(h, listing) == 7, "the window shows the 7 units that are left");
}

/// A buyer who leaves while the purchase is in flight: the world takes the answer and writes the bags, with the purchase when it was stored and without it when it was refused;
/// the log-in waits for that.
static void test_market_logout_during_a_purchase() {
    std::printf("the Marketplace: a buyer who leaves during a purchase loses nothing and pays once\n");
    {
        MarketDb db;
        const svc::Character record = buyer_record();
        db.store_character(record);
        const std::string listing = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
        db.hold_buy = true;
        MarketHarness h(record, &db);
        ready_to_trade(h);
        const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
        h.client_sends({h.client.actor(h.pc_ch, purchase_request(market, guid_of_hex(listing), 4), true)});
        CHECK(db.wait_started(3000), "the purchase reached the database");
        h.s->finish("the client left");
        h.s.reset();
        CHECK(h.fx.world->character_writes_pending(record.id), "a log-in of the buyer waits: the purchase has no answer yet");
        db.release_all();
        CHECK(tick_world_until(h, [&] { return !h.fx.world->character_writes_pending(record.id); }, 5000), "the world finished the purchase and wrote the bags");
        CHECK(db.units_of(record.id, fi::kPotion) == 5 + 4 && db.copper_of(record.id) == kStartPurse - 600, "the bags hold the 4 units and the purse paid 400 and 200 tax, once");
        CHECK(cached_quantity(h, listing) == 6 && db.quantity_of(listing) == 6 && db.sale_mails(kSeller).size() == 1, "the listing shows 6 units and the seller is mailed");
        MarketPeer again(h, db.stored_character(record));
        again.play();
        CHECK(potion_units(again.s->inventory()) == 5 + 4 && again.s->wallet().copper() == kStartPurse - 600, "the relog reads the bags the purchase made");
        again.s->finish("the client left");
        again.s.reset();
    }
    {
        MarketDb db;
        const svc::Character record = buyer_record();
        db.store_character(record);
        const std::string listing = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
        db.hold_buy = true;
        MarketHarness h(record, &db);
        ready_to_trade(h);
        const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
        h.client_sends({h.client.actor(h.pc_ch, purchase_request(market, guid_of_hex(listing), 4), true)});
        CHECK(db.wait_started(3000), "the second purchase reached the database");
        {
            std::lock_guard<std::mutex> lock(db.mu);
            db.rows[0].status = market_db::kRemoved;   // the seller took it off meanwhile (in another world): the purchase is refused
        }
        h.s->finish("the client left");
        h.s.reset();
        db.release_all();
        CHECK(tick_world_until(h, [&] { return !h.fx.world->character_writes_pending(record.id); }, 5000), "the world finished the refused purchase");
        CHECK(db.units_of(record.id, fi::kPotion) == 5 && db.copper_of(record.id) == kStartPurse, "the bags and the purse are the ones before the purchase: it was refused");
        CHECK(db.sale_mails(kSeller).empty() && db.collected_of(kNode) == 0 && cached_quantity(h, listing) == -1, "no mail, no tax, and the listing is gone from the window");
    }
}

/// A seller who leaves while a removal is in flight: the stack is in the bags the world writes when the removal was stored.
static void test_market_logout_during_a_removal() {
    std::printf("the Marketplace: a seller who leaves during a removal loses nothing\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string listing = db.seed_listing(record.id, "cyb", kNode, fi::kPotion, 8, kUnitPrice);
    db.hold_remove = true;
    MarketHarness h(record, &db);
    ready_to_trade(h);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    h.client_sends({h.client.actor(h.pc_ch, removal_request(market, guid_of_hex(listing)), true)});
    CHECK(db.wait_started(3000), "the removal reached the database");
    h.s->finish("the client left");
    h.s.reset();
    CHECK(h.fx.world->character_writes_pending(record.id), "a log-in of the seller waits");
    db.release_all();
    CHECK(tick_world_until(h, [&] { return !h.fx.world->character_writes_pending(record.id); }, 5000), "the world finished the removal");
    CHECK(db.units_of(record.id, fi::kPotion) == 5 + 8 && db.status_of(listing) == market_db::kRemoved, "the stack is in the stored bags and the listing is REMOVED");
    CHECK(cached_quantity(h, listing) == -1, "the listing left the window");
}

/// The world goes down in the middle of a purchase (its answer lost): what the database holds is all of it or none of it, and a world that starts again reads it.
static void test_market_restart_in_the_middle_of_a_purchase() {
    std::printf("the Marketplace: a restart in the middle of a purchase commits all or nothing\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string listing = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
    db.lose_all_answers = true;   // the world never hears an answer
    {
        MarketHarness h(record, &db);
        ready_to_trade(h);
        const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
        h.client_sends({h.client.actor(h.pc_ch, purchase_request(market, guid_of_hex(listing), 4), true)});
        CHECK(db.wait_started(3000), "the purchase reached the database");
        CHECK(wait_until_true([&] { return db.collected_of(kNode) > 0; }, 3000), "the database stored it, and the answer is lost");
        for (int i = 0; i < 3; ++i) h.step();
        CHECK(h.s->market_pending() == 1, "the world has no answer");
    }   // the world stops here: the step is undecided
    const bool stored = db.collected_of(kNode) > 0;
    CHECK(stored == (db.quantity_of(listing) == 6) && stored == (db.sale_mails(kSeller).size() == 1) && stored == (db.units_of(record.id, fi::kPotion) == 5 + 4) &&
              stored == (db.copper_of(record.id) == kStartPurse - 600),
          "the listing, the seller's mail, the node's total and the buyer's bags and purse all hold the purchase, or none of them does");
    db.lose_all_answers = false;
    MarketHarness again(db.stored_character(record), &db);
    again.play();
    CHECK(cached_quantity(again, listing) == db.quantity_of(listing), "the world that starts again reads the listing as the database holds it");
    CHECK(potion_units(again.s->inventory()) == db.units_of(record.id, fi::kPotion) && again.s->wallet().copper() == db.copper_of(record.id), "and the buyer logs in with the bags and the purse the database holds");
}

// ---- the expiry --------------------------------------------------------------------------------------------------------------------------

/// A listing past its time is expired by the world's sweep: its seller is mailed the stack and the unread count, a buy or a removal of it is answered 5 and sweeps it at once.
static void test_market_expiry_mails_the_stack_back() {
    std::printf("the Marketplace: an expired listing comes back to its seller by mail\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    db.seed_listing(record.id, "cyb", kNode, fi::kPotion, 4, kUnitPrice, /*expires_in_ms=*/-5000, /*issued_ago_ms=*/72ll * 3600 * 1000 + 5000);
    const std::string live = db.seed_listing(record.id, "cyb", kNode, fi::kPotion, 2, kUnitPrice);
    MarketHarness h(record, &db);   // the world reads the rows at its start: the first sweep expires what ended while it was down
    ready_to_trade(h);
    CHECK(h.fx.world->market_listings()->size() == 2, "the world holds both listings");
    CHECK(tick_world_until(h, [&] { return h.fx.world->market_listings()->size() == 1 && !db.mails_of(record.id).empty(); }, 5000), "the sweep takes the expired listing out of the window");
    CHECK(cached_quantity(h, live) == 2, "and leaves the other");
    const std::vector<svc::Mail> mails = db.mails_of(record.id);
    CHECK(mails.size() == 1 && mails[0].sender == "Marketplace" && mails[0].subject == "Listing expired" && mails[0].items.size() == 1 && mails[0].items[0].item.quantity == 4 &&
              mails[0].items[0].item.item_record_id == fi::kPotion,
          "the seller has a mail with the 4 units attached");
    CHECK(db.expire_calls == 1, "one call, on the worker");
    for (int i = 0; i < 400 && h.s->mails().empty(); ++i) h.step();
    CHECK(h.s->mails().size() == 1 && h.s->unread_mail_count() == 1, "the seller is online: the mail and the unread count reached the session at once");

    // a purchase or a removal of a listing past its time is answered 5 and the listing is swept at once
    const std::string late = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 3, kUnitPrice, -1000, 72ll * 3600 * 1000 + 1000);
    const std::string late2 = db.seed_listing(record.id, "cyb", kNode, fi::kPotion, 3, kUnitPrice, -1000, 72ll * 3600 * 1000 + 1000);
    MarketHarness restarted(db.stored_character(record), &db);
    restarted.play();
    restarted.run(milliseconds(1500));
    restarted.open_market(restarted.manager);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    CHECK(cached_quantity(restarted, late) == 3, "the world holds the listing that ended while it was down");
    const BuyerState before = state_of(restarted);
    size_t from = restarted.got.size();
    restarted.client_request(purchase_request(market, guid_of_hex(late), 1), restarted.pc_ch);
    const std::vector<repl::RpcCall> answer_calls = pc_calls_since(restarted, from);
    const repl::RpcCall* answer = purchase_answer(answer_calls);
    CHECK(answer && enum_of(*answer, "Response").value_or(0) == 5 && state_of(restarted) == before, "a purchase of a listing past its time is answered 5 and changes nothing");
    from = restarted.got.size();
    restarted.client_request(removal_request(market, guid_of_hex(late2)), restarted.pc_ch);
    const std::vector<repl::RpcCall> removal_calls = pc_calls_since(restarted, from);
    const repl::RpcCall* removal = removal_answer(removal_calls);
    CHECK(removal && enum_of(*removal, "Response").value_or(0) == 5 && state_of(restarted) == before, "a removal of one is answered 5 as well: it comes back by mail, not through Cancel Listing");
    CHECK(tick_world_until(restarted, [&] { return db.status_of(late) == market_db::kExpiredClaimed && db.status_of(late2) == market_db::kExpiredClaimed; }, 5000), "both were swept at once");
    CHECK(tick_world_until(restarted, [&] { return cached_quantity(restarted, late) == -1 && cached_quantity(restarted, late2) == -1; }, 5000), "and left the window");
    CHECK(db.sale_mails(kSeller).empty() && db.mails_of(kSeller).size() == 1, "the seller of the first is mailed the stack back, not paid");
}

// ---- the seller's money --------------------------------------------------------------------------------------------------------------------

/// A mail of the Marketplace that carries `money` copper, in the database.
static std::string add_sale_mail(MarketDb& db, const std::string& owner, int64_t money) {
    market_db::MailRow m;
    m.owner = owner;
    m.mail.id = db.fresh();
    m.mail.sender = "Marketplace";
    m.mail.subject = "Item sold";
    m.mail.body = "Something sold.";
    m.mail.money = money;
    m.mail.issued_at_ms = market_db::wall_ms();
    db.mails.push_back(m);
    return m.mail.id;
}

/// ServerClaimMailCurrency(MailboxGuid, MailGuid) on the mail's social component.
static BunchWriter claim_currency_request(const Harness& h, const std::string& mail_id) {
    const schema::RpcSchema* rpc = class_named("UIntrepidSocialComponent").rpc("ServerClaimMailCurrency");
    const auto mail = items::guid_of(mail_id);
    repl::RpcParams params;
    if (!rpc || !mail || !put_guid(*rpc, params, "MailboxGuid", repl::GuidValue{5100u, 0u, 0u, 0u}) || !put_guid(*rpc, params, "MailGuid", *mail)) return BunchWriter();
    return social_rpc(h, "ServerClaimMailCurrency", params);
}

/// ServerClaimAllMail(MailboxGuid, MailGuids).
static BunchWriter claim_all_request(const Harness& h, const std::vector<std::string>& ids) {
    const schema::RpcSchema* rpc = class_named("UIntrepidSocialComponent").rpc("ServerClaimAllMail");
    repl::RpcParams params;
    if (!rpc || !put_guid(*rpc, params, "MailboxGuid", repl::GuidValue{5100u, 0u, 0u, 0u})) return BunchWriter();
    repl::ArrayValue list;
    for (const std::string& id : ids) {
        const auto guid = items::guid_of(id);
        if (!guid) return BunchWriter();
        repl::ElementState element;
        element.values[1] = repl::Value::i64(static_cast<int64_t>(guid->a));
        element.values[2] = repl::Value::i64(static_cast<int64_t>(guid->b));
        element.values[3] = repl::Value::i64(static_cast<int64_t>(guid->c));
        element.values[4] = repl::Value::i64(static_cast<int64_t>(guid->d));
        list.elements.push_back(std::move(element));
    }
    if (!repl::set_rpc_param(*rpc, params, "MailGuids", repl::Value(std::move(list)))) return BunchWriter();
    return social_rpc(h, "ServerClaimAllMail", params);
}

static const svc::Mail* mail_in_cache(MarketHarness& h, const std::string& id) {
    for (const svc::Mail& m : h.s->mails()) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

/// The seller takes the money of a sale mail into the purse, once: the claim by the mail, the claim-all, and a second claim of the same money.
static void test_market_the_seller_takes_the_money_by_mail() {
    std::printf("the Marketplace: the seller takes the sale's money from the mail into the purse\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string first = add_sale_mail(db, record.id, 700), second = add_sale_mail(db, record.id, 600), third = add_sale_mail(db, record.id, 50);
    MarketHarness h(record, &db);
    ready_to_trade(h);
    h.client_request(actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerFetchMail", {}), h.pc_ch);
    CHECK(h.s->mail_loaded() && h.s->mails().size() == 3, "the mails are loaded");
    const svc::Mail* m = mail_in_cache(h, first);
    CHECK(m && m->money == 700 && !m->money_claimed, "the sale mail carries 700 copper, not claimed");
    const int64_t purse = h.s->wallet().copper();
    size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, claim_currency_request(h, first), true)});
    h.settle();
    CHECK(h.s->wallet().copper() == purse + 700 && db.copper_of(record.id) == purse + 700, "the purse holds the 700 copper, here and in the database");
    m = mail_in_cache(h, first);
    CHECK(m && m->money_claimed, "the mail shows the money claimed");
    const std::vector<repl::RpcCall> callout_calls = pc_calls_since(h, from);
    const repl::RpcCall* callout = currency_callout(callout_calls);
    CHECK(callout != nullptr, "the money popup shows it");
    // the same claim again, and another's, change nothing
    const int64_t claims = db.claim_calls;
    from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, claim_currency_request(h, first), true)});
    h.client_sends({h.client.actor(h.pc_ch, claim_currency_request(h, guid_n(0xbeef)), true)});
    h.settle();
    CHECK(h.s->wallet().copper() == purse + 700 && db.claim_calls == claims && currency_callout(pc_calls_since(h, from)) == nullptr, "a second claim of the same money, and a claim of a mail that is not there, change nothing");
    // claim-all takes the money of the mails that still have some, once
    h.client_sends({h.client.actor(h.pc_ch, claim_all_request(h, {first, second, third}), true)});
    h.settle();
    CHECK(h.s->wallet().copper() == purse + 700 + 600 + 50 && db.copper_of(record.id) == purse + 1350, "claim-all took the money of the other two mails, and not the first one's again");
    CHECK(db.mails_of(record.id).size() == 3 && db.sale_mails(record.id).size() == 3, "the mails are still there");
    // the relog reads the purse with the money
    h.s->finish("the client left");
    h.s.reset();
    MarketPeer again(h, db.stored_character(record));
    again.play();
    CHECK(again.s->wallet().copper() == purse + 1350, "after the relog the purse is the same");
}

/// A purse that would pass its cap refuses the claim, and the money stays in the mail.
static void test_market_a_full_purse_refuses_the_money() {
    std::printf("the Marketplace: a purse that would pass its cap refuses the money\n");
    MarketDb db;
    svc::Character record = buyer_record();
    record.wallet = {{items::kCopperCurrency, items::kMoneyCap - 100}};
    db.store_character(record);
    const std::string mail = add_sale_mail(db, record.id, 700);
    MarketHarness h(record, &db);
    ready_to_trade(h);
    h.client_request(actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerFetchMail", {}), h.pc_ch);
    const int64_t purse = h.s->wallet().copper();
    CHECK(purse == items::kMoneyCap - 100, "the purse is 100 copper from its cap");
    h.client_sends({h.client.actor(h.pc_ch, claim_currency_request(h, mail), true)});
    h.settle();
    const svc::Mail* m = mail_in_cache(h, mail);
    CHECK(h.s->wallet().copper() == purse && m && !m->money_claimed && db.claim_calls == 0, "the purse is the same, the money is still in the mail and the database was not asked");
    h.client_sends({h.client.actor(h.pc_ch, claim_all_request(h, {mail}), true)});
    h.settle();
    CHECK(h.s->wallet().copper() == purse && db.claim_calls == 0, "claim-all refuses it as well");
}

/// A money claim the database refuses (the money was taken in another session) gives the money back; a session that ends with the claim in flight hands it to the world, which
/// writes the purse the answer decides.
static void test_market_money_claim_refused_and_handed_over() {
    std::printf("the Marketplace: a money claim that is refused gives the money back; a logout hands it over\n");
    {
        MarketDb db;
        const svc::Character record = buyer_record();
        db.store_character(record);
        const std::string mail = add_sale_mail(db, record.id, 700);
        MarketHarness h(record, &db);
        ready_to_trade(h);
        h.client_request(actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerFetchMail", {}), h.pc_ch);
        {
            std::lock_guard<std::mutex> lock(db.mu);
            db.mails[0].mail.money_claimed = true;   // another session of the character took it meanwhile
            db.bags[record.id] = h.s->inventory().items();
        }
        const int64_t purse = h.s->wallet().copper();
        h.client_sends({h.client.actor(h.pc_ch, claim_currency_request(h, mail), true)});
        h.settle();
        CHECK(h.s->wallet().copper() == purse && db.copper_of(record.id) == purse, "the claim was refused: the purse is back to what it was, here and in the database");
    }
    {
        MarketDb db;
        const svc::Character record = buyer_record();
        db.store_character(record);
        const std::string mail = add_sale_mail(db, record.id, 700);
        db.hold_claim = true;
        MarketHarness h(record, &db);
        ready_to_trade(h);
        h.client_request(actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerFetchMail", {}), h.pc_ch);
        const int64_t purse = h.s->wallet().copper();
        h.client_sends({h.client.actor(h.pc_ch, claim_currency_request(h, mail), true)});
        CHECK(db.wait_started(3000), "the claim reached the database");
        CHECK(h.s->wallet().copper() == purse + 700, "the purse holds the money while the claim is in flight");
        h.s->finish("the client left");
        h.s.reset();
        CHECK(h.fx.world->character_writes_pending(record.id), "a log-in of the seller waits for the claim");
        db.release_all();
        CHECK(wait_until_true([&] { return !h.fx.world->character_writes_pending(record.id); }, 5000), "the claim was answered and the bags were written");
        CHECK(db.copper_of(record.id) == purse + 700 && db.sale_mails(record.id).size() == 1 && db.sale_mails(record.id)[0].money_claimed, "the purse in the database holds the money and the mail shows it claimed");
    }
}

/// A step and a mail claim of one character write the same bags: a purchase is answered 3 while a claim is in flight, and a claim made while a purchase is in flight is not made.
static void test_market_step_and_mail_claim_wait_for_each_other() {
    std::printf("the Marketplace: a purchase and a mail claim of one character never overlap\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string listing = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
    const std::string mail_id = guid_n(9001), attachment = guid_n(9101);
    for (const std::pair<std::string, std::string>& m : {std::pair<std::string, std::string>{mail_id, attachment}, std::pair<std::string, std::string>{guid_n(9002), guid_n(9102)}}) {
        market_db::MailRow row;
        row.owner = record.id;
        row.mail = potion_mail(m.first, m.second);
        db.mails.push_back(row);
    }
    db.hold_claim = true;
    MarketHarness h(record, &db);
    ready_to_trade(h);
    h.client_request(actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerFetchMail", {}), h.pc_ch);
    CHECK(h.s->mail_loaded(), "the mails are loaded");
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    h.client_sends({h.client.actor(h.pc_ch, claim_request(h, mail_id), true)});
    CHECK(db.wait_started(3000), "the claim is in flight");
    const int64_t purse = h.s->wallet().copper();
    size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, purchase_request(market, guid_of_hex(listing), 2), true)});
    for (int i = 0; i < 5; ++i) h.step();
    const std::vector<repl::RpcCall> refused_calls = pc_calls_since(h, from);
    const repl::RpcCall* refused = purchase_answer(refused_calls);
    CHECK(refused && enum_of(*refused, "Response").value_or(0) == 3, "a purchase while a claim is in flight is answered 3");
    CHECK(h.s->wallet().copper() == purse && db.buy_calls == 0, "it took nothing and never reached the database");
    db.release_all();
    h.settle();
    CHECK(potion_units(h.s->inventory()) == 5 + 1, "the claim landed: the mailed potion is in the bags");

    // the other way round: a claim made while a purchase is in flight is not made
    db.hold_buy = true;
    db.reset_started();
    h.client_sends({h.client.actor(h.pc_ch, purchase_request(market, guid_of_hex(listing), 2), true)});
    CHECK(db.wait_started(3000), "the purchase is in flight");
    const int claims = db.claim_calls;
    h.client_sends({h.client.actor(h.pc_ch, claim_request(h, guid_n(9002)), true)});
    for (int i = 0; i < 5; ++i) h.step();
    CHECK(db.claim_calls == claims, "a claim made while the purchase is in flight is not made");
    db.release_all();
    h.settle();
    CHECK(h.s->wallet().copper() == purse - 300 && potion_units(h.s->inventory()) == 5 + 1 + 2, "the purchase landed: 200 and 100 tax paid, 2 units in the bags");
}

/// A listing whose time comes while a purchase of it is in flight: the sweep leaves it to that step, the database refuses the purchase (its time has come), the buyer's bags
/// and purse go back, and the listing is then mailed back to its seller.
static void test_market_the_sweep_waits_for_a_step_in_flight() {
    std::printf("the Marketplace: the expiry sweep never overtakes a step of the listing\n");
    MarketDb db;
    const svc::Character record = buyer_record();
    db.store_character(record);
    const std::string listing = db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 10, kUnitPrice);
    db.hold_buy = true;
    MarketHarness h(record, &db);
    ready_to_trade(h);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    h.client_sends({h.client.actor(h.pc_ch, purchase_request(market, guid_of_hex(listing), 3), true)});
    CHECK(db.wait_started(3000), "the purchase is in flight, before the listing's time");
    {
        std::lock_guard<std::mutex> lock(db.mu);
        db.rows[0].l.expiry_date_ms = market_db::wall_ms() - 1;   // the database's clock passes the listing's time while the step waits
    }
    h.fx.world->tick_world(world_time_at(h.t) + 300000.0);   // (and the world's clock is well past it: the sweep looks at the listing)
    {
        std::lock_guard<std::mutex> lock(db.mu);
        CHECK(db.expire_calls == 0, "the sweep did not ask for the expiry: a step of the listing is in flight");
    }
    db.release_all();
    h.settle();
    CHECK(h.s->wallet().copper() == kStartPurse && potion_units(h.s->inventory()) == 5, "the database refused the purchase (its time had come): the bags and the purse are as they were");
    CHECK(db.sale_mails(kSeller).empty() && db.collected_of(kNode) == 0, "no sale mail and no tax");
    CHECK(tick_world_until(h, [&] { return db.status_of(listing) == market_db::kExpiredClaimed; }, 5000), "the refused purchase swept the listing at once: it is mailed back to its seller");
    CHECK(db.mails_of(kSeller).size() == 1 && db.mails_of(kSeller)[0].items.size() == 1 && db.mails_of(kSeller)[0].items[0].item.quantity == 10, "the seller has the 10 units by mail");
}

/// The memory index of listings takes a listing out, shrinks one and lists the ones that are due.
static void test_market_memory_store_follows_the_sales() {
    std::printf("the Marketplace: the listings' memory index follows a sale, a removal and an expiry\n");
    ::aoc::worldd::market::MemoryMarketStore store;
    const auto make = [](uint32_t n, int64_t node, uint32_t seller, int32_t quantity, double expires) {
        ::aoc::worldd::market::Listing l;
        l.node = node;
        l.seller = repl::GuidValue{seller, 2, 3, 4};
        l.seller_name = "s" + std::to_string(seller);
        l.guid = repl::GuidValue{n, 9, 9, 9};
        l.item.item_record_id = fi::kPotion;
        l.item.instance_guid = guid_n(n);
        l.item.quantity = quantity;
        l.price_per_unit = 100;
        l.issue_date = 10.0 + n;
        l.expiration_date = expires;
        return l;
    };
    store.add(make(1, kNode, 1, 10, 100.0));
    store.add(make(2, kNode, 1, 5, 200.0));
    store.add(make(3, kNode, 2, 7, 150.0));
    store.add(make(4, kNode + 1, 2, 1, 120.0));
    CHECK(store.size() == 4 && store.count(repl::GuidValue{1, 2, 3, 4}) == 2 && store.storefronts(kNode).size() == 2, "four listings, two at the market of the node");
    CHECK(store.set_quantity(repl::GuidValue{1, 9, 9, 9}, 4) && store.find(repl::GuidValue{1, 9, 9, 9})->item.quantity == 4, "a part sale leaves its units");
    CHECK(!store.set_quantity(repl::GuidValue{1, 9, 9, 9}, 0) && !store.set_quantity(repl::GuidValue{77, 9, 9, 9}, 3), "no units, and a listing that is not there, are not set");
    const std::vector<::aoc::worldd::market::Listing> due = store.expired(160.0, 10);
    CHECK(due.size() == 3 && due[0].guid.a == 1 && due[1].guid.a == 3 && due[2].guid.a == 4, "the three listings past 160 seconds are due, oldest first");
    CHECK(store.expired(160.0, 2).size() == 2, "the sweep takes a batch at most");
    CHECK(store.remove(repl::GuidValue{3, 9, 9, 9}) && store.find(repl::GuidValue{3, 9, 9, 9}) == nullptr && store.size() == 3, "a sold listing leaves");
    CHECK(!store.remove(repl::GuidValue{3, 9, 9, 9}), "it leaves once");
    CHECK(store.count(repl::GuidValue{2, 2, 3, 4}) == 1 && store.listings(kNode, repl::GuidValue{1, 2, 3, 4}).size() == 2 && store.listings(kNode, repl::GuidValue{2, 2, 3, 4}).empty(), "the indexes follow");
    CHECK(store.find(repl::GuidValue{1, 9, 9, 9}) && store.find(repl::GuidValue{2, 9, 9, 9}) && store.find(repl::GuidValue{4, 9, 9, 9}), "the others are where they were");
    CHECK(store.storefronts(kNode).size() == 1 && store.storefronts(kNode + 1).size() == 1, "and the storefronts");
}

/// A listing as FMarketplaceItemData carries it, read from element `e` of a data answer.
static repl::MarketListing listing_of_element(const repl::RpcCall& c, const repl::ElementState& e) {
    const char* array = "MarketplaceItemData";
    repl::MarketListing l;
    l.node = member_number(c, array, e, "MarketplaceItemData.NodeGuid").value_or(0);
    l.seller = repl::GuidValue{member_word(c, array, e, "MarketplaceItemData.PlayerGuid.A").value_or(0), member_word(c, array, e, "MarketplaceItemData.PlayerGuid.B").value_or(0),
                               member_word(c, array, e, "MarketplaceItemData.PlayerGuid.C").value_or(0), member_word(c, array, e, "MarketplaceItemData.PlayerGuid.D").value_or(0)};
    l.seller_name = member_text(c, array, e, "MarketplaceItemData.PlayerName");
    l.guid = repl::GuidValue{member_word(c, array, e, "MarketplaceItemData.MarketplaceItemGuid.A").value_or(0), member_word(c, array, e, "MarketplaceItemData.MarketplaceItemGuid.B").value_or(0),
                             member_word(c, array, e, "MarketplaceItemData.MarketplaceItemGuid.C").value_or(0), member_word(c, array, e, "MarketplaceItemData.MarketplaceItemGuid.D").value_or(0)};
    if (const auto* item = repl::rpcf::as<repl::ItemInstance>(member_of(c, array, e, "MarketplaceItemData.Item"))) l.item = *item;
    l.item_name = member_text(c, array, e, "MarketplaceItemData.ItemName");
    if (const auto* price = repl::rpcf::as<repl::CurrencyContainer>(member_of(c, array, e, "MarketplaceItemData.PricePerUnit"))) l.price = *price;
    if (const auto* issue = repl::rpcf::as<double>(member_of(c, array, e, "MarketplaceItemData.IssueDate"))) l.issue_date = *issue;
    if (const auto* expiry = repl::rpcf::as<double>(member_of(c, array, e, "MarketplaceItemData.ExpirationDate"))) l.expiration_date = *expiry;
    l.status = static_cast<uint8_t>(member_enum(c, array, e, "MarketplaceItemData.Status").value_or(99));
    return l;
}

/// What the reader made of a recorded call, in words.
static std::string describe_call(const char* what, const std::optional<repl::RpcCall>& c) {
    if (!c) return std::string(what) + ": no call";
    return std::string(what) + ": " + (c->rpc ? c->rpc->name : "?") + (c->decoded ? ", decoded, " : ", NOT decoded, ") + std::to_string(c->param_bits) + " parameter bits";
}

/// The data answers retail sent before and after the first of the recorded buys (one unit of a listing that held two).  Joined, the three pieces of each are an answer of 22,687 bits, and
/// the two answers differ in exactly two neighbouring bits of that listing's units (a packed 2 becoming 1): the answer after a part sale keeps the listing, in the same place and
/// the same form, with the units that are left and nothing else changed.  Where the reader can read the joined answers, the listing's units are the units before minus the one
/// bought, every other listing is as it was, and the answer our builder makes from the decoded listings is the recorded one.
static void test_market_recorded_data_answer_after_a_part_sale() {
    std::printf("the Marketplace: the recorded data answer after a part sale is the one our layout builds\n");
    const BunchWriter a_bits = recorded_chain({kDataFirstPiece, kDataBeforeMiddle, kDataLastPiece});
    const BunchWriter b_bits = recorded_chain({kDataFirstPiece, kDataAfterMiddle, kDataLastPiece});
    std::vector<size_t> differing;
    for (size_t i = 0; i < a_bits.bit_pos() && i < b_bits.bit_pos(); ++i) {
        if (((a_bits.data()[i / 8] >> (i % 8)) & 1) != ((b_bits.data()[i / 8] >> (i % 8)) & 1)) differing.push_back(i);
    }
    CHECK(a_bits.bit_pos() == 22687 && b_bits.bit_pos() == 22687, "each answer is the three pieces joined: 22,687 bits");
    CHECK(differing.size() == 2 && differing[1] == differing[0] + 1 && differing[0] >= kDataFirstPiece.bits && differing[1] < kDataFirstPiece.bits + kDataBeforeMiddle.bits,
          ("the two recorded answers differ in two neighbouring bits of the middle piece: " + std::to_string(differing.size()) + " bits differ").c_str());
    const std::optional<repl::RpcCall> before = call_in(a_bits, true);
    const std::optional<repl::RpcCall> after = call_in(b_bits, true);
    const std::optional<repl::RpcCall> bought = recorded_call(kBuy1Request, false);
    CHECK(bought && bought->decoded, "the first buy's request reads");
    if (!bought || !bought->decoded) return;
    if (!before || !after || !before->decoded || !after->decoded) {
        std::printf("  [note] the reader does not decode the recorded data answers (%s; %s): they are compared bit for bit only\n", describe_call("before", before).c_str(),
                    describe_call("after", after).c_str());
        return;
    }
    const std::optional<repl::MarketPurchase> purchase = repl::parse_market_purchase(*bought);
    const repl::ArrayValue* a = array_of(*before, "MarketplaceItemData");
    const repl::ArrayValue* b = array_of(*after, "MarketplaceItemData");
    CHECK(purchase && a && b && a->elements.size() == b->elements.size() && a->elements.size() >= 2, "both answers hold the same number of listings");
    if (!purchase || !a || !b || a->elements.size() != b->elements.size()) return;
    int found = 0;
    bool others_same = true;
    for (size_t i = 0; i < a->elements.size(); ++i) {
        const repl::MarketListing x = listing_of_element(*before, a->elements[i]), y = listing_of_element(*after, b->elements[i]);
        if (x.guid == purchase->listing) {
            ++found;
            repl::MarketListing y_with_before_units = y;
            y_with_before_units.item.quantity = x.item.quantity;
            CHECK(x.item.quantity == y.item.quantity + static_cast<uint32_t>(purchase->quantity), "the bought listing shows the units that are left: the answer's units before minus the units bought");
            CHECK(y_with_before_units == x && y.status == 0 && x.status == 0, "nothing else of it changed: the same guid, price, dates and status LISTED");
        } else {
            others_same = others_same && a->elements[i] == b->elements[i];
        }
    }
    CHECK(found == 1 && others_same, "the listing is in the answer once, and every other listing is exactly as it was");
    std::vector<repl::MarketListing> listings;
    for (const repl::ElementState& e : b->elements) listings.push_back(listing_of_element(*after, e));
    const repl::GuidValue seller{word_of(*after, "PlayerGuid.A").value_or(0), word_of(*after, "PlayerGuid.B").value_or(0), word_of(*after, "PlayerGuid.C").value_or(0), word_of(*after, "PlayerGuid.D").value_or(0)};
    const std::optional<repl::RpcParams> built = repl::market_data_params(*after->rpc, seller, listings);
    CHECK(built && params_write_same_bits(*after->rpc, *built, after->params), "the answer our builder makes from the decoded listings is the recorded one after the sale, bit for bit");
}

AOC_REGISTER_TEST(test_market_tax_is_the_clients_float_formula);
AOC_REGISTER_TEST(test_market_recorded_requests_and_answers);
AOC_REGISTER_TEST(test_market_goal_buy_all_or_part_of_a_stack);
AOC_REGISTER_TEST(test_market_goal_full_bags_refuse_the_buy);
AOC_REGISTER_TEST(test_market_goal_removal_gives_the_stack_back);
AOC_REGISTER_TEST(test_market_refusals_change_nothing);
AOC_REGISTER_TEST(test_market_one_step_in_flight);
AOC_REGISTER_TEST(test_market_two_buyers_of_one_listing);
AOC_REGISTER_TEST(test_market_removal_while_a_purchase_is_in_flight);
AOC_REGISTER_TEST(test_market_lost_answer_is_asked_again);
AOC_REGISTER_TEST(test_market_logout_during_a_purchase);
AOC_REGISTER_TEST(test_market_logout_during_a_removal);
AOC_REGISTER_TEST(test_market_restart_in_the_middle_of_a_purchase);
AOC_REGISTER_TEST(test_market_expiry_mails_the_stack_back);
AOC_REGISTER_TEST(test_market_the_seller_takes_the_money_by_mail);
AOC_REGISTER_TEST(test_market_a_full_purse_refuses_the_money);
AOC_REGISTER_TEST(test_market_money_claim_refused_and_handed_over);
AOC_REGISTER_TEST(test_market_step_and_mail_claim_wait_for_each_other);
AOC_REGISTER_TEST(test_market_the_sweep_waits_for_a_step_in_flight);
AOC_REGISTER_TEST(test_market_memory_store_follows_the_sales);
AOC_REGISTER_TEST(test_market_recorded_data_answer_after_a_part_sale);
