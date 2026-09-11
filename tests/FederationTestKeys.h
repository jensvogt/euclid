// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/9/26.
//

#pragma once

// C++ includes
#include <string>

// RSA key pairs used by the federation tests, and nothing else - generated for these files, valid
// for nothing, and deliberately checked in so that the tests need no key material at run time.
//
// Two of them: the one the "identity provider" signs with and publishes, and one it does not, for
// the case that matters most - a token or an assertion signed by the wrong hands.
namespace Euclid::Test {

    // The key the "provider" signs with, and which the key set below publishes.
    constexpr auto kProviderKey =
            "-----BEGIN PRIVATE KEY-----\n"
            "MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQCJ7DyIhgmjer+M\n"
            "pPts1gCoNeoorUOWLP1jL5j/pTRKp59zX+e5ySYpS9B+SaHVTTzmoVlyEgvMQ65K\n"
            "PeiCKSs37QNc1Ekdkcwj8gBGI7TbH/JuKHjhxRpG2sJaZdKIHLaHWKxi2J3YqsJ/\n"
            "IWx0yoU5r0XW2nBYXY9naUOaMziIx2VdK04qqzcH6LwY1ov9vTIt/qHGBGJtV5/M\n"
            "lkLEegV+AiS++ncEZUkeEmr0ZNx3+kpb7lXAwulkLVNnTd02InNeqQytOQgRh2OW\n"
            "w1YzTmzR00NPzbA/pzPzVUvE3pf9n8UEj6k+NLVwEjU0ELaw2Q25F642b628gV1Z\n"
            "qXhiYZG9AgMBAAECggEAFsm/wa3l00tBcV9EBHpCXwiTcReZmmtCDXyMira00l3q\n"
            "OEGhE58CzWFvqCIJ2Wia1p9TnEeTh/Mpza33Z+Qd9i5UtyaDEx3nDQxb07rsE6Vu\n"
            "ZJmOyXzLx+e2o4F93Mvjs66owuc4A1fJbYBr/0sD4qFNSH/Mfqksu8EWcJikb2LG\n"
            "iOZ83Xsp5v8dyDQJ7QQm+B916lo0CwFJ7pl9V6m3zhrBb8J4WkVElmcTVm5c9OV3\n"
            "yECOjMZpG/a7hfBOEMsJr6VRyqeX8BZWnlbTdcSp6Dre6Ns1sLznf66YyxuEqURg\n"
            "r35Sb3aimZ7boBqmVGWAG40+DkuE3a4FIIeXI9/jsQKBgQC+mJnYK5y7Ijc/zVdW\n"
            "iL30UwfBOlEnb8lWH2mXksmRgsyykA7Ki9uDmu/DRqAhSKfp7bbD3iAwzZsgOECz\n"
            "YOD2i8/N5MvTi1OZxJNNKFJob3Mytci1ScliKOdwyCh9+8ZehqbfkK4bE/3r869c\n"
            "wxkhYQPzEKyNmtYYaBBBI3hhEQKBgQC5QGpi75CHXx+Vwu8VwIHegdGHKokArEFj\n"
            "YjtMaopldSJjxG2TwmkE4tn8bcVoWvjsaKc5Jkgm3NmrapXv+SFAnYMvNyHfxRC9\n"
            "h2vrQ4ViT1xR+lPiSRPjhST2VH37NPrQNyMhtqBRYg5DcsQzEPlj/QYj7Wd2UzaW\n"
            "5yrT82Rl7QKBgQCTM9jeFBDglvSE2lryAnpzEQ2UNy4mfzfIxEoRPtVfBN6Dqq9B\n"
            "z/KiGi/DafCSxEKrcWUOsae7CZEduVR/NfuJb0H3ixXBNIJE6IW7ucCr2Cfoqm1x\n"
            "VqOqpFzbxjs/0gzLRMPtNxjA4Rfj/DkcM7JdDUi3mdErLMLd5sFnDbYTgQKBgQCB\n"
            "bjUmbNThECAERc8/tmHGcZNQSfKwyqlg47gkmueZHW7qn8oji5hOdqTxDkz3rV/v\n"
            "5Oq5LjsJFBLtBio+ISUPfT2z1mRPcONSWHSZSnK+Dk6lZC4Jkx9XPqZliQEAa7K1\n"
            "mWBtZKi8U7M9gsj8GOb16knI/kzkSh2A74Bn180UvQKBgQCOs6Yq9Lp4GaxImL9C\n"
            "FyU1Iizd8/q5RND9v4pMTfMbBc1EXtTKY+UznwLHAL+dQqWQmTm2rqAWH471cCYM\n"
            "rDjGK+cLI0LtdaEMLVpjIL4Ubjci/s9wlRmY3wBi6ZvHgI9FlYypE7zm5b00cRqX\n"
            "7rYKY7HjO/l6dEymUy0q/KBvlw==\n"
            "-----END PRIVATE KEY-----\n";

