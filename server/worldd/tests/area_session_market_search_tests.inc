// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/area_session_market_search_tests.inc: the search and the storefront descriptions through real sessions
//  (registered with AOC_REGISTER_TEST: test_worldd_area_session.cpp runs them through aoc_tests::run; included from area_session_registered.inc)
//
//  Every test sends the client's own call and reads the client's own answers: ClientReceiveMarketplaceHeaders pages (the text and the rarity echoed, bComplete on the last page, the
//  headers' names and descriptions, the version) and ClientMarketplaceHeaderUpdateResponse (the text and the response), on the accepted and on the refused path.
//
//    the recording    the two recorded searches ("wand", "ruby" with rarity 0) and the recorded empty search (answered 16): the requests the tests build are the recorded ones bit for
//                     bit, and the answers our server sends are the recorded answers' fields from the recorded listings
//    the search       the item's display name, any case, anywhere in it; the rarity exactly (7: any); the searcher's own storefront left out; another market's listings left out;
//                     texts of 0, 1, 50 and 51 characters; 250 matching storefronts as three pages; one a second; nothing written
//    the rarity       a listing's rarity is its record's at the stack's quality, read from the saved rows and made when the listing is
//    the edit         0, 14, 15 and 1; the text stored as sent and answered 0; the next pages (the editor's refresh, a viewer's open, refresh and search) carry the new text and
//                     the raised version; an empty text puts the default back; per market; a restart keeps it; edits in a line, a bound, a log-out mid-edit, a service that is down
//                     or whose answer is lost
//    the words        the bad words come from content.db's list (read at test time) and from a list the tests build; matching is any case and anywhere in the text
// ============================================================================
#include "market/market.h"
#include "market/storefront_text.h"
#include "protocol/repl/market_rpc.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace market_search_test {

/// The recorded searches of retail's Marketplace (the 'wand' search, the 'ruby' search and the empty one): the client's bunches (the 'wand' request is sent twice, with the same bits) and the
/// server's answers.  The guids of the requests are the recording's: the tests rewrite them.
struct RecordedSearch {
    const char* hex;
    size_t      bits;
};
constexpr RecordedSearch kSearchEmptyRequest{"0e0900fd20709a6b14a59aa9fc75a68a4fb7689d36c003", 179};
constexpr RecordedSearch kSearchWandRequest{"4e0b00fd29709a6b14a59aa9fc75a68a4fb7689d3660010000c05d981b19c003", 251};
constexpr RecordedSearch kSearchRubyRequest{"2e0b007d291075f48e7d2f682ee49b6e26f471b51f78010000805c9d581e00", 247};
constexpr RecordedSearch kSearchRefusedAnswer{"cacce22004", 35};
constexpr RecordedSearch kSearchWandAnswer{"ae2b8c73ab10cf8a96045a0c006000009e152d09b4180000609802c56cd3fe8dd173b729a2bc497e01000040915c5a1980060000805c18595a981b1dc85d5918dc9bdb1c48981b1948985cdb9b1c4010df65a3346c59100b000000eec2dcc8005e", 775};
constexpr RecordedSearch kSearchRubyAnswer{"56538c134a31cfcaae045a0c00a000409e955d09b41800c0e3ac3187c5d372c1404114ee57c052840200008051195bd8599d1b5918c00500008051195bd8599d1b59d8c91cc814dd9b5c9999dc9b1b1d00ffba3475f53a5a903c2bbb1268310080e802227170a0bd08d050d6d1cd65bd0c06000000a43436b6bb3436b6b4b036800c000000a43436b6bb3436b6b4b0b693399029ba37b93233b937373a008759a1a1fe19b52016000000c8d589e50108", 1404};

inline constexpr int64_t kThirdMarket = kNode + 2;

/// One header of a page, as the client reads it.
struct HeaderSeen {
    std::string name;
    std::string description;
};

/// One ClientReceiveMarketplaceHeaders page, as the client reads it.
struct PageRead {
    std::vector<HeaderSeen> headers;
    bool        complete = false;
    int64_t     node = 0;
    uint64_t    version = 0;
    uint64_t    rarity = 99;
    std::string search;
};

/// One ClientMarketplaceHeaderUpdateResponse, as the client reads it.
struct UpdateRead {
    std::string text;
    uint64_t    response = 99;
};

inline std::vector<PageRead> pages_in(const std::vector<repl::RpcCall>& calls) {
    std::vector<PageRead> out;
    for (const repl::RpcCall& c : calls) {
        if (!c.rpc || !c.decoded || std::string(c.rpc->name) != "ClientReceiveMarketplaceHeaders") continue;
        PageRead p;
        if (const repl::ArrayValue* list = array_of(c, "Headers")) {
            for (const repl::ElementState& e : list->elements) p.headers.push_back({member_text(c, "Headers", e, "Headers.PlayerName"), member_text(c, "Headers", e, "Headers.Description")});
        }
        const auto* complete = repl::rpcf::as<bool>(repl::rpcf::value_of(*c.rpc, c.params, "bComplete"));
        p.complete = complete && *complete;
        p.node = number_of(c, "NodeId").value_or(0);
        p.version = enum_of(c, "HeaderVersion").value_or(0);
        p.rarity = enum_of(c, "ItemRarity").value_or(0);
        p.search = text_of(c, "SearchItemString");
        out.push_back(std::move(p));
    }
    return out;
}

inline std::vector<UpdateRead> updates_in(const std::vector<repl::RpcCall>& calls) {
    std::vector<UpdateRead> out;
    for (const repl::RpcCall& c : calls) {
        if (!c.rpc || !c.decoded || std::string(c.rpc->name) != "ClientMarketplaceHeaderUpdateResponse") continue;
        out.push_back({text_of(c, "HeaderDescription"), enum_of(c, "Response").value_or(0)});
    }
    return out;
}

inline bool has_header(const PageRead& p, const std::string& name, const std::string& description) {
    return std::any_of(p.headers.begin(), p.headers.end(), [&](const HeaderSeen& h) { return h.name == name && h.description == description; });
}

inline size_t header_count(const std::vector<PageRead>& pages) {
    size_t n = 0;
    for (const PageRead& p : pages) n += p.headers.size();
    return n;
}

/// A listing of the stranger `n` at `node`: the item name the client shows, and its rarity.  `k` tells one listing of a seller from another (the listing's own guid).
inline market::Listing shop_listing(uint32_t n, const std::string& seller_name, const std::string& item_name, uint8_t rarity = 0, int64_t node = kNode, uint32_t k = 0) {
    market::Listing l = stranger_listing(n);
    l.node = node;
    l.seller_name = seller_name;
    l.item_name = item_name;
    l.rarity = rarity;
    l.guid.d = 13u + k;
    return l;
}

}  // namespace market_search_test

using namespace market_search_test;

/// ServerSearchMarketplaceHeaders as the Buy tab sends it: an empty text and the rarity 0 are left off the wire.
static BunchWriter search_request(const repl::GuidValue& market, const std::string& text, uint64_t rarity) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerSearchMarketplaceHeaders");
    if (!rpc) return BunchWriter();
    repl::RpcParams params;
    bool ok = put_guid(*rpc, params, "MarketplaceActorGuid", market);
    if (!text.empty()) ok = ok && repl::set_rpc_param(*rpc, params, "SearchItemString", repl::Value::str(text));
    ok = ok && repl::set_rpc_param(*rpc, params, "ItemRarity", repl::Value::u64(rarity));
    return ok ? actor_rpc(pc, "ServerSearchMarketplaceHeaders", params) : BunchWriter();
}

/// ServerRequestMarketplaceHeaderUpdate as the Sell tab sends it.
static BunchWriter header_update_request(const repl::GuidValue& market, const std::string& text) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerRequestMarketplaceHeaderUpdate");
    if (!rpc) return BunchWriter();
    repl::RpcParams params;
    bool ok = put_guid(*rpc, params, "MarketplaceActorGuid", market);
    if (!text.empty()) ok = ok && repl::set_rpc_param(*rpc, params, "HeaderDescription", repl::Value::str(text));
    return ok ? actor_rpc(pc, "ServerRequestMarketplaceHeaderUpdate", params) : BunchWriter();
}

static BunchWriter recorded_search_bunch(const RecordedSearch& r) {
    const std::vector<uint8_t> bytes = recorded_bytes(r.hex);
    BunchWriter w;
    w.write_bit_range(bytes.data(), 0, r.bits);
    return w;
}

static std::optional<repl::RpcCall> recorded_search_call(const RecordedSearch& r, bool server_form) { return call_in(recorded_search_bunch(r), server_form); }

