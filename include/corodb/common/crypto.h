// Copyright (c) 2024 CoroDB Authors. All rights reserved.

/** @file crypto.h @brief 认证用密码学原语：SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256。
 *
 * 无外部依赖的标准实现，供口令存储使用（ROADMAP P2：强制认证 + 口令 KDF）。
 * 口令存储格式（UserManager）：pbkdf2-sha256$<iterations>$<salt_hex>$<dk_hex>，
 * 每用户独立随机盐；旧 FNV 口令仍可校验（登录后应重设口令迁移）。 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace corodb::crypto {

    /** @brief SHA-256 摘要（原始 32 字节）。 */
    [[nodiscard]] std::string sha256(const std::string& data);

    /** @brief HMAC-SHA256（原始 32 字节）。 */
    [[nodiscard]] std::string hmac_sha256(const std::string& key, const std::string& data);

    /** @brief PBKDF2-HMAC-SHA256（原始 dk_len 字节；RFC 2898）。
     *  @param password 口令。
     *  @param salt 盐。
     *  @param iterations 迭代次数（≥1）。
     *  @param dk_len 派生密钥长度（字节）。 */
    [[nodiscard]] std::string pbkdf2_hmac_sha256(const std::string& password, const std::string& salt,
                                                 uint32_t iterations, std::size_t dk_len);

    /** @brief 字节串转小写十六进制。 */
    [[nodiscard]] std::string to_hex(const std::string& raw);

    /** @brief 恒定时间比较（防时序侧信道；长度不同立即返回 false）。 */
    bool constant_time_equal(const std::string& a, const std::string& b);

} // namespace corodb::crypto