    // A self-signed certificate over kProviderKey, which is all a SAML deployment ever uses a
    // certificate for: a container for a public key. Checked in rather than generated at run time
    // so the tests need no openssl binary, no temporary files and no shell - none of which a
    // Windows runner has in the shape a POSIX one does. Valid until 2046 and protecting nothing.
    constexpr auto kProviderCertificate =
            "-----BEGIN CERTIFICATE-----\n"
            "MIIDBDCCAeygAwIBAgIBATANBgkqhkiG9w0BAQsFADAbMRkwFwYDVQQDDBBldWNs\n"
            "aWQtc2FtbC10ZXN0MB4XDTI2MDkwOTIxMTIwOVoXDTQ2MDkwNDIxMTIwOVowGzEZ\n"
            "MBcGA1UEAwwQZXVjbGlkLXNhbWwtdGVzdDCCASIwDQYJKoZIhvcNAQEBBQADggEP\n"
            "ADCCAQoCggEBAInsPIiGCaN6v4yk+2zWAKg16iitQ5Ys/WMvmP+lNEqnn3Nf57nJ\n"
            "JilL0H5JodVNPOahWXISC8xDrko96IIpKzftA1zUSR2RzCPyAEYjtNsf8m4oeOHF\n"
            "Gkbawlpl0ogctodYrGLYndiqwn8hbHTKhTmvRdbacFhdj2dpQ5ozOIjHZV0rTiqr\n"
            "NwfovBjWi/29Mi3+ocYEYm1Xn8yWQsR6BX4CJL76dwRlSR4SavRk3Hf6SlvuVcDC\n"
            "6WQtU2dN3TYic16pDK05CBGHY5bDVjNObNHTQ0/NsD+nM/NVS8Tel/2fxQSPqT40\n"
            "tXASNTQQtrDZDbkXrjZvrbyBXVmpeGJhkb0CAwEAAaNTMFEwHQYDVR0OBBYEFGJJ\n"
            "59VHz8qC8tLAkRiqraPgq7ICMB8GA1UdIwQYMBaAFGJJ59VHz8qC8tLAkRiqraPg\n"
            "q7ICMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBAD7VoGO5c6ik\n"
            "lVoiuPQqg8fih8rAoyd3LoJoe0S4/pFbOKvK+GYs/tfoSgVfghjj4J56hfeKI+SL\n"
            "pxlV7SFwhYJEl0iUEanqtOd6zzWi0QSwHmSNsgLbAVCyeGKUX6/o+5sNL+6lp4Bb\n"
            "0XE+nj/owor+S9tqaQuHsLXvoElUdRuqWlHiEM0rMkBu6Bti42YcinzRppxd22Tc\n"
            "GxX+WcGEXP5Tt8lkl0OiJQzOHVf9U9kGzryzaa5iCTNpbyzHKDaXQoHcJE+ZR2fq\n"
            "99QFQGUs6GkNTnUBdMwifixeLkF34gaLfjyIqR43CV5kODaxFpE/5Q+3J3jf6MXc\n"
            "XXbRElhC8SA=\n"
            "-----END CERTIFICATE-----\n";