/// One search of `h`, a second after the one before (the server answers one a second): the calls the client heard.
static std::vector<repl::RpcCall> search_calls(MarketHarness& h, const std::string& text, uint64_t rarity, int64_t node = kNode) {
    h.run(milliseconds(1100));
    const size_t from = h.got.size();
    h.client_request(search_request(::aoc::worldd::market::marketplace_guid(node), text, rarity), h.pc_ch);
    return pc_calls_since(h, from);
}

static std::vector<repl::RpcCall> update_calls(MarketHarness& h, const std::string& text, int64_t node = kNode) {
    const size_t from = h.got.size();
    h.client_request(header_update_request(::aoc::worldd::market::marketplace_guid(node), text), h.pc_ch);
    return pc_calls_since(h, from);
}

/// Checks that `calls` hold exactly one answer to an edit: `text` and `response`, and no storefront page.
static void check_one_update(const std::vector<repl::RpcCall>& calls, const std::string& text, uint64_t response, const char* what) {
    const std::vector<UpdateRead> updates = updates_in(calls);
    const std::string w = what;
    CHECK(updates.size() == 1, (w + ": exactly one ClientMarketplaceHeaderUpdateResponse").c_str());
    if (updates.size() == 1) {
        CHECK(updates[0].response == response, (w + ": the response is " + std::to_string(response)).c_str());
        CHECK(updates[0].text == text, (w + ": the text is the one the client shows: \"" + text + "\"").c_str());
    }
    CHECK(count_calls(calls, "ClientReceiveMarketplaceHeaders") == 0, (w + ": no storefront page comes with it").c_str());
}

static std::string seed_cyb_listing(MarketDb& db, int64_t node = kNode, int32_t quality = 1) {
    return db.seed_listing(kCyb, "cyb", node, fi::kPotion, 2, kUnitPrice, 72ll * 3600 * 1000, 1000, quality);
}

/// A harness with a Manager at each of two markets, 8 m either side of the player.
struct TwoMarketHarness : Harness {
    uint64_t manager = 0, other = 0;
    TwoMarketHarness(svc::Character r, PositionSink* sink) : Harness(with_start_purse(std::move(r))) {
        fx.world->set_position_sink(sink);
        manager = add_market_manager(fx, kCx + 800, kCy, "Marketplace Manager");
        other = add_market_manager(fx, kCx - 800, kCy, "Marketplace Manager B", market_sell_test::kOtherNode);
        fx.world->set_gm_vendors(test_vendors());
        start();
    }
    void open_market(uint64_t actor) { client_request(interact(market_component(fx.world->registry(), actor)), pawn_ch); }
};

// ---- the text rules ---------------------------------------------------------------------------------------------------------------------

static void test_market_text_rules() {
    std::printf("the Marketplace: the text rules (UTF-16 length, case, the bad-word list)\n");
    namespace mk = ::aoc::worldd::market;
    CHECK(mk::utf16_length("") == 0u && mk::utf16_length("wand") == 4u && mk::utf16_length(std::string(50, 'x')) == 50u, "an ASCII text is as long as it has characters");
    CHECK(mk::utf16_length("caf\xC3\xA9") == 4u && mk::utf16_length("\xE4\xBD\xA0\xE5\xA5\xBD") == 2u, "a character of two or three bytes is one unit");
    CHECK(mk::utf16_length("\xF0\x9F\x98\x80") == 2u && mk::utf16_length("a\xF0\x9F\x98\x80" "b") == 4u, "a character outside the first plane is two units, as the client counts it");
    std::string astral;
    for (int i = 0; i < 25; ++i) astral += "\xF0\x9F\x98\x80";
    CHECK(mk::utf16_length(astral) == 50u && mk::utf16_length(astral + "\xF0\x9F\x98\x80") == 52u, "25 such characters are the 50 the limit allows, 26 are over it");
    CHECK(!mk::utf16_length("\xFF") && !mk::utf16_length("\xC3") && !mk::utf16_length("a\xE4\xBD") && !mk::utf16_length("\xC0\x80") && !mk::utf16_length("\xED\xA0\x80") &&
              !mk::utf16_length("\xF4\x90\x80\x80") && !mk::utf16_length("\x80"),
          "a text that is not well-formed UTF-8 (a stray byte, a cut or overlong sequence, a surrogate, a value past U+10FFFF) has no length");
    CHECK(mk::has_control_character("a\nb") && mk::has_control_character("a\rb") && mk::has_control_character("a\tb") && mk::has_control_character("a\x7f") &&
              !mk::has_control_character("Journeyman \\  Apprentice Sickle, Pickaxe, Axe  ") && !mk::has_control_character("<b> & | ' ( ) * ! : ;"),
          "a control character is below U+0020 or the delete character; the punctuation the recorded descriptions hold is none");
    CHECK(mk::contains_ignoring_case("Flameheart Emberfang Wand", "wand") && mk::contains_ignoring_case("Flameheart Emberfang Wand", "EMBERF") &&
              mk::contains_ignoring_case("Rough Ruby", "ough r") && mk::contains_ignoring_case("x", "") && !mk::contains_ignoring_case("Rough Ruby", "rubys") &&
              !mk::contains_ignoring_case("ab", "abc") && !mk::contains_ignoring_case("", "a"),
          "a text is inside another whatever the case and wherever it sits; nothing is inside a shorter text");
    CHECK(mk::folded("MiXed Case 12") == "mixed case 12", "folding lower-cases the ASCII letters");
    CHECK(mk::default_description("Erie") == "Erie's Storefront", "the default description is \"<name>'s Storefront\"");

    const mk::BadWords words(std::vector<std::string>{"Gorblax", "snarfwidget", "", "zq-x"});
    CHECK(words.size() == 3 && !words.empty(), "an empty word is dropped from the list");
    CHECK(words.contains("a GORBLAX here") && words.contains("gorblax") && words.contains("xxGorBlaXxx") && words.contains("SnarfWidget!") && words.contains("a zq-x b"),
          "a word of the list is found whatever the case, alone or inside another word");
    CHECK(!words.contains("") && !words.contains("gorbla x") && !words.contains("snarf widget") && !words.contains("zq x") && !words.contains("Fine goods"),
          "a word split by a space, a text with none of them and no text are clean");
    CHECK(!mk::BadWords().contains("anything") && mk::BadWords().empty(), "no list, no bad word");
}

// ---- the recorded searches ------------------------------------------------------------------------------------------------------------

