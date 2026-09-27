// Copyright (c) 2024 CoroDB Authors. All rights reserved.
//
// @file crypto.cpp
// @brief SHA-256 / HMAC / PBKDF2 标准实现（FIPS 180-4 / RFC 2104 / RFC 2898）。

#include "corodb/common/crypto.h"

#include <cstring>
#include <stdexcept>
#include <vector>

namespace corodb::crypto {

    namespace {

        constexpr uint32_t kK[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };

        inline uint32_t rotr(uint32_t x, uint32_t n) noexcept {
            return (x >> n) | (x << (32 - n));
        }

        /** @brief 最小化 SHA-256 流式实现。 */
        class Sha256 {
        public:
            void update(const unsigned char* data, std::size_t len) {
                total_ += len;
                while (len > 0) {
                    const std::size_t take = std::min(len, std::size_t{ 64 } - buflen_);
                    std::memcpy(buf_ + buflen_, data, take);
                    buflen_ += take;
                    data += take;
                    len -= take;
                    if (buflen_ == 64) {
                        transform(buf_);
                        buflen_ = 0;
                    }
                }
            }

            void final(unsigned char out[32]) {
                // 填充：0x80 + 零 + 8 字节大端总位数。
                const uint64_t bits = total_ * 8;
                unsigned char pad = 0x80;
                update(&pad, 1);
                const unsigned char zero = 0;
                while (buflen_ != 56)
                    update(&zero, 1);
                unsigned char len_be[8];
                for (int i = 0; i < 8; ++i)
                    len_be[i] = static_cast<unsigned char>((bits >> (56 - 8 * i)) & 0xFF);
                update(len_be, 8);
                for (int i = 0; i < 8; ++i) {
                    out[i * 4] = static_cast<unsigned char>((state_[i] >> 24) & 0xFF);
                    out[i * 4 + 1] = static_cast<unsigned char>((state_[i] >> 16) & 0xFF);
                    out[i * 4 + 2] = static_cast<unsigned char>((state_[i] >> 8) & 0xFF);
                    out[i * 4 + 3] = static_cast<unsigned char>(state_[i] & 0xFF);
                }
            }

        private:
            void transform(const unsigned char* block) {
                uint32_t w[64];
                for (int i = 0; i < 16; ++i) {
                    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                           static_cast<uint32_t>(block[i * 4 + 3]);
                }
                for (int i = 16; i < 64; ++i) {
                    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
                uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
                for (int i = 0; i < 64; ++i) {
                    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                    const uint32_t ch = (e & f) ^ (~e & g);
                    const uint32_t t1 = h + s1 + ch + kK[i] + w[i];
                    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                    const uint32_t t2 = s0 + maj;
                    h = g;
                    g = f;
                    f = e;
                    e = d + t1;
                    d = c;
                    c = b;
                    b = a;
                    a = t1 + t2;
                }
                state_[0] += a;
                state_[1] += b;
                state_[2] += c;
                state_[3] += d;
                state_[4] += e;
                state_[5] += f;
                state_[6] += g;
                state_[7] += h;
            }

            uint32_t state_[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
            unsigned char buf_[64]{};
            std::size_t buflen_{ 0 };
            uint64_t total_{ 0 };
        };

    } // namespace

    std::string sha256(const std::string& data) {
        Sha256 h;
        h.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
        unsigned char out[32];
        h.final(out);
        return std::string(reinterpret_cast<const char*>(out), 32);
    }

    std::string hmac_sha256(const std::string& key, const std::string& data) {
        const std::size_t kBlock = 64;
        std::string k = key;
        if (k.size() > kBlock)
            k = sha256(k);
        k.resize(kBlock, '\0');
        std::string ipad(kBlock, '\x36');
        std::string opad(kBlock, '\x5c');
        for (std::size_t i = 0; i < kBlock; ++i) {
            ipad[i] = static_cast<char>(static_cast<unsigned char>(ipad[i]) ^
                                        static_cast<unsigned char>(k[i]));
            opad[i] = static_cast<char>(static_cast<unsigned char>(opad[i]) ^
                                        static_cast<unsigned char>(k[i]));
        }
        return sha256(opad + sha256(ipad + data));
    }

    std::string pbkdf2_hmac_sha256(const std::string& password, const std::string& salt, uint32_t iterations,
                                   std::size_t dk_len) {
        if (iterations == 0)
            throw std::invalid_argument("PBKDF2 iterations must be >= 1");
        std::string out;
        out.reserve(dk_len);
        const std::string p = password;
        for (uint32_t block = 1; out.size() < dk_len; ++block) {
            // U1 = HMAC(P, S || INT_32_BE(block))
            const unsigned char be[4] = { static_cast<unsigned char>((block >> 24) & 0xFF),
                                          static_cast<unsigned char>((block >> 16) & 0xFF),
                                          static_cast<unsigned char>((block >> 8) & 0xFF),
                                          static_cast<unsigned char>(block & 0xFF) };
            std::string u = hmac_sha256(p, salt + std::string(reinterpret_cast<const char*>(be), 4));
            std::string t = u;
            for (uint32_t i = 1; i < iterations; ++i) {
                u = hmac_sha256(p, u);
                for (std::size_t j = 0; j < t.size(); ++j)
                    t[j] = static_cast<char>(static_cast<unsigned char>(t[j]) ^
                                             static_cast<unsigned char>(u[j]));
            }
            out += t;
        }
        out.resize(dk_len);
        return out;
    }

    std::string to_hex(const std::string& raw) {
        static const char* digits = "0123456789abcdef";
        std::string out;
        out.reserve(raw.size() * 2);
        for (unsigned char c: raw) {
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0x0F]);
        }
        return out;
    }

    bool constant_time_equal(const std::string& a, const std::string& b) {
        if (a.size() != b.size())
            return false;
        unsigned char diff = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
        return diff == 0;
    }

} // namespace corodb::crypto
