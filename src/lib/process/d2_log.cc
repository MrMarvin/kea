// Copyright (C) 2013-2016 Internet Systems Consortium, Inc. ("ISC")
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

/// Defines the logger used by the top-level component of kea-dhcp-ddns.

#include <process/d2_log.h>

namespace isc {
namespace d2 {

/// @brief Defines the logger used within libkea-process library.
isc::log::Logger dctl_logger("dhcpddns");

} // namespace d2
} // namespace isc

