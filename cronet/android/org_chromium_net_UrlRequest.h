// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_CRONET_ANDROID_URLREQUEST_H_
#define NET_CRONET_ANDROID_URLREQUEST_H_

#include <jni.h>

namespace net {

// Define request priority values like REQUEST_PRIORITY_IDLE in a
// way that ensures they're always the same than their Java counterpart.
enum UrlRequestPriority {
#define DEFINE_REQUEST_PRIORITY(x, y) REQUEST_PRIORITY_##x = y,
#include "net/cronet/android/org_chromium_net_UrlRequest_priority_list.h"
#undef DEFINE_REQUEST_PRIORITY
};

// Define request priority values like REQUEST_ERROR_SUCCESS in a
// way that ensures they're always the same than their Java counterpart.
enum UrlRequestError {
#define DEFINE_REQUEST_ERROR(x, y) REQUEST_ERROR_##x = y,
#include "net/cronet/android/org_chromium_net_UrlRequest_error_list.h"
#undef DEFINE_REQUEST_ERROR
};

bool UrlRequestRegisterJni(JNIEnv* env);

}  // namespace net

#endif  // NET_CRONET_ANDROID_URLREQUEST_H_