static void test_market_search_recorded() {
    std::printf("the Marketplace: the recorded searches, answered from the recorded listings\n");
    namespace mk = ::aoc::worldd::market;
    const repl::GuidValue market = mk::marketplace_guid(kNode);
    struct Pair {
        const RecordedSearch* request;
        std::string           text;
        uint64_t              rarity;
    };
    const Pair requests[] = {{&kSearchEmptyRequest, "", 7}, {&kSearchWandRequest, "wand", 7}, {&kSearchRubyRequest, "ruby", 0}};
    for (const Pair& p : requests) {
        const std::optional<repl::RpcCall> call = recorded_search_call(*p.request, /*server_form=*/false);
        CHECK(call && call->decoded && call->rpc && std::string(call->rpc->name) == "ServerSearchMarketplaceHeaders", "a recorded search request reads");
        if (!call || !call->decoded) continue;
        const std::optional<repl::MarketSearchRequest> parsed = repl::parse_market_search(*call);
        CHECK(parsed && parsed->text == p.text && parsed->rarity == p.rarity, "it names the text and the rarity the recording sent (an empty text and the rarity 0 are not on the wire)");
        if (!parsed) continue;
        CHECK(same_bits(search_request(parsed->market, parsed->text, parsed->rarity), recorded_search_bunch(*p.request)), "the request the tests build is the recorded one, bit for bit");
    }

    // the 'wand' search: answered with the one storefront that lists the Emberfang Wand (Erie's description is the recorded one)
    {
        MarketHarness h;
        h.play();
        h.run(milliseconds(1500));
        const char* erie_items[] = {"Hollowshade Shoulders", "Hollowshade Gloves", "Thistleborn Necklace", "Hollowshade Bracers", "Flameheart Emberfang Wand", "Hollowshade Boots", "Hollowshade Belt",
                                    "Hollowshade Cape", "Flameheart Cinderborn Great Mace", "Flameheart Cinderslip Rapier", "Thistleborn Hoop Earring", "Hollowshade Jerkin", "Thistleborn Ring", "Flameheart Axe"};
        uint32_t k = 0;
        for (const char* item : erie_items) h.fx.world->market_listings()->add(shop_listing(1, "Erie", item, 0, kNode, k++));   // one seller: every listing is Erie's
        h.fx.world->market_listings()->add(shop_listing(2, "Wojtek", "Oak Timber"));   // (a recorded storefront that lists no wand)
        h.fx.world->market_listings()->set_description(kNode, stranger_listing(1).seller, "radiant weapons and armor");
        h.open_market(h.manager);
        const std::vector<repl::RpcCall> calls = search_calls(h, "wand", 7);
        const std::vector<PageRead> pages = pages_in(calls);
        const std::optional<repl::RpcCall> recorded = recorded_search_call(kSearchWandAnswer, /*server_form=*/true);
        CHECK(recorded && recorded->decoded && std::string(recorded->rpc->name) == "ClientReceiveMarketplaceHeaders", "the recorded answer of the 'wand' search reads");
        CHECK(pages.size() == 1 && count_calls(calls, "ClientMarketplaceHeaderUpdateResponse") == 0, "one page answers the search, and no refusal");
        if (recorded && recorded->decoded && pages.size() == 1) {
            const repl::ArrayValue* list = array_of(*recorded, "Headers");
            CHECK(list && list->elements.size() == 1 && pages[0].headers.size() == 1, "the recorded page holds one header, and so does ours");
            if (list && list->elements.size() == 1 && pages[0].headers.size() == 1) {
                CHECK(member_text(*recorded, "Headers", list->elements[0], "Headers.PlayerName") == pages[0].headers[0].name && pages[0].headers[0].name == "Erie",
                      "the storefront is Erie's, as recorded");
                CHECK(member_text(*recorded, "Headers", list->elements[0], "Headers.Description") == pages[0].headers[0].description && pages[0].headers[0].description == "radiant weapons and armor",
                      "its description is the recorded one");
            }
            CHECK(text_of(*recorded, "SearchItemString") == pages[0].search && pages[0].search == "wand", "the page echoes the text, as the recorded one does");
            CHECK(enum_of(*recorded, "ItemRarity").value_or(0) == pages[0].rarity && pages[0].rarity == 7, "and the rarity 7");
            const auto* recorded_complete = repl::rpcf::as<bool>(repl::rpcf::value_of(*recorded->rpc, recorded->params, "bComplete"));
            CHECK(recorded_complete && *recorded_complete == pages[0].complete && pages[0].complete, "and it is the last page");
            CHECK(enum_of(*recorded, "HeaderVersion").value_or(0) == pages[0].version && pages[0].version == 0, "retail's search page carries no version, and neither does ours");
            CHECK(pages[0].node == kNode, "it names the market's node");
        }
    }

    // the 'ruby' search at rarity 0 (Common): answered with the two storefronts that list the Rough Ruby
    {
        MarketHarness h;
        h.play();
        h.run(milliseconds(1500));
        const char* felagunda[] = {"Zinc Ingot", "Slate", "Copper Ingot", "Rough Ruby", "Recipe: NightreaverMag 2H Longbow", "Recipe: Nightreaver 1H Scepter", "Recipe: Nightreaver Mace"};
        uint32_t k = 0;
        for (const char* item : felagunda) h.fx.world->market_listings()->add(shop_listing(10, "Felagunda", item, 0, kNode, k++));
        for (const char* item : {"Dried Ash Board", "Dried Oak Board", "Rough Ruby"}) h.fx.world->market_listings()->add(shop_listing(11, "Hillwilliam", item, 0, kNode, k++));
        h.fx.world->market_listings()->add(shop_listing(12, "Wojtek", "Oak Timber"));
        h.open_market(h.manager);
        const std::vector<repl::RpcCall> calls = search_calls(h, "ruby", 0);
        const std::vector<PageRead> pages = pages_in(calls);
        const std::optional<repl::RpcCall> recorded = recorded_search_call(kSearchRubyAnswer, /*server_form=*/true);
        CHECK(recorded && recorded->decoded, "the recorded answer of the 'ruby' search reads");
        CHECK(pages.size() == 1 && pages[0].headers.size() == 2, "one page of two storefronts answers it");
        if (recorded && recorded->decoded && pages.size() == 1 && pages[0].headers.size() == 2) {
            const repl::ArrayValue* list = array_of(*recorded, "Headers");
            CHECK(list && list->elements.size() == 2, "the recorded page holds two headers");
            for (size_t i = 0; list && i < list->elements.size() && i < 2; ++i) {
                CHECK(member_text(*recorded, "Headers", list->elements[i], "Headers.PlayerName") == pages[0].headers[i].name &&
                          member_text(*recorded, "Headers", list->elements[i], "Headers.Description") == pages[0].headers[i].description,
                      "each header is the recorded one: the seller's name and \"<name>'s Storefront\"");
            }
            CHECK(has_header(pages[0], "Felagunda", "Felagunda's Storefront") && has_header(pages[0], "Hillwilliam", "Hillwilliam's Storefront"), "the storefronts are Felagunda's and Hillwilliam's");
            CHECK(text_of(*recorded, "SearchItemString") == pages[0].search && pages[0].search == "ruby", "the page echoes the text");
            CHECK(enum_of(*recorded, "ItemRarity").value_or(0) == pages[0].rarity && pages[0].rarity == 0, "and the rarity 0 (which retail left off the wire)");
            const auto* recorded_complete = repl::rpcf::as<bool>(repl::rpcf::value_of(*recorded->rpc, recorded->params, "bComplete"));
            CHECK(recorded_complete && *recorded_complete && pages[0].complete, "and it is the last page");
        }
    }

    // the empty search: answered 16, with no text, and that answer is the one our server builds
    {
        const std::optional<repl::RpcCall> recorded = recorded_search_call(kSearchRefusedAnswer, /*server_form=*/true);
        CHECK(recorded && recorded->decoded && std::string(recorded->rpc->name) == "ClientMarketplaceHeaderUpdateResponse", "the recorded refusal of the empty search reads");
        if (recorded && recorded->decoded) {
            CHECK(enum_of(*recorded, "Response").value_or(0) == 16 && text_of(*recorded, "HeaderDescription").empty(), "it is 16, with no text");
            const std::optional<repl::RpcParams> built = repl::market_header_update_params(*recorded->rpc, "", 16);
            CHECK(built && params_write_same_bits(*recorded->rpc, *built, recorded->params), "the answer our server builds for it is the recorded one, bit for bit");
        }
        MarketHarness h;
        h.play();
        h.run(milliseconds(1500));
        h.open_market(h.manager);
        const std::vector<repl::RpcCall> calls = search_calls(h, "", 7);
        const std::vector<UpdateRead> updates = updates_in(calls);
        CHECK(updates.size() == 1 && updates[0].response == 16 && updates[0].text.empty() && count_calls(calls, "ClientReceiveMarketplaceHeaders") == 0,
              "the empty search is answered with ClientMarketplaceHeaderUpdateResponse(\"\", 16) and no page, as recorded");
    }
}

// ---- the search ------------------------------------------------------------------------------------------------------------------------

