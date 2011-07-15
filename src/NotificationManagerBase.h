/*
 * Copyright (C) 2011 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#ifndef __NOTIFICATION_MANAGER_BASE_H
#define __NOTIFICATION_MANAGER_BASE_H

#include "syncevo/declarations.h"

#include <string>

SE_BEGIN_CXX

class NotificationManagerBase {
    public:
        /** Initializes the backend. */
        virtual bool init() { return true; }

        /** Publishes the notification through the backend.
         *  @param summary    The summary of the notification.
         *  @param body       The main content of the notification.
         *  @param viewParams Parameters used by sync-ui (opened when the
         *  notification activated.
         */
        virtual void publish(const std::string& summary,
                             const std::string& body,
                             const std::string& viewParams = std::string()) {}
};

SE_END_CXX

#endif

