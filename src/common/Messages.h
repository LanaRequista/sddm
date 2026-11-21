/***************************************************************************
* Copyright (c) 2013 Abdurrahman AVCI <abdurrahmanavci@gmail.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the
* Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
***************************************************************************/

#ifndef SDDM_MESSAGES_H
#define SDDM_MESSAGES_H

#include <QFlags>

namespace SDDM {
    enum class GreeterMessages {
        Connect = 0,
        Login,
        PowerOff,
        Reboot,
        Suspend,
        Hibernate,
        HybridSleep,
        // fingerprint login: start the PAM conversation for the pre-selected
        // user before any password is available, queue a password typed while
        // the conversation is already running, and cancel the running one
        // when the user switches to another account
        LoginInteractive,
        LoginPassword,
        CancelLogin
    };

    enum class DaemonMessages {
        HostName,
        Capabilities,
        LoginSucceeded,
        LoginFailed,
        InformationMessage,
        // fingerprint login: the factor stage of the running race --
        // "fingerprint" while the sensor channel is available for the
        // selected user, "password" when only the keyboard remains,
        // "" when no race runs. The motor owns this truth; themes
        // display it, they never guess it from messages
        AuthStage,
        // fingerprint login: the sensor channel died of an honest
        // verify failure (a swipe that did not match) while the race
        // ran. A TYPED event, never text: the daemon alone knows
        // which channel spoke, so themes react to a code instead of
        // pattern-matching PAM sentences in one language among
        // hundreds
        SensorVerifyFailed
    };

    enum Capability {
        None = 0x0000,
        PowerOff = 0x0001,
        Reboot = 0x0002,
        Suspend = 0x0004,
        Hibernate = 0x0008,
        HybridSleep = 0x0010
    };

    Q_DECLARE_FLAGS(Capabilities, Capability)
    Q_DECLARE_OPERATORS_FOR_FLAGS(Capabilities)
}

#endif // SDDM_MESSAGES_H
