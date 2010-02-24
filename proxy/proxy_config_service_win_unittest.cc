// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/proxy/proxy_config_service_win.h"

#include "net/base/net_errors.h"
#include "net/proxy/proxy_config.h"
#include "net/proxy/proxy_config_service_common_unittest.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

TEST(ProxyConfigServiceWinTest, SetFromIEConfig) {
  const struct {
    // Input.
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie_config;

    // Expected outputs (fields of the ProxyConfig).
    bool auto_detect;
    GURL pac_url;
    ProxyRulesExpectation proxy_rules;
    const char* proxy_bypass_list;  // newline separated
  } tests[] = {
    // Auto detect.
    {
      { // Input.
        TRUE,  // fAutoDetect
        NULL,  // lpszAutoConfigUrl
        NULL,  // lpszProxy
        NULL,  // lpszProxyBypass
      },

      // Expected result.
      true,                       // auto_detect
      GURL(),                     // pac_url
      ProxyRulesExpectation::Empty(),
    },

    // Valid PAC url
    {
      { // Input.
        FALSE,                    // fAutoDetect
        L"http://wpad/wpad.dat",  // lpszAutoConfigUrl
        NULL,                     // lpszProxy
        NULL,                     // lpszProxy_bypass
      },

      // Expected result.
      false,                         // auto_detect
      GURL("http://wpad/wpad.dat"),  // pac_url
      ProxyRulesExpectation::Empty(),
    },

    // Invalid PAC url string.
    {
      { // Input.
        FALSE,        // fAutoDetect
        L"wpad.dat",  // lpszAutoConfigUrl
        NULL,         // lpszProxy
        NULL,         // lpszProxy_bypass
      },

      // Expected result.
      false,                      // auto_detect
      GURL(),                     // pac_url
      ProxyRulesExpectation::Empty(),
    },

    // Single-host in proxy list.
    {
      { // Input.
        FALSE,              // fAutoDetect
        NULL,               // lpszAutoConfigUrl
        L"www.google.com",  // lpszProxy
        NULL,               // lpszProxy_bypass
      },

      // Expected result.
      false,                                   // auto_detect
      GURL(),                                  // pac_url
      ProxyRulesExpectation::Single(
          "www.google.com:80",  // single proxy
          ""),                  // bypass rules
    },

    // Per-scheme proxy rules.
    {
      { // Input.
        FALSE,                                            // fAutoDetect
        NULL,                                             // lpszAutoConfigUrl
        L"http=www.google.com:80;https=www.foo.com:110",  // lpszProxy
        NULL,                                             // lpszProxy_bypass
      },

      // Expected result.
      false,                                   // auto_detect
      GURL(),                                  // pac_url
      ProxyRulesExpectation::PerScheme(
          "www.google.com:80",  // http
          "www.foo.com:110",   // https
          "",                  // ftp
          ""),                 // bypass rules
    },

    // SOCKS proxy configuration
    {
      { // Input.
        FALSE,                                            // fAutoDetect
        NULL,                                             // lpszAutoConfigUrl
        L"http=www.google.com:80;https=www.foo.com:110;"
        L"ftp=ftpproxy:20;socks=foopy:130",               // lpszProxy
        NULL,                                             // lpszProxy_bypass
      },

      // Expected result.
      false,                                   // auto_detect
      GURL(),                                  // pac_url
      ProxyRulesExpectation::PerSchemeWithSocks(
          "www.google.com:80",   // http
          "www.foo.com:110",     // https
          "ftpproxy:20",         // ftp
          "socks4://foopy:130",  // socks
          ""),                   // bypass rules
    },

    // Bypass local names.
    {
      { // Input.
        TRUE,            // fAutoDetect
        NULL,            // lpszAutoConfigUrl
        NULL,            // lpszProxy
        L"<local>",      // lpszProxy_bypass
      },

      true,                       // auto_detect
      GURL(),                     // pac_url
      ProxyRulesExpectation::EmptyWithBypass("<local>"),
    },

    // Bypass "google.com" and local names, using semicolon as delimeter
    // (ignoring white space).
    {
      { // Input.
        TRUE,                         // fAutoDetect
        NULL,                         // lpszAutoConfigUrl
        NULL,                         // lpszProxy
        L"<local> ; google.com",      // lpszProxy_bypass
      },

      // Expected result.
      true,                       // auto_detect
      GURL(),                     // pac_url
      ProxyRulesExpectation::EmptyWithBypass("<local>,google.com"),
    },

    // Bypass "foo.com" and "google.com", using lines as delimeter.
    {
      { // Input.
        TRUE,                      // fAutoDetect
        NULL,                      // lpszAutoConfigUrl
        NULL,                      // lpszProxy
        L"foo.com\r\ngoogle.com",  // lpszProxy_bypass
      },

      // Expected result.
      true,                       // auto_detect
      GURL(),                     // pac_url
      ProxyRulesExpectation::EmptyWithBypass("foo.com,google.com"),
    },
  };

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(tests); ++i) {
    ProxyConfig config;
    ProxyConfigServiceWin::SetFromIEConfig(&config, tests[i].ie_config);

    EXPECT_EQ(tests[i].auto_detect, config.auto_detect());
    EXPECT_EQ(tests[i].pac_url, config.pac_url());
    EXPECT_TRUE(tests[i].proxy_rules.Matches(config.proxy_rules()));
  }
}

}  // namespace net