static void test_market_search_matches_the_display_name() {
    std::printf("the Marketplace: the search finds a text inside an item's display name, whatever the case\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    auto* store = h.fx.world->market_listings();
    store->add(shop_listing(1, "Erie", "Flameheart Emberfang Wand"));
    store->add(shop_listing(2, "Brannoc", "Oak Timber"));
    market::Listing internal = shop_listing(3, "Cathal", "Elixir of Wit");
    internal.item.item_record_id = fi::kPotion;   // the record's own name is Test_Potion: it is no part of what is searched
    store->add(internal);
    store->add(shop_listing(4, "Dara", "Rough Ruby", 0, kThirdMarket));   // another market's listing
    h.open_market(h.manager);
    const auto names = [&](const std::string& text, uint64_t rarity = 7) {
        std::vector<std::string> out;
        for (const PageRead& p : pages_in(search_calls(h, text, rarity))) {
            for (const HeaderSeen& header : p.headers) out.push_back(header.name);
        }
        return out;
    };
    CHECK(names("wand") == std::vector<std::string>{"Erie"}, "'wand' finds the Emberfang Wand's storefront");
    CHECK(names("WAND") == std::vector<std::string>{"Erie"} && names("WaNd") == std::vector<std::string>{"Erie"}, "whatever the case of the text");
    CHECK(names("emberfang") == std::vector<std::string>{"Erie"} && names("MBERF") == std::vector<std::string>{"Erie"}, "a word of the name, or the middle of one, is found: it is a substring, not a prefix or a whole word");
    CHECK(names("flameheart emberfang wand") == std::vector<std::string>{"Erie"}, "the whole name is found");
    CHECK(names("k Timb") == std::vector<std::string>{"Brannoc"}, "a text across two words is found");
    CHECK(names("elixir") == std::vector<std::string>{"Cathal"} && names("wit") == std::vector<std::string>{"Cathal"}, "the display name is what is searched");
    CHECK(names("potion").empty() && names("test_").empty(), "the record's own name (Test_Potion) is not searched");
    CHECK(names("ruby").empty(), "a listing of another market is not found");
    CHECK(names("zzz").empty(), "a text no name holds finds nothing");
    const std::vector<PageRead> none = pages_in(search_calls(h, "zzz", 7));
    CHECK(none.size() == 1 && none[0].headers.empty() && none[0].complete && none[0].search == "zzz" && none[0].rarity == 7 && none[0].version == 0,
          "and answers one empty, complete page that echoes the text and the rarity (the client then shows an empty list)");
    const std::vector<PageRead> third = pages_in(search_calls(h, "ruby", 7, kThirdMarket));
    CHECK(third.empty(), "a search at a market the player has not opened is ignored");
}

static void test_market_search_rarity_is_exact() {
    std::printf("the Marketplace: the rarity is exactly the one picked, 7 is any\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    auto* store = h.fx.world->market_listings();
    const char* rarities[] = {"Common", "Uncommon", "Rare", "Heroic", "Epic", "Legendary", "Artifact"};
    for (uint32_t r = 0; r < 7; ++r) store->add(shop_listing(20 + r, std::string("seller ") + rarities[r], std::string("Test Gem ") + rarities[r], static_cast<uint8_t>(r)));
    h.open_market(h.manager);
    for (uint64_t r = 0; r < 7; ++r) {
        const std::vector<PageRead> pages = pages_in(search_calls(h, "test gem", r));
        const std::string what = std::string("rarity ") + std::to_string(r);
        CHECK(pages.size() == 1 && pages[0].headers.size() == 1 && pages[0].headers[0].name == std::string("seller ") + rarities[r], (what + ": only the listing of exactly that rarity is found").c_str());
        CHECK(pages.size() == 1 && pages[0].rarity == r && pages[0].search == "test gem" && pages[0].complete, (what + ": the page echoes the rarity and the text and is the last").c_str());
    }
    const std::vector<PageRead> any = pages_in(search_calls(h, "test gem", 7));
    CHECK(any.size() == 1 && any[0].headers.size() == 7 && any[0].rarity == 7, "7 finds the listing of every rarity");
    CHECK(pages_in(search_calls(h, "test gem", 8)).empty() && pages_in(search_calls(h, "test gem", 15)).empty(), "a rarity of 8 to 15 is no request of the client's dropdown: it is ignored, not answered");
    CHECK(count_calls(search_calls(h, "test gem", 15), "ClientMarketplaceHeaderUpdateResponse") == 0, "and it is not refused with 16 either");
}

static void test_market_search_leaves_out_the_searchers_own_storefront() {
    std::printf("the Marketplace: the searcher's own storefront is never in the answer\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    auto* store = h.fx.world->market_listings();
    market::Listing mine = shop_listing(30, "cyb", "Flameheart Emberfang Wand");
    mine.seller = character_guid(h);
    store->add(mine);
    store->add(shop_listing(31, "Erie", "Flameheart Emberfang Wand"));
    market::Listing only_mine = shop_listing(32, "cyb", "Dreamwood Staff");
    only_mine.seller = character_guid(h);
    store->add(only_mine);
    h.open_market(h.manager);
    const std::vector<PageRead> pages = pages_in(search_calls(h, "wand", 7));
    CHECK(pages.size() == 1 && pages[0].headers.size() == 1 && pages[0].headers[0].name == "Erie", "the search finds Erie's storefront and not the searcher's own, who lists a wand too");
    const std::vector<PageRead> only = pages_in(search_calls(h, "staff", 7));
    CHECK(only.size() == 1 && only[0].headers.empty() && only[0].complete && only[0].search == "staff", "a text only the searcher's own listing holds finds nothing: one empty, complete page");
}

static void test_market_search_text_lengths() {
    std::printf("the Marketplace: a search of 0 or more than 50 characters is refused 16, 1 to 50 is answered\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    const std::string fifty = std::string(49, 'q') + "z";
    h.fx.world->market_listings()->add(shop_listing(40, "Erie", fifty + " Wand"));
    h.fx.world->market_listings()->add(shop_listing(41, "Brannoc", "a"));
    h.open_market(h.manager);
    {
        const std::vector<repl::RpcCall> calls = search_calls(h, "", 7);
        check_one_update(calls, "", 16, "0 characters");
    }
    {
        const std::vector<repl::RpcCall> calls = search_calls(h, "w", 7);
        const std::vector<PageRead> pages = pages_in(calls);
        CHECK(pages.size() == 1 && pages[0].headers.size() == 1 && pages[0].headers[0].name == "Erie" && pages[0].search == "w" && count_calls(calls, "ClientMarketplaceHeaderUpdateResponse") == 0,
              "1 character: answered with a page that echoes it");
    }
    {
        const std::vector<repl::RpcCall> calls = search_calls(h, fifty, 7);
        const std::vector<PageRead> pages = pages_in(calls);
        CHECK(pages.size() == 1 && pages[0].headers.size() == 1 && pages[0].search == fifty && pages[0].complete && count_calls(calls, "ClientMarketplaceHeaderUpdateResponse") == 0,
              "50 characters: answered with a page that echoes all of them");
    }
    {
        const std::string over = fifty + "x";
        const std::vector<repl::RpcCall> calls = search_calls(h, over, 7);
        check_one_update(calls, over, 16, "51 characters");
    }
    {
        const std::vector<repl::RpcCall> calls = search_calls(h, std::string(200, 'x'), 7);
        check_one_update(calls, std::string(200, 'x'), 16, "200 characters");
    }
}

static void test_market_search_pages() {
    std::printf("the Marketplace: 250 matching storefronts come as three pages of 100, bComplete on the last\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    for (uint32_t n = 1; n <= 250; ++n) h.fx.world->market_listings()->add(shop_listing(100 + n, "shop " + std::to_string(n), "Oak Wand " + std::to_string(n), 2));
    for (uint32_t n = 1; n <= 20; ++n) h.fx.world->market_listings()->add(shop_listing(500 + n, "other " + std::to_string(n), "Pine Staff " + std::to_string(n), 2));
    market::Listing mine = shop_listing(900, "cyb", "Oak Wand Mine", 2);
    mine.seller = character_guid(h);
    h.fx.world->market_listings()->add(mine);
    h.open_market(h.manager);
    const std::vector<repl::RpcCall> calls = search_calls(h, "oak wand", 2);
    const std::vector<PageRead> pages = pages_in(calls);
    CHECK(pages.size() == 3, "250 storefronts are three pages");
    if (pages.size() == 3) {
        CHECK(pages[0].headers.size() == 100 && pages[1].headers.size() == 100 && pages[2].headers.size() == 50, "the pages hold 100, 100 and 50");
        CHECK(!pages[0].complete && !pages[1].complete && pages[2].complete, "only the last page is complete");
        for (const PageRead& p : pages) CHECK(p.search == "oak wand" && p.rarity == 2 && p.version == 0 && p.node == kNode, "each page echoes the text and the rarity, carries no version and names the node");
    }
    CHECK(header_count(pages) == 250, "every matching storefront is on a page once, and the searcher's own is not");
    CHECK(count_calls(calls, "ClientMarketplaceHeaderUpdateResponse") == 0, "no refusal comes with the pages");
}

static void test_market_search_is_one_a_second() {
    std::printf("the Marketplace: one search a second, as the game itself allows\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.fx.world->market_listings()->add(shop_listing(50, "Erie", "Flameheart Emberfang Wand"));
    h.open_market(h.manager);
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    h.run(milliseconds(1100));
    size_t from = h.got.size();
    h.client_sends({h.client.actor(h.pc_ch, search_request(market, "wand", 7), true), h.client.actor(h.pc_ch, search_request(market, "wand", 7), true)});
    h.settle();
    CHECK(pages_in(pc_calls_since(h, from)).size() == 1, "two identical searches in one frame (as the recorded 'wand' search holds) are answered once");
    from = h.got.size();
    h.run(milliseconds(300));
    h.client_request(search_request(market, "emberfang", 7), h.pc_ch);
    CHECK(pc_calls_since(h, from).empty() || (pages_in(pc_calls_since(h, from)).empty() && updates_in(pc_calls_since(h, from)).empty()), "a search within a second of the one before is answered with nothing");
    from = h.got.size();
    h.run(milliseconds(900));
    h.client_request(search_request(market, "emberfang", 7), h.pc_ch);
    const std::vector<PageRead> later = pages_in(pc_calls_since(h, from));
    CHECK(later.size() == 1 && later[0].headers.size() == 1 && later[0].search == "emberfang", "a second after the last one that was answered, a search is answered again");
    // a refusal for a length is not rate limited: the game checks the length before it counts
    from = h.got.size();
    h.client_request(search_request(market, "", 7), h.pc_ch);
    CHECK(updates_in(pc_calls_since(h, from)).size() == 1 && updates_in(pc_calls_since(h, from))[0].response == 16, "an empty search straight after is still refused 16");
}

static void test_market_search_ignored_without_the_window() {
    std::printf("the Marketplace: a search with no window, for another market or from out of reach is ignored\n");
    MarketHarness h;
    h.play();
    h.run(milliseconds(1500));
    h.fx.world->market_listings()->add(shop_listing(60, "Erie", "Flameheart Emberfang Wand"));
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const auto quiet = [&](const BunchWriter& request, const char* what) {
        h.run(milliseconds(1100));
        const size_t from = h.got.size();
        h.client_request(request, h.pc_ch);
        const std::vector<repl::RpcCall> calls = pc_calls_since(h, from);
        CHECK(count_calls(calls, "ClientReceiveMarketplaceHeaders") == 0 && count_calls(calls, "ClientMarketplaceHeaderUpdateResponse") == 0 && !h.s->failed(), what);
    };
    quiet(search_request(market, "wand", 7), "with no window open a search is ignored");
    quiet(search_request(market, "", 7), "and so is an empty one: no window, no answer");
    h.open_market(h.manager);
    repl::GuidValue another = market;
    another.a += 1;
    quiet(search_request(another, "wand", 7), "a search for another market is ignored");
    h.open_market(h.edge);
    quiet(search_request(market, "wand", 7), "a search from beyond 1000 units is ignored");
}

static void test_market_search_writes_nothing() {
    std::printf("the Marketplace: a search asks the database for nothing and writes nothing\n");
    MarketDb db;
    seed_cyb_listing(db);
    db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 3, kUnitPrice);
    MarketHarness h(cyb_record(), &db);
    ready_to_trade(h);
    const int saves = db.listing_saves;
    const size_t inventories = db.inventories.size();
    for (const char* text : {"test", "potion", "zzz"}) {
        h.run(milliseconds(1100));
        h.client_request(search_request(::aoc::worldd::market::marketplace_guid(kNode), text, 7), h.pc_ch);
    }
    h.run(milliseconds(1100));
    h.client_request(search_request(::aoc::worldd::market::marketplace_guid(kNode), "", 7), h.pc_ch);
    CHECK(db.listing_saves == saves && db.buy_calls == 0 && db.remove_calls == 0 && db.expire_calls == 0 && db.claim_calls == 0 && db.storefront_calls == 0 && db.inventories.size() == inventories,
          "the character service was asked for nothing: no listing, purchase, removal, expiry, claim, description or bag save");
    CHECK(db.raises_of(kNode) == 0 && db.storefront_of(kNode, kCyb).empty(), "and no description or page count changed");
}

/// The calls a peer's session heard for one request of its own client (the peer's copy of `update_calls` and `search_calls`).
static std::vector<repl::RpcCall> peer_calls(MarketPeer& p, const BunchWriter& rpc) {
    const size_t from = p.got.size();
    p.request(rpc, p.pc_ch);
    return market_calls_since(p.got, p.pc_ch, from);
}

static void peer_waits_a_second(MarketPeer& p) {
    for (int i = 0; i < 55; ++i) p.step();
}

static std::vector<repl::RpcCall> peer_search(MarketPeer& p, const std::string& text, uint64_t rarity, int64_t node = kNode) {
    peer_waits_a_second(p);
    return peer_calls(p, search_request(::aoc::worldd::market::marketplace_guid(node), text, rarity));
}

// ---- the rarity of a listing ----------------------------------------------------------------------------------------------------------

static void test_market_listing_rarity_comes_from_the_record_and_the_quality() {
    std::printf("the Marketplace: a listing's rarity is its record's at the stack's quality, from the saved rows and from a new listing\n");
    namespace mk = ::aoc::worldd::market;
    MarketDb db;
    // the potion record shows the real colour bands: Common 1-5, Uncommon 6-15, Rare 16-30, Heroic 31-50, Epic 51-70, Legendary 71-100, Artifact 101-200
    const int qualities[] = {1, 6, 16, 31, 51, 71, 101};
    for (size_t i = 0; i < 7; ++i) db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 1, kUnitPrice, 72ll * 3600 * 1000, 1000 + static_cast<int64_t>(i), qualities[i]);
    MarketHarness h(cyb_record(), &db);
    ready_to_trade(h);
    const std::vector<mk::Listing> loaded = h.fx.world->market_listings()->listings(kNode, guid_of_hex(kSeller));
    CHECK(loaded.size() == 7, "the world read the seven saved listings");
    bool all = loaded.size() == 7;
    for (size_t i = 0; i < loaded.size(); ++i) all = all && loaded[i].rarity == i;
    CHECK(all, "their rarities are Common to Artifact, by the quality of each stack");
    // a search for each rarity finds the seller once
    for (uint64_t r = 0; r < 7; ++r) {
        const std::vector<PageRead> pages = pages_in(search_calls(h, "test_potion", r));
        CHECK(pages.size() == 1 && pages[0].headers.size() == 1 && pages[0].headers[0].name == "Brannoc", ("a search at rarity " + std::to_string(r) + " finds the seller of that stack").c_str());
    }

    // a listing made now: the stack in the bag has the quality it was given
    svc::Character seller = cyb_record();
    for (svc::InventoryItem& it : seller.items) {
        if (it.container == Bag && it.slot == 1) it.quality = 51;   // Epic
    }
    MarketDb db2;
    MarketHarness m(seller, &db2);
    ready_to_trade(m);
    const svc::InventoryItem* held = m.s->inventory().at(Bag, 1);
    CHECK(held != nullptr && held->quality == 51, "the bag holds a stack of quality 51");
    if (!held) return;
    m.client_request(sell_request(mk::marketplace_guid(kNode), items::to_instance(*held), 2, kPrice), m.pc_ch);
    const std::vector<mk::Listing> mine = m.fx.world->market_listings()->listings(kNode, character_guid(m));
    CHECK(mine.size() == 1 && mine[0].rarity == 4, "the new listing is Epic (4)");
    MarketPeer buyer(m, second_record());
    buyer.play();
    buyer.request(interact(market_component(m.fx.world->registry(), m.manager)), buyer.pawn_ch);
    const std::vector<PageRead> epic = pages_in(peer_search(buyer, "test_potion", 4));
    CHECK(epic.size() == 1 && epic[0].headers.size() == 1 && epic[0].headers[0].name == "cyb" && epic[0].rarity == 4, "a buyer searching at Epic finds the new listing");
    const std::vector<PageRead> common = pages_in(peer_search(buyer, "test_potion", 0));
    CHECK(common.size() == 1 && common[0].headers.empty() && common[0].complete, "and a buyer searching at Common does not");
}

// ---- the descriptions ------------------------------------------------------------------------------------------------------------------

/// The peer opens the Manager's window (the interact key) and hears the open.
static std::vector<repl::RpcCall> peer_opens(MarketPeer& p, MarketHarness& h, uint64_t manager) {
    const size_t from = p.got.size();
    p.request(interact(market_component(h.fx.world->registry(), manager)), p.pawn_ch);
    return market_calls_since(p.got, p.pc_ch, from);
}

static void test_market_description_is_stored_and_the_next_pages_carry_it() {
    std::printf("the Marketplace: a seller edits his storefront's description, and the pages after it carry the text and the raised version\n");
    namespace mk = ::aoc::worldd::market;
    MarketDb db;
    seed_cyb_listing(db);
    db.seed_listing(kSeller, "Brannoc", kNode, fi::kPotion, 1, kUnitPrice);
    MarketHarness h(cyb_record(), &db);
    ready_to_trade(h);
    const repl::GuidValue market = mk::marketplace_guid(kNode);

    // a viewer has the window open before the edit: the pages he holds are the default ones at the game's own version
    MarketPeer bob(h, second_record());
    bob.play();
    const std::vector<PageRead> before = pages_in(peer_opens(bob, h, h.manager));
    CHECK(before.size() == 1 && before[0].version == 1843 && before[0].headers.size() == 2 && has_header(before[0], "cyb", "cyb's Storefront") &&
              has_header(before[0], "Brannoc", "Brannoc's Storefront"),
          "before the edit the viewer's pages hold the default text at version 1843");

    // the accepted edit: answered 0 with the text, stored with its market's version
    check_one_update(update_calls(h, "Fine goods at fair prices"), "Fine goods at fair prices", 0, "the edit");
    CHECK(db.storefront_of(kNode, kCyb) == "Fine goods at fair prices" && db.raises_of(kNode) == 1 && db.storefront_calls == 1,
          "the service stored the text and raised the market's version, in the one call it was made");
    CHECK(h.fx.world->market_listings()->header_raises(kNode) == 1 && h.fx.world->market_listings()->storefronts(kNode).size() == 2, "the world's cache holds the version");

    // the editor's own refresh: the new text and the raised version
    size_t from = h.got.size();
    h.client_request(market_headers_request(market, 1843), h.pc_ch);
    std::vector<PageRead> pages = pages_in(pc_calls_since(h, from));
    CHECK(pages.size() == 1 && pages[0].complete && pages[0].version == 1844 && pages[0].search.empty() && pages[0].rarity == 7, "the refresh is one complete page at version 1844 with no search and the rarity 7");
    CHECK(pages.size() == 1 && has_header(pages[0], "cyb", "Fine goods at fair prices") && has_header(pages[0], "Brannoc", "Brannoc's Storefront"),
          "it carries the new text of the edited storefront and the default of the other");

    // the viewer who had the window open sees it at his next refresh
    pages = pages_in(peer_calls(bob, market_headers_request(market, 1843)));
    CHECK(pages.size() == 1 && pages[0].version == 1844 && has_header(pages[0], "cyb", "Fine goods at fair prices"), "the viewer's refresh carries the raised version and the new text");
    // ... and in a search
    peer_waits_a_second(bob);
    pages = pages_in(peer_calls(bob, search_request(market, "test_potion", 7)));
    CHECK(pages.size() == 1 && pages[0].complete && pages[0].search == "test_potion" && pages[0].rarity == 7 && pages[0].version == 0 && pages[0].headers.size() == 2 &&
              has_header(pages[0], "cyb", "Fine goods at fair prices") && has_header(pages[0], "Brannoc", "Brannoc's Storefront"),
          "a search of the viewer's carries the new text on the editor's header (and no version: the pages of a search carry none)");
    // ... and a viewer who opens the window afterwards
    pages = pages_in(peer_opens(bob, h, h.manager));
    CHECK(pages.size() == 1 && pages[0].version == 1844 && has_header(pages[0], "cyb", "Fine goods at fair prices"), "the open of the window carries the new text at version 1844");

    // a second edit raises it again; the text stays whole
    check_one_update(update_calls(h, "Better goods"), "Better goods", 0, "the second edit");
    pages = pages_in(peer_calls(bob, market_headers_request(market, 1844)));
    CHECK(pages.size() == 1 && pages[0].version == 1845 && has_header(pages[0], "cyb", "Better goods") && db.raises_of(kNode) == 2, "the second edit is at version 1845 with its own text");

    // the same text again changes nothing: the service says so and the version stays
    check_one_update(update_calls(h, "Better goods"), "Better goods", 0, "the same text again");
    CHECK(db.raises_of(kNode) == 2 && h.fx.world->market_listings()->header_raises(kNode) == 2, "the same text again does not raise the version");
}

static void test_market_description_is_stored_as_sent() {
    std::printf("the Marketplace: a description of 50 characters, with spaces at both ends, is stored as sent\n");
    namespace mk = ::aoc::worldd::market;
    MarketDb db;
    seed_cyb_listing(db);
    MarketHarness h(cyb_record(), &db);
    ready_to_trade(h);
    const std::string recorded_longest = "ELG. I want Joeva to be trade hub. Vote for Me pls";   // the longest of the recorded retail descriptions
    CHECK(mk::utf16_length(recorded_longest) == 50u, "the recorded longest description is 50 characters");
    check_one_update(update_calls(h, recorded_longest), recorded_longest, 0, "50 characters");
    CHECK(db.storefront_of(kNode, kCyb) == recorded_longest, "stored whole");
    const std::string spaced = "  Journeyman \\  Apprentice Sickle, Axe  ";
    check_one_update(update_calls(h, spaced), spaced, 0, "spaces at both ends");
    CHECK(db.storefront_of(kNode, kCyb) == spaced, "the spaces at both ends are kept: the server does not trim");
    const std::string punctuation = "<b> & | ' ( ) * ! : ;";
    check_one_update(update_calls(h, punctuation), punctuation, 0, "the punctuation of the recorded descriptions");
    CHECK(db.storefront_of(kNode, kCyb) == punctuation, "stored whole");
    const std::string one = "x";
    check_one_update(update_calls(h, one), one, 0, "1 character");
    CHECK(db.storefront_of(kNode, kCyb) == one && db.raises_of(kNode) == 4, "one character is a description; four changes raised the version four times");
}

static void test_market_description_empty_puts_the_default_back() {
    std::printf("the Marketplace: an empty description puts the default \"<name>'s Storefront\" back\n");
    namespace mk = ::aoc::worldd::market;
    MarketDb db;
    seed_cyb_listing(db);
    MarketHarness h(cyb_record(), &db);
    ready_to_trade(h);
    const repl::GuidValue market = mk::marketplace_guid(kNode);
    MarketPeer bob(h, second_record());
    bob.play();
    peer_opens(bob, h, h.manager);
    check_one_update(update_calls(h, "Something to clear"), "Something to clear", 0, "the first edit");
    // an empty text: answered 0 with the text the storefront shows now, which is the default
    check_one_update(update_calls(h, ""), "cyb's Storefront", 0, "the empty edit");
    CHECK(db.storefront_of(kNode, kCyb).empty() && db.raises_of(kNode) == 2, "nothing is stored any more, and the clear raised the version");
    std::vector<PageRead> pages = pages_in(peer_calls(bob, market_headers_request(market, 1844)));
    CHECK(pages.size() == 1 && pages[0].version == 1845 && has_header(pages[0], "cyb", "cyb's Storefront"), "the next pages show the default text at the raised version");
    // clearing what is clear already changes nothing
    check_one_update(update_calls(h, ""), "cyb's Storefront", 0, "a second empty edit");
    CHECK(db.raises_of(kNode) == 2, "and does not raise the version");
}

static void test_market_description_refusals_change_nothing() {
    std::printf("the Marketplace: a description of 51 characters, with a bad word or a control character, or from a seller with no listing, is refused and changes nothing\n");
    namespace mk = ::aoc::worldd::market;
    MarketDb db;
    seed_cyb_listing(db);
    MarketHarness h(cyb_record(), &db);
    ready_to_trade(h);
    h.fx.world->set_market_bad_words(mk::BadWords(std::vector<std::string>{"gorblax", "SnarfWidget"}));   // a list the test builds: no word of the game's
    const repl::GuidValue market = mk::marketplace_guid(kNode);
    check_one_update(update_calls(h, "Kept text"), "Kept text", 0, "the text that must stay");
    const int calls_before = db.storefront_calls;
    const std::string fifty = std::string(50, 'a'), over = std::string(51, 'a');
    check_one_update(update_calls(h, over), over, 14, "51 characters");
    check_one_update(update_calls(h, std::string(500, 'b')), std::string(500, 'b'), 14, "500 characters");
    check_one_update(update_calls(h, "Cheap GORBLAX here"), "Cheap GORBLAX here", 15, "a bad word in capitals");
    check_one_update(update_calls(h, "xxgorblaxyy"), "xxgorblaxyy", 15, "a bad word inside another word");
    check_one_update(update_calls(h, "snarfwidget"), "snarfwidget", 15, "a bad word alone");
    check_one_update(update_calls(h, "two\nlines"), "two\nlines", 15, "a line break");
    check_one_update(update_calls(h, "a\ttab"), "a\ttab", 15, "a tab");
    CHECK(db.storefront_calls == calls_before, "the service was not asked: a refusal is made before it, so nothing could be half written");
    CHECK(db.storefront_of(kNode, kCyb) == "Kept text" && db.raises_of(kNode) == 1 && h.fx.world->market_listings()->header_raises(kNode) == 1, "the old text stays and the version does not move");
    size_t from = h.got.size();
    h.client_request(market_headers_request(market, 1843), h.pc_ch);
    const std::vector<PageRead> pages = pages_in(pc_calls_since(h, from));
    CHECK(pages.size() == 1 && pages[0].version == 1844 && has_header(pages[0], "cyb", "Kept text"), "the pages after the refusals carry the old text at the version it had");
    // a text just under the bad word's two halves, and 50 characters, pass
    check_one_update(update_calls(h, "gorbla x snarf widget"), "gorbla x snarf widget", 0, "a bad word split by a space");
    check_one_update(update_calls(h, fifty), fifty, 0, "50 characters");

    // a seller with no listing at this market has no storefront there: refused 1 (the client turns the button off), nothing stored
    MarketPeer bob(h, second_record());
    bob.play();
    peer_opens(bob, h, h.manager);
    const int before = db.storefront_calls;
    const std::vector<repl::RpcCall> refused = peer_calls(bob, header_update_request(market, "I list nothing"));
    check_one_update(refused, "I list nothing", 1, "a seller with no listing");
    CHECK(db.storefront_calls == before && db.storefront_of(kNode, kSecondId).empty(), "and the service was not asked");
}

static void test_market_description_ignored_without_the_window() {
    std::printf("the Marketplace: a description edit with no window, for another market or from out of reach is ignored\n");
    MarketDb db;
    seed_cyb_listing(db);
    MarketHarness h(cyb_record(), &db);
    h.play();
    h.run(milliseconds(1500));
    const repl::GuidValue market = ::aoc::worldd::market::marketplace_guid(kNode);
    const auto quiet = [&](const BunchWriter& request, const char* what) {
        const size_t from = h.got.size();
        h.client_request(request, h.pc_ch);
        const std::vector<repl::RpcCall> calls = pc_calls_since(h, from);
        CHECK(count_calls(calls, "ClientMarketplaceHeaderUpdateResponse") == 0 && count_calls(calls, "ClientReceiveMarketplaceHeaders") == 0 && !h.s->failed(), what);
    };
    quiet(header_update_request(market, "no window"), "with no window open an edit is ignored");
    h.open_market(h.manager);
    repl::GuidValue another = market;
    another.a += 1;
    quiet(header_update_request(another, "another market"), "an edit for another market is ignored");
    h.open_market(h.edge);
    quiet(header_update_request(market, "out of reach"), "an edit from beyond 1000 units is ignored");
    CHECK(db.storefront_calls == 0 && db.storefront_of(kNode, kCyb).empty() && db.raises_of(kNode) == 0, "nothing was stored");
}

static void test_market_description_belongs_to_one_market() {
    std::printf("the Marketplace: a description belongs to the one market where it was edited\n");
    namespace mk = ::aoc::worldd::market;
    MarketDb db;
    seed_cyb_listing(db, kNode);
    seed_cyb_listing(db, market_sell_test::kOtherNode);
    TwoMarketHarness h(cyb_record(), &db);
    h.play();
    h.run(milliseconds(1500));
    const repl::GuidValue first = mk::marketplace_guid(kNode), second = mk::marketplace_guid(market_sell_test::kOtherNode);
    const auto refreshed = [&](const repl::GuidValue& market) {
        const size_t from = h.got.size();
        h.client_request(market_headers_request(market, 1843), h.pc_ch);
        return pages_in(pc_calls_since(h, from));
    };
    const auto update = [&](const repl::GuidValue& market, const std::string& text) {
        const size_t from = h.got.size();
        h.client_request(header_update_request(market, text), h.pc_ch);
        return pc_calls_since(h, from);
    };
    h.open_market(h.manager);
    check_one_update(update(first, "The first market's text"), "The first market's text", 0, "the edit at the first market");
    h.open_market(h.other);
    check_one_update(update(second, "The second market's text"), "The second market's text", 0, "the edit at the second market");
    CHECK(db.storefront_of(kNode, kCyb) == "The first market's text" && db.storefront_of(market_sell_test::kOtherNode, kCyb) == "The second market's text",
          "two markets, two texts, each stored at its own node");
    CHECK(db.raises_of(kNode) == 1 && db.raises_of(market_sell_test::kOtherNode) == 1, "each market's version was raised once, by its own edit");
    std::vector<PageRead> pages = refreshed(second);
    CHECK(pages.size() == 1 && pages[0].node == market_sell_test::kOtherNode && pages[0].version == 1844 && has_header(pages[0], "cyb", "The second market's text") &&
              !has_header(pages[0], "cyb", "The first market's text"),
          "the second market's page carries the second market's text");
    h.open_market(h.manager);
    pages = refreshed(first);
    CHECK(pages.size() == 1 && pages[0].node == kNode && pages[0].version == 1844 && has_header(pages[0], "cyb", "The first market's text") && !has_header(pages[0], "cyb", "The second market's text"),
          "and the first market's page carries the first market's text");
    // clearing at one market leaves the other's text
    check_one_update(update(first, ""), "cyb's Storefront", 0, "the clear at the first market");
    h.open_market(h.other);
    pages = refreshed(second);
    CHECK(pages.size() == 1 && pages[0].version == 1844 && has_header(pages[0], "cyb", "The second market's text"), "the second market keeps its text and its version");
}

static void test_market_description_survives_a_relog_and_a_restart() {
    std::printf("the Marketplace: a description survives a log-out and a restart of the world\n");
    namespace mk = ::aoc::worldd::market;
    MarketDb db;
    seed_cyb_listing(db);
    const repl::GuidValue market = mk::marketplace_guid(kNode);
    {
        MarketHarness h(cyb_record(), &db);
        ready_to_trade(h);
        check_one_update(update_calls(h, "Here to stay"), "Here to stay", 0, "the edit");
        // the editor logs out and in again on the same world
        h.s->finish("the client left");
        h.s.reset();
        MarketPeer again(h, cyb_record());
        again.play();
        peer_opens(again, h, h.manager);
        const std::vector<PageRead> own = pages_in(peer_calls(again, market_headers_request(market, 1843)));
        CHECK(own.size() == 1 && own[0].version == 1844 && has_header(own[0], "cyb", "Here to stay"), "after the relog the pages carry the text at version 1844");
    }
    // the world restarts on the saved rows: a viewer opens the window
    MarketHarness restarted(second_record(), &db);
    CHECK(restarted.fx.world->market_listings()->header_raises(kNode) == 1, "the restarted world read the market's version");
    const size_t from = restarted.got.size();
    ready_to_trade(restarted);
    const std::vector<PageRead> pages = pages_in(pc_calls_since(restarted, from));
    CHECK(pages.size() == 1 && pages[0].version == 1844 && pages[0].complete && has_header(pages[0], "cyb", "Here to stay"), "after the restart the open carries the text at version 1844");
    CHECK(db.raises_of(kNode) == 1 && db.storefront_calls == 1, "and nothing was written again");
}

static void test_market_two_edits_at_once_the_last_wins_whole() {
    std::printf("the Marketplace: edits at once are written in a line, the last one stays whole, and the line is bounded\n");
    namespace mk = ::aoc::worldd::market;
    MarketDb db;
    seed_cyb_listing(db);
    MarketHarness h(cyb_record(), &db);
    ready_to_trade(h);
    const repl::GuidValue market = mk::marketplace_guid(kNode);
    db.hold_storefront = true;
    db.reset_started();
    const size_t from = h.got.size();
    std::vector<fake::Bunch> burst;
    for (const char* text : {"first text", "second text", "third text"}) burst.push_back(h.client.actor(h.pc_ch, header_update_request(market, text), true));
    h.client_sends(std::move(burst));
    CHECK(db.wait_started(3000), "the first edit is in flight");
    for (int i = 0; i < 5; ++i) h.step();
    CHECK(h.s->market_describing() == 3 && updates_in(pc_calls_since(h, from)).empty(), "three edits are in line and none is answered before the service answers");
    db.release_all();
    CHECK(step_session_until(h, [&] { return h.s->market_describing() == 0; }, 5000), "the line is written");
    const std::vector<UpdateRead> updates = updates_in(pc_calls_since(h, from));
    CHECK(updates.size() == 3 && updates[0].text == "first text" && updates[1].text == "second text" && updates[2].text == "third text" && updates[0].response == 0 && updates[1].response == 0 &&
              updates[2].response == 0,
          "each edit is answered 0 with its own text, in the order they were sent");
    CHECK(db.storefront_of(kNode, kCyb) == "third text" && db.raises_of(kNode) == 3 && db.storefront_calls == 3, "the last one stays, whole, and each of the three raised the version");
    CHECK(h.fx.world->market_listings()->header_raises(kNode) == 3, "the world's version is the service's");

    // the line is bounded: the fifth and sixth edits are answered 3 at once and never reach the service
    MarketDb bounded;
    seed_cyb_listing(bounded);
    MarketHarness b(cyb_record(), &bounded);
    ready_to_trade(b);
    bounded.hold_storefront = true;
    bounded.reset_started();
    const size_t bfrom = b.got.size();
    std::vector<fake::Bunch> six;
    for (int i = 1; i <= 6; ++i) six.push_back(b.client.actor(b.pc_ch, header_update_request(market, "edit " + std::to_string(i)), true));
    b.client_sends(std::move(six));
    CHECK(bounded.wait_started(3000), "the first of six edits is in flight");
    for (int i = 0; i < 5; ++i) b.step();
    const std::vector<UpdateRead> refused = updates_in(pc_calls_since(b, bfrom));
    CHECK(refused.size() == 2 && refused[0].response == 3 && refused[0].text == "edit 5" && refused[1].response == 3 && refused[1].text == "edit 6",
          "the fifth and the sixth are answered 3 (REQUEST_ALREADY_IN_PROGRESS) with their text, while the first four are still in line");
    bounded.release_all();
    CHECK(step_session_until(b, [&] { return b.s->market_describing() == 0; }, 5000), "the four in line are written");
    const std::vector<UpdateRead> all = updates_in(pc_calls_since(b, bfrom));
    CHECK(all.size() == 6 && bounded.storefront_calls == 4 && bounded.storefront_of(kNode, kCyb) == "edit 4" && bounded.raises_of(kNode) == 4, "four edits were written, and edit 4 is the text that stays");
}

static void test_market_description_of_a_session_that_ends_is_finished_by_the_world() {
    std::printf("the Marketplace: a log-out while an edit is being written loses nothing and writes nothing half\n");
    namespace mk = ::aoc::worldd::market;
    MarketDb db;
    seed_cyb_listing(db);
    MarketHarness h(cyb_record(), &db);
    ready_to_trade(h);
    const repl::GuidValue market = mk::marketplace_guid(kNode);
    db.hold_storefront = true;
    db.reset_started();
    h.client_sends({h.client.actor(h.pc_ch, header_update_request(market, "Written while leaving"), true)});
    CHECK(db.wait_started(3000), "the edit is in flight");
    CHECK(h.fx.world->market_listings()->header_raises(kNode) == 0, "the world's cache does not hold it yet");
    h.s->finish("the client left");
    h.s.reset();
    db.release_all();
    CHECK(tick_world_until(h, [&] { return h.fx.world->market_listings()->header_raises(kNode) == 1; }, 5000), "the world takes the answer of the session that left");
    const std::vector<mk::Storefront> fronts = h.fx.world->market_listings()->storefronts(kNode);
    CHECK(fronts.size() == 1 && fronts[0].description == "Written while leaving", "the cache holds the text the service stored");
    CHECK(db.storefront_of(kNode, kCyb) == "Written while leaving" && db.raises_of(kNode) == 1 && db.storefront_calls == 1, "and it was written once");
    MarketPeer bob(h, second_record());
    bob.play();
    const std::vector<PageRead> pages = pages_in(peer_opens(bob, h, h.manager));
    CHECK(pages.size() == 1 && pages[0].version == 1844 && has_header(pages[0], "cyb", "Written while leaving"), "a viewer's open carries the text and the raised version");
}

static void test_market_description_survives_a_service_that_is_down_or_loses_the_answer() {
    std::printf("the Marketplace: a service that is down or whose answer is lost does not lose or double an edit\n");
    namespace mk = ::aoc::worldd::market;
    {
        MarketDb db;
        seed_cyb_listing(db);
        MarketHarness h(cyb_record(), &db);
        ready_to_trade(h);
        db.fail_storefront_before = 2;
        check_one_update(update_calls(h, "After the outage"), "After the outage", 0, "an edit while the service is down");
        CHECK(db.storefront_calls == 3 && db.storefront_of(kNode, kCyb) == "After the outage" && db.raises_of(kNode) == 1, "the world asked again until the service answered: stored once, version raised once");
    }
    {
        MarketDb db;
        seed_cyb_listing(db);
        MarketHarness h(cyb_record(), &db);
        ready_to_trade(h);
        db.lose_storefront_answers = 1;
        check_one_update(update_calls(h, "Answer lost once"), "Answer lost once", 0, "an edit whose answer is lost");
        CHECK(db.storefront_calls == 2 && db.storefront_of(kNode, kCyb) == "Answer lost once", "the world asked again with the same text");
        CHECK(db.raises_of(kNode) == 1 && h.fx.world->market_listings()->header_raises(kNode) == 1, "which the service held already: the version was raised once, not twice");
    }
    {
        // the listing is gone from the database though the world's cache still holds it: the service refuses with nothing written
        MarketDb db;
        const std::string listing = seed_cyb_listing(db);
        MarketHarness h(cyb_record(), &db);
        ready_to_trade(h);
        {
            std::lock_guard<std::mutex> lock(db.mu);
            db.find_row(listing)->status = market_db::kSold;
        }
        check_one_update(update_calls(h, "No listing any more"), "No listing any more", 1, "an edit of a seller whose listing sold meanwhile");
        CHECK(db.storefront_calls == 1 && db.storefront_of(kNode, kCyb).empty() && db.raises_of(kNode) == 0 && h.fx.world->market_listings()->header_raises(kNode) == 0,
              "the service was asked and refused it: nothing was written");
    }
}

// ---- the bad words ---------------------------------------------------------------------------------------------------------------------

static void test_market_bad_words_come_from_content_db() {
    std::printf("the Marketplace: the world reads the bad-word list of content.db, and a content.db without one checks nothing\n");
    namespace mk = ::aoc::worldd::market;
    // a content.db with the bad_word table (rows the test makes up)
    {
        content_fixture::DbOptions options;
        options.bad_word_rows = "(0, 'gorblax'), (1, 'SnarfWidget')";
        MarketDb db;
        seed_cyb_listing(db);
        MarketHarness h(cyb_record(), &db, options);
        ready_to_trade(h);
        const mk::BadWords* words = h.fx.world->market_bad_words();
        CHECK(words != nullptr && words->size() == 2 && words->contains("a SNARFWIDGET") && !words->contains("fine"), "the world holds the table's two words");
        check_one_update(update_calls(h, "Gorblax for sale"), "Gorblax for sale", 15, "a word of content.db's list");
        check_one_update(update_calls(h, "Honest goods"), "Honest goods", 0, "a text with none");
        CHECK(db.storefront_calls == 1 && db.storefront_of(kNode, kCyb) == "Honest goods", "only the clean text was stored");
    }
    // a content.db made before contentc version 52: no list, no check (the world says so in its log)
    {
        MarketDb db;
        seed_cyb_listing(db);
        MarketHarness h(cyb_record(), &db);
        ready_to_trade(h);
        const mk::BadWords* words = h.fx.world->market_bad_words();
        CHECK(words != nullptr && words->empty(), "a content.db with no bad_word table gives an empty list");
    }
}

static void test_market_description_refuses_a_word_of_the_real_bad_word_list() {
    std::printf("the Marketplace: a word of the client's real bad-word list is refused, in any case and inside another word\n");
    namespace mk = ::aoc::worldd::market;
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (!env || !*env) {
        std::printf("  [skip] AOC_CONTENT_DB is not set: the client's bad-word list is not at hand (content.db built by contentc 52)\n");
        return;
    }
    std::string error;
    auto real = content::ContentDb::open(env, error);
    CHECK(real.has_value(), "the real content.db opens");
    if (!real) return;
    bool present = false;
    const std::vector<std::string> list = real->bad_words(present, error);
    CHECK(present && error.empty() && list.size() > 100, "it holds the client's bad-word list (contentc version 52)");
    if (!present || list.empty()) return;
    MarketDb db;
    seed_cyb_listing(db);
    MarketHarness h(cyb_record(), &db);
    ready_to_trade(h);
    h.fx.world->set_market_bad_words(mk::BadWords(list));
    const std::string first = list.front(), last = list.back();
    std::string upper = first;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const std::string a = "a " + upper + "!", b = "xx" + last + "yy";
    check_one_update(update_calls(h, a), a, 15, "the list's first word in capitals");
    check_one_update(update_calls(h, b), b, 15, "the list's last word inside another word");
    check_one_update(update_calls(h, "Honest goods"), "Honest goods", 0, "a clean text");
    CHECK(db.storefront_calls == 1, "only the clean text reached the service");
}

AOC_REGISTER_TEST(test_market_text_rules);
AOC_REGISTER_TEST(test_market_search_recorded);
AOC_REGISTER_TEST(test_market_search_matches_the_display_name);
AOC_REGISTER_TEST(test_market_search_rarity_is_exact);
AOC_REGISTER_TEST(test_market_search_leaves_out_the_searchers_own_storefront);
AOC_REGISTER_TEST(test_market_search_text_lengths);
AOC_REGISTER_TEST(test_market_search_pages);
AOC_REGISTER_TEST(test_market_search_is_one_a_second);
AOC_REGISTER_TEST(test_market_search_ignored_without_the_window);
AOC_REGISTER_TEST(test_market_search_writes_nothing);
AOC_REGISTER_TEST(test_market_listing_rarity_comes_from_the_record_and_the_quality);
AOC_REGISTER_TEST(test_market_description_is_stored_and_the_next_pages_carry_it);
AOC_REGISTER_TEST(test_market_description_is_stored_as_sent);
AOC_REGISTER_TEST(test_market_description_empty_puts_the_default_back);
AOC_REGISTER_TEST(test_market_description_refusals_change_nothing);
AOC_REGISTER_TEST(test_market_description_ignored_without_the_window);
AOC_REGISTER_TEST(test_market_description_belongs_to_one_market);
AOC_REGISTER_TEST(test_market_description_survives_a_relog_and_a_restart);
AOC_REGISTER_TEST(test_market_two_edits_at_once_the_last_wins_whole);
AOC_REGISTER_TEST(test_market_description_of_a_session_that_ends_is_finished_by_the_world);
AOC_REGISTER_TEST(test_market_description_survives_a_service_that_is_down_or_loses_the_answer);
AOC_REGISTER_TEST(test_market_bad_words_come_from_content_db);
AOC_REGISTER_TEST(test_market_description_refuses_a_word_of_the_real_bad_word_list);
