#pragma once
// Stored SrtData records of the DrawACM_v draws of capture 11906 (RenderDoc export E10, read-only, not in git).
// Extracted offline from E10 blobs/000335 (resource 329, 12,582,912 B, SHA-256
// 02474f0403795c16f9e25fd9777d1a54137bd4b206819f738af329b4aa10ecb4), 416 bytes each, little-endian float32.
//  * acm_stored_draw3_record_hex      : pass 0, ordinal 3, offset 967184, record SHA-256
//    af5697ae756799dc697e04594792e773e1ff5878ac0d5ac18e22732892994fb4
//    (A at +0 and C at +64 are a valid view-projection pair; B at +256 is a pure translation; +360 = 1.0; +400 = 512,-448,2048,2048.)
//  * acm_stored_pass6_ord35_record_hex: pass 6, ordinal 35, offset 1075616, record SHA-256
//    922ffdc042c8f2101d656330972737be65f0d122c7f1d15ceeca3a56768d7269
//    (A bit-identical to draw 3's; +64..+127 is NOT a matrix: -2.95,1.3,-37,0 / 0,NaN,NaN,0 / int32 128s; B is a rotation + translation.)
// The record hashes were computed offline at extraction (not re-hashed by the tests); the tests pin the decoded A matrix, B translation,
// +360 and +400 values of both records instead.
#include <array>
#include <cstdint>
#include <cstring>
namespace acm_fixture {
static const char* const acm_stored_draw3_record_hex =
    "8046dd444917ba44075538c3a30c393f1004a33f572525c40667833fe9e983bb"
    "61018bc47054afc479353043fee430bff6e91a4d40693b4d5c7be7ca7b98ba47"
    "09c18f3f2a29173db10e393fa30c393f0115173d3de738c060eb83bbe9e983bb"
    "875a963f8512633df5e630bffee430bf3871fbc759b36c458e99ba477b98ba47"
    "013005005c22f5c14800000000000000a4a04447c3158347c3150346a6f9f938"
    "cf8bd33e2fbdbb3300000000000000000000000000000000000000000000803f"
    "a7a6a63ea7a6a63e8988883e0000803f9998983da9a8a83da9a8a83d0000803f"
    "0000000000000000000000000000803fdedcdc3ef4f2f23ed4d2d23e0000803f"
    "0000803f000000000000000000000000000000000000803f0000000000000000"
    "00000000000000000000803f000000005e603d46a80b00443e1bc2470000803f"
    "40a400450a54a400340000000200005000007f435555b5bbfbf8863f6c7f87bb"
    "00000000000000000000803f00000000a9a8a83ed2d0d03ecccaca3e0000803f"
    "a4a04447c3158347c3150346a6f9f938000000440000e0c30000004500000045";
static const char* const acm_stored_pass6_ord35_record_hex =
    "8046dd444917ba44075538c3a30c393f1004a33f572525c40667833fe9e983bb"
    "61018bc47054afc479353043fee430bff6e91a4d40693b4d5c7be7ca7b98ba47"
    "04e73cc06666a63f000014c200000000000000002af1ffff04faffff00000000"
    "800000008000000080000000800000003c15783f2e8e263f5600000000000000"
    "5d1cfc3e0000008000000000000000007d1b443f000080bf0000000000000000"
    "cf8bd33e2fbdbb3300000000000000000000000000000000000000000000803f"
    "a7a6a63ea7a6a63e8988883e0000803f9998983da9a8a83da9a8a83d0000803f"
    "0000000000000000000000000000803fdedcdc3ef4f2f23ed4d2d23e0000803f"
    "2475163fc362fb3ec49e243f00000000921da7be88035f3f83d1bbbe00000000"
    "4c843dbf9707bb3bbd182c3f00000000c35660c6aae257458f89f0470000803f"
    "00a500510a14a500340000000200005000007f435555b5bb0080ef470000803f"
    "0000803f000000000000803f00000000a9a8a83ed2d0d03ecccaca3e0000803f"
    "35ef98bf1f855bc0f4fd72c100000000000000440000e0c30000004500000045";

inline std::array<uint8_t,416> acm_stored_record(const char* hex) {
    std::array<uint8_t,416> out{};
    auto nib=[](char c)->unsigned {return c>='0'&&c<='9'?unsigned(c-'0'):unsigned(c-'a'+10);};
    for(size_t i=0;i<416;++i)out[i]=uint8_t(nib(hex[2*i])<<4|nib(hex[2*i+1]));
    return out;
}
}  // namespace acm_fixture