    // A different key, published by nobody - for the token that is signed by the wrong hands.
    constexpr auto kStrangerKey =
            "-----BEGIN PRIVATE KEY-----\n"
            "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC8tBBs8qZaRua1\n"
            "JtHael9nRUBLNF2puSxhFbpDj0HNUgJZfIllVKTJezBIepEXyrBeP3f+0bq7PCF/\n"
            "I+mATL97R8Wf4RUxBa6ADAxzjOcN9E8Rf1cNm+JwDZSfPkWLSkttq8Y86RukhNWO\n"
            "5biP/1boTtRf1VJBdxWjU3UfvFGEL/Lj16cs8C3X+1FXEegQRd32f3TvvCXX8G+H\n"
            "i+aiN7/GP3uOqPyCmfO3KXYVK4WyXeT88BCmWdpgqBwlUV/FKsAIWHEw1kN9vYwe\n"
            "ISvTYrLFvN6Du08ireoVI1zu65Z+4fUGP5/HiarF9lN/L4ezAGhSbwUQwsN68R8o\n"
            "/isldBURAgMBAAECggEAEtyhxq4TdDMhlOpiMAcq2Zj5tfqwQQqGFdc05NNhNat0\n"
            "mirBVgxZo10JpLkBn0HukqEf9qvSkSwPwhReHums+x7fsV0vf9SsTlbF+NfU8Zdh\n"
            "vuM+WZkjD2XAHRG9pFBjDT2OBWEclvuDGDM8mUIh1vsoLcMWI36KfmaVbrxlXoO7\n"
            "ckQobwkmgcIviA1a6bBwmmiA2kbYP6NaZeokM66+udvE6ydJjkVP9vW1dvfFd8SN\n"
            "bQQeCbIe/W+cegUSVjAuYBGJp6njsA+TWb0IZWmyNRQbmiV9ZSzbsalq/DCA+4CB\n"
            "EONbQ1KGf9EFT+drujNpPDZfpfCPHt1GYI337AUYVQKBgQDnFivtQ9EBRqnSibTA\n"
            "zuP9x+zHp+juTBeuag3HRZ6sQ/Uj7OkBavTZqAYHUUxi1Ls0YyQ/1OAZ7XThbGv5\n"
            "beTpJTiszwiKP+mt4NR+eegM+DxclWaf5k8vzmKo8hvdqFFyZaHUk8NrKIZzKlmi\n"
            "ZORXsPkl7/WRYnmXUdde1Kj/lQKBgQDRDCUjbW6P+svQBQb8Ax/q6kmChsPAEjey\n"
            "UqsltZCC+CoDQyFjWCuarbxai1p7vaNsvw6INMK3NxoGyk3a56a8CiN7D6ChtTSG\n"
            "YuEHBQFSF++kfyLixnUREjc8NudMvnj2U5L9/SPIT7yGlBib6zksAsYd9Km1it3P\n"
            "z96IAU4QjQKBgQDfw1Rdt2FW/vnKaJWibbPPNhxNaIXg2cXEmeUlpIB1BokFdI5b\n"
            "VRoq+Mx9oXd568wqeiXLuXIXKRwYfEhBiuwx5lzu2LrIRAMe36pjnd/ZTzYD4Jcz\n"
            "FmxvOkXGmknpZOcZAtilYz/DL8ahFc0dttx1HWTkG2uKj/R1VkZaCUpr0QKBgBo0\n"
            "MoeRVRWZM2kc6DY40gxpV9OTvv/ZiL/Cmw98BeRU5TdJd+rzB2w5MO4Mn8f5Cn48\n"
            "KL8sFehPcOS/ASladk0F3FK1r8X1Z+Mci+aMWxEGTRTKlv3kMJJ2PRRAMZguHA2M\n"
            "hlVuIMkgLZqU/8yznJQGuPzSSV+nS4ycGuHjf/yVAoGATYRYD8epAlux4s2KYT6z\n"
            "WyXj4sfVYClXRTrgnku6bZZAL6YNx9NlltHj6xpDVIpSzhgxx3FEIKYk3n9DPTBA\n"
            "C9a6Sxcc4LDdjIIZ2SUg3C48xNTi89h1cK4+AzwPafdfCmu5T4GXpWPfgSfmoxtC\n"
            "jeatGPrxeK3WU0wFEEEfj3o=\n"
            "-----END PRIVATE KEY-----\n";

    // The key set the provider publishes: kProviderKey's modulus and exponent, as OneLogin
    // publishes them (n/e rather than a certificate chain).
    const std::string kJwks = R"({"keys":[{"kty":"RSA","use":"sig","alg":"RS256","kid":"test-key-1",)"
                              R"("n":"iew8iIYJo3q_jKT7bNYAqDXqKK1Dliz9Yy-Y_6U0Sqefc1_nuckmKUvQfkmh1U085qFZchILzEOuSj3ogikrN-0DXNRJHZHMI_IARiO02x_ybih44cUaRtrCWmXSiBy2h1isYtid2KrCfyFsdMqFOa9F1tpwWF2PZ2lDmjM4iMdlXStOKqs3B-i8GNaL_b0yLf6hxgRibVefzJZCxHoFfgIkvvp3BGVJHhJq9GTcd_pKW-5VwMLpZC1TZ03dNiJzXqkMrTkIEYdjlsNWM05s0dNDT82wP6cz81VLxN6X_Z_FBI-pPjS1cBI1NBC2sNkNuReuNm-tvIFdWal4YmGRvQ",)"
                              R"("e":"AQAB"}]})";

}// namespace Euclid::Test
