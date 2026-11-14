#include <set>

#include "archon/crypto.hpp"
#include "test.hpp"

using namespace arc;

TEST(pairing_code_format_and_spread) {
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
        std::string c = generate_pairing_code();
        CHECK(is_valid_pairing_code(c));
        seen.insert(c);
    }
    CHECK(seen.size() > 190);  // collisions among 200 draws of 10^6 are rare
    CHECK(!is_valid_pairing_code("12345"));
    CHECK(!is_valid_pairing_code("12a456"));
}

static bool run_cpace(const std::string& agent_code, const std::string& ctrl_code, SessionKeys* a_out = nullptr,
                      SessionKeys* c_out = nullptr) {
    const Id16 sid = random_array<16>(), aid = random_array<16>(), cid = random_array<16>();
    CpaceParty a, c;
    cpace_start(cpace_generator(agent_code, sid, aid, cid), a);
    cpace_start(cpace_generator(ctrl_code, sid, aid, cid), c);
    Key32 ka, kc;
    if (!cpace_finish(a, c.y, ka) || !cpace_finish(c, a.y, kc)) return false;
    SessionKeys sa = derive_pair_keys(ka, sid, aid, cid, a.y, c.y);
    SessionKeys sc = derive_pair_keys(kc, sid, aid, cid, a.y, c.y);
    if (a_out) *a_out = sa;
    if (c_out) *c_out = sc;
    return ct_equal(sa.tag_controller, sc.tag_controller);
}

TEST(cpace_same_code_agrees_on_all_keys) {
    SessionKeys a, c;
    CHECK(run_cpace("482913", "482913", &a, &c));
    CHECK(ct_equal(a.pair_key, c.pair_key));
    CHECK(ct_equal(a.key_a2c, c.key_a2c));
    CHECK(ct_equal(a.key_c2a, c.key_c2a));
    CHECK(!ct_equal(a.key_a2c, a.key_c2a));
    CHECK(!ct_equal(a.tag_agent, a.tag_controller));
}

TEST(cpace_wrong_code_disagrees) {
    CHECK(!run_cpace("482913", "482914"));
    CHECK(!run_cpace("000000", "999999"));
}

TEST(cpace_rejects_degenerate_peer_share) {
    CpaceParty a;
    cpace_start(cpace_generator("123456", random_array<16>(), random_array<16>(), random_array<16>()), a);
    Key32 zero{}, k;
    CHECK(!cpace_finish(a, zero, k));
}

TEST(resume_keys_depend_on_both_nonces) {
    const Key32 pk = random_array<32>();
    const Id16 cid = random_array<16>(), aid = random_array<16>();
    const Key32 n1 = random_array<32>(), n2 = random_array<32>(), n3 = random_array<32>();
    SessionKeys a = derive_resume_keys(pk, cid, aid, n1, n2);
    SessionKeys b = derive_resume_keys(pk, cid, aid, n1, n2);
    SessionKeys c = derive_resume_keys(pk, cid, aid, n1, n3);
    CHECK(ct_equal(a.tag_agent, b.tag_agent));
    CHECK(!ct_equal(a.tag_agent, c.tag_agent));
    SessionKeys wrong = derive_resume_keys(random_array<32>(), cid, aid, n1, n2);
    CHECK(!ct_equal(a.tag_controller, wrong.tag_controller));
}

TEST(secure_channel_roundtrip_tamper_and_replay) {
    const Key32 k1 = random_array<32>(), k2 = random_array<32>();
    SecureChannel a, b;
    a.init(k1, k2);
    b.init(k2, k1);
    Bytes m1{1, 2, 3}, m2{4, 5};
    Bytes f1 = a.seal(m1), f2 = a.seal(m2);
    Bytes out;
    REQUIRE(b.open(f1.data(), f1.size(), out));
    CHECK(out == m1);
    // Replaying f1 must fail: the receiver expects counter 1 now.
    CHECK(!b.open(f1.data(), f1.size(), out));

    SecureChannel c;
    c.init(k2, k1);
    Bytes t = f1;
    t[t.size() - 1] ^= 1;
    CHECK(!c.open(t.data(), t.size(), out));
    // Reordering fails too.
    SecureChannel d;
    d.init(k2, k1);
    CHECK(!d.open(f2.data(), f2.size(), out));
    CHECK(!d.open(f1.data(), 10, out));  // shorter than a MAC
}
