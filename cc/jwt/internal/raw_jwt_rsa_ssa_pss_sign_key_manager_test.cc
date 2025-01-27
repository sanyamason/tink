// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////////

#include "tink/jwt/internal/raw_jwt_rsa_ssa_pss_sign_key_manager.h"

#include <string>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "openssl/rsa.h"
#include "tink/public_key_sign.h"
#include "tink/subtle/rsa_ssa_pss_verify_boringssl.h"
#include "tink/subtle/subtle_util_boringssl.h"
#include "tink/util/status.h"
#include "tink/util/statusor.h"
#include "tink/util/test_matchers.h"
#include "proto/jwt_rsa_ssa_pss.pb.h"

namespace crypto {
namespace tink {
namespace {

using ::crypto::tink::subtle::SubtleUtilBoringSSL;
using ::crypto::tink::test::IsOk;
using ::crypto::tink::util::StatusOr;
using ::google::crypto::tink::JwtRsaSsaPssAlgorithm;
using ::google::crypto::tink::JwtRsaSsaPssKeyFormat;
using ::google::crypto::tink::JwtRsaSsaPssPrivateKey;
using ::google::crypto::tink::JwtRsaSsaPssPublicKey;
using ::google::crypto::tink::KeyData;
using ::testing::Eq;
using ::testing::Gt;
using ::testing::Not;
using ::testing::SizeIs;

TEST(RawJwtRsaSsaPssSignKeyManagerTest, Basic) {
  EXPECT_THAT(RawJwtRsaSsaPssSignKeyManager().get_version(), Eq(0));
  EXPECT_THAT(RawJwtRsaSsaPssSignKeyManager().key_material_type(),
              Eq(KeyData::ASYMMETRIC_PRIVATE));
  EXPECT_THAT(
      RawJwtRsaSsaPssSignKeyManager().get_key_type(),
      Eq("type.googleapis.com/google.crypto.tink.JwtRsaSsaPssPrivateKey"));
}

JwtRsaSsaPssKeyFormat CreateKeyFormat(JwtRsaSsaPssAlgorithm algorithm,
                                      int modulus_size_in_bits,
                                      int public_exponent) {
  JwtRsaSsaPssKeyFormat key_format;
  key_format.set_algorithm(algorithm);
  key_format.set_modulus_size_in_bits(modulus_size_in_bits);

  bssl::UniquePtr<BIGNUM> e(BN_new());
  BN_set_word(e.get(), public_exponent);
  key_format.set_public_exponent(
      subtle::SubtleUtilBoringSSL::bn2str(e.get(), BN_num_bytes(e.get()))
          .ValueOrDie());

  return key_format;
}

TEST(RawJwtRsaSsaPssSignKeyManagerTest, ValidatePs256KeyFormat) {
  JwtRsaSsaPssKeyFormat key_format =
      CreateKeyFormat(JwtRsaSsaPssAlgorithm::PS256, 3072, RSA_F4);
  EXPECT_THAT(RawJwtRsaSsaPssSignKeyManager().ValidateKeyFormat(key_format),
              IsOk());
}

TEST(RawJwtRsaSsaPssSignKeyManagerTest, ValidatePs512KeyFormat) {
  JwtRsaSsaPssKeyFormat key_format =
      CreateKeyFormat(JwtRsaSsaPssAlgorithm::PS512, 3072, RSA_F4);
  EXPECT_THAT(RawJwtRsaSsaPssSignKeyManager().ValidateKeyFormat(key_format),
              IsOk());
}

TEST(RawJwtRsaSsaPssSignKeyManagerTest, KeyWithSmallModulusIsInvalid) {
  JwtRsaSsaPssKeyFormat key_format =
      CreateKeyFormat(JwtRsaSsaPssAlgorithm::PS256, 512, RSA_F4);
  key_format.set_modulus_size_in_bits(512);
  EXPECT_THAT(RawJwtRsaSsaPssSignKeyManager().ValidateKeyFormat(key_format),
              Not(IsOk()));
}

TEST(RawJwtRsaSsaPssSignKeyManagerTest, ValidateKeyFormatUnkownHashDisallowed) {
  JwtRsaSsaPssKeyFormat key_format =
      CreateKeyFormat(JwtRsaSsaPssAlgorithm::PS_UNKNOWN, 3072, RSA_F4);
  EXPECT_THAT(RawJwtRsaSsaPssSignKeyManager().ValidateKeyFormat(key_format),
              Not(IsOk()));
}

// Runs several sanity checks, checking if a given private key fits a format.
void CheckNewKey(const JwtRsaSsaPssPrivateKey& private_key,
                 const JwtRsaSsaPssKeyFormat& key_format) {
  JwtRsaSsaPssPublicKey public_key = private_key.public_key();

  EXPECT_THAT(private_key.version(), Eq(0));
  EXPECT_THAT(private_key.version(), Eq(public_key.version()));
  EXPECT_THAT(public_key.n().length(), Gt(0));
  EXPECT_THAT(public_key.e().length(), Gt(0));
  EXPECT_THAT(public_key.algorithm(), Eq(key_format.algorithm()));

  EXPECT_THAT(key_format.public_exponent(), Eq(public_key.e()));
  auto n = std::move(SubtleUtilBoringSSL::str2bn(public_key.n()).ValueOrDie());
  auto d = std::move(SubtleUtilBoringSSL::str2bn(private_key.d()).ValueOrDie());
  auto p = std::move(SubtleUtilBoringSSL::str2bn(private_key.p()).ValueOrDie());
  auto q = std::move(SubtleUtilBoringSSL::str2bn(private_key.q()).ValueOrDie());
  auto dp =
      std::move(SubtleUtilBoringSSL::str2bn(private_key.dp()).ValueOrDie());
  auto dq =
      std::move(SubtleUtilBoringSSL::str2bn(private_key.dq()).ValueOrDie());
  bssl::UniquePtr<BN_CTX> ctx(BN_CTX_new());

  // Check n = p * q.
  auto n_calc = bssl::UniquePtr<BIGNUM>(BN_new());
  EXPECT_TRUE(BN_mul(n_calc.get(), p.get(), q.get(), ctx.get()));
  EXPECT_TRUE(BN_equal_consttime(n_calc.get(), n.get()));

  // Check n size >= modulus_size_in_bits bit.
  EXPECT_GE(BN_num_bits(n.get()), key_format.modulus_size_in_bits());

  // dp = d mod (p - 1)
  auto pm1 = bssl::UniquePtr<BIGNUM>(BN_dup(p.get()));
  EXPECT_TRUE(BN_sub_word(pm1.get(), 1));
  auto dp_calc = bssl::UniquePtr<BIGNUM>(BN_new());
  EXPECT_TRUE(BN_mod(dp_calc.get(), d.get(), pm1.get(), ctx.get()));
  EXPECT_TRUE(BN_equal_consttime(dp_calc.get(), dp.get()));

  // dq = d mod (q - 1)
  auto qm1 = bssl::UniquePtr<BIGNUM>(BN_dup(q.get()));
  EXPECT_TRUE(BN_sub_word(qm1.get(), 1));
  auto dq_calc = bssl::UniquePtr<BIGNUM>(BN_new());
  EXPECT_TRUE(BN_mod(dq_calc.get(), d.get(), qm1.get(), ctx.get()));
  EXPECT_TRUE(BN_equal_consttime(dq_calc.get(), dq.get()));
}

TEST(RawJwtRsaSsaPssSignKeyManagerTest, CreatePs256KeyValid) {
  JwtRsaSsaPssKeyFormat key_format =
      CreateKeyFormat(JwtRsaSsaPssAlgorithm::PS256, 2048, RSA_F4);
  StatusOr<JwtRsaSsaPssPrivateKey> private_key_or =
      RawJwtRsaSsaPssSignKeyManager().CreateKey(key_format);
  ASSERT_THAT(private_key_or.status(), IsOk());
  CheckNewKey(private_key_or.ValueOrDie(), key_format);
  EXPECT_THAT(
      RawJwtRsaSsaPssSignKeyManager().ValidateKey(private_key_or.ValueOrDie()),
      IsOk());
}

TEST(RawJwtRsaSsaPssSignKeyManagerTest, CreatePs384KeyValid) {
  JwtRsaSsaPssKeyFormat key_format =
      CreateKeyFormat(JwtRsaSsaPssAlgorithm::PS384, 3072, RSA_F4);
  StatusOr<JwtRsaSsaPssPrivateKey> private_key_or =
      RawJwtRsaSsaPssSignKeyManager().CreateKey(key_format);
  ASSERT_THAT(private_key_or.status(), IsOk());
  CheckNewKey(private_key_or.ValueOrDie(), key_format);
  EXPECT_THAT(
      RawJwtRsaSsaPssSignKeyManager().ValidateKey(private_key_or.ValueOrDie()),
      IsOk());
}

TEST(RawJwtRsaSsaPssSignKeyManagerTest, CreatePs512KeyValid) {
  JwtRsaSsaPssKeyFormat key_format =
      CreateKeyFormat(JwtRsaSsaPssAlgorithm::PS512, 4096, RSA_F4);
  StatusOr<JwtRsaSsaPssPrivateKey> private_key_or =
      RawJwtRsaSsaPssSignKeyManager().CreateKey(key_format);
  ASSERT_THAT(private_key_or.status(), IsOk());
  CheckNewKey(private_key_or.ValueOrDie(), key_format);
  EXPECT_THAT(
      RawJwtRsaSsaPssSignKeyManager().ValidateKey(private_key_or.ValueOrDie()),
      IsOk());
}

// Check that in a bunch of CreateKey calls all generated primes are distinct.
TEST(RawJwtRsaSsaPssSignKeyManagerTest, CreateKeyAlwaysNewRsaPair) {
  JwtRsaSsaPssKeyFormat key_format =
      CreateKeyFormat(JwtRsaSsaPssAlgorithm::PS256, 2048, RSA_F4);
  absl::flat_hash_set<std::string> keys;
  // This test takes about a second per key.
  int num_generated_keys = 5;
  for (int i = 0; i < num_generated_keys; ++i) {
    StatusOr<JwtRsaSsaPssPrivateKey> key_or =
        RawJwtRsaSsaPssSignKeyManager().CreateKey(key_format);
    ASSERT_THAT(key_or.status(), IsOk());
    keys.insert(key_or.ValueOrDie().p());
    keys.insert(key_or.ValueOrDie().q());
  }
  EXPECT_THAT(keys, SizeIs(2 * num_generated_keys));
}

TEST(RawJwtRsaSsaPssSignKeyManagerTest, GetPublicKey) {
  JwtRsaSsaPssKeyFormat key_format =
      CreateKeyFormat(JwtRsaSsaPssAlgorithm::PS256, 2048, RSA_F4);
  StatusOr<JwtRsaSsaPssPrivateKey> key_or =
      RawJwtRsaSsaPssSignKeyManager().CreateKey(key_format);
  ASSERT_THAT(key_or.status(), IsOk());
  StatusOr<JwtRsaSsaPssPublicKey> public_key_or =
      RawJwtRsaSsaPssSignKeyManager().GetPublicKey(key_or.ValueOrDie());
  ASSERT_THAT(public_key_or.status(), IsOk());
  EXPECT_THAT(public_key_or.ValueOrDie().version(),
              Eq(key_or.ValueOrDie().public_key().version()));
  EXPECT_THAT(public_key_or.ValueOrDie().n(),
              Eq(key_or.ValueOrDie().public_key().n()));
  EXPECT_THAT(public_key_or.ValueOrDie().e(),
              Eq(key_or.ValueOrDie().public_key().e()));
}

TEST(RawJwtRsaSsaPssSignKeyManagerTest, Create) {
  JwtRsaSsaPssKeyFormat key_format =
      CreateKeyFormat(JwtRsaSsaPssAlgorithm::PS256, 3072, RSA_F4);
  StatusOr<JwtRsaSsaPssPrivateKey> key_or =
      RawJwtRsaSsaPssSignKeyManager().CreateKey(key_format);
  ASSERT_THAT(key_or.status(), IsOk());
  JwtRsaSsaPssPrivateKey key = key_or.ValueOrDie();

  auto signer_or =
      RawJwtRsaSsaPssSignKeyManager().GetPrimitive<PublicKeySign>(key);
  ASSERT_THAT(signer_or.status(), IsOk());

  subtle::SubtleUtilBoringSSL::RsaSsaPssParams params;
  params.sig_hash = subtle::HashType::SHA256;
  params.mgf1_hash = subtle::HashType::SHA256;
  params.salt_length = 32;
  auto direct_verifier_or = subtle::RsaSsaPssVerifyBoringSsl::New(
      {key.public_key().n(), key.public_key().e()}, params);

  ASSERT_THAT(direct_verifier_or.status(), IsOk());

  std::string message = "Some message";
  EXPECT_THAT(direct_verifier_or.ValueOrDie()->Verify(
                  signer_or.ValueOrDie()->Sign(message).ValueOrDie(), message),
              IsOk());
}

TEST(RawJwtRsaSsaPssSignKeyManagerTest, CreateWrongKey) {
  JwtRsaSsaPssKeyFormat key_format =
      CreateKeyFormat(JwtRsaSsaPssAlgorithm::PS256, 3072, RSA_F4);
  StatusOr<JwtRsaSsaPssPrivateKey> key_or =
      RawJwtRsaSsaPssSignKeyManager().CreateKey(key_format);
  ASSERT_THAT(key_or.status(), IsOk());
  JwtRsaSsaPssPrivateKey key = key_or.ValueOrDie();

  auto signer_or =
      RawJwtRsaSsaPssSignKeyManager().GetPrimitive<PublicKeySign>(key);

  StatusOr<JwtRsaSsaPssPrivateKey> second_key_or =
      RawJwtRsaSsaPssSignKeyManager().CreateKey(key_format);
  ASSERT_THAT(second_key_or.status(), IsOk());
  JwtRsaSsaPssPrivateKey second_key = second_key_or.ValueOrDie();

  ASSERT_THAT(signer_or.status(), IsOk());

  subtle::SubtleUtilBoringSSL::RsaSsaPssParams params;
  params.sig_hash = subtle::HashType::SHA256;
  params.mgf1_hash = subtle::HashType::SHA256;
  params.salt_length = 32;
  auto direct_verifier_or = subtle::RsaSsaPssVerifyBoringSsl::New(
      {second_key.public_key().n(), second_key.public_key().e()}, params);

  ASSERT_THAT(direct_verifier_or.status(), IsOk());

  std::string message = "Some message";
  EXPECT_THAT(direct_verifier_or.ValueOrDie()->Verify(
                  signer_or.ValueOrDie()->Sign(message).ValueOrDie(), message),
              Not(IsOk()));
}

}  // namespace
}  // namespace tink
}  // namespace crypto
