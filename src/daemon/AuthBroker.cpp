/*
 * The authentication broker. See AuthBroker.h for the design.
 *
 * Copyright (C) 2026 Lana Requista
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "AuthBroker.h"
#include "AuthChannel.h"
#include "Display.h"

#include <QDebug>
#include <QLocalSocket>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusMetaType>

namespace SDDM {
    AuthBroker::AuthBroker(Display *display)
        : QObject(display)
        , m_display(display)
    {
    }

    AuthBroker::State AuthBroker::state() const {
        return m_state;
    }

    bool AuthBroker::hasRunningSession() const {
        // a channel of this broker holds a running user session
        // (the winner, after the go-ahead): the display terminal
        // belongs to it
        if (m_fingerprintChannel && m_fingerprintChannel->state() == AuthChannel::SessionRunning)
            return true;
        if (m_keyboardChannel && m_keyboardChannel->state() == AuthChannel::SessionRunning)
            return true;
        return false;
    }

    void AuthBroker::userChosen(const QString &user, const Session &session, QLocalSocket *socket) {
        // a different user ends the current channels first: a PAM
        // conversation belongs to one user and cannot be recycled
        if (user != m_user && (m_fingerprintChannel || m_keyboardChannel))
            stop();

        // E3, the burst lesson: confirming the user whose channels
        // already run restarts nothing. The channels live, the
        // greeter keeps its field ready, the user keeps waiting for
        // whatever he is waiting for
        if (user == m_user && m_state == Running) {
            m_socket = socket;
            return;
        }

        // once the race is decided nothing new may spawn here: the
        // winner is opening its session, the greeter is leaving
        if (m_state == Resolved) {
            qWarning() << "AuthBroker: login request after a resolved race, ignoring";
            return;
        }

        m_user = user;
        m_session = session;
        m_socket = socket;
        m_state = Running;
        m_dead = false;

        spawnChannels();
    }

    void AuthBroker::stop() {
        // user switch, greeter cancellation or display teardown: the
        // channels die quietly (no failure counted, nothing
        // reported), the state returns to idle and nothing is
        // reborn until the next user choice
        m_dead = true;
        m_winner = nullptr;
        if (m_fingerprintChannel) {
            m_fingerprintChannel->requestShutdown();
            m_fingerprintChannel->deleteLater();
            m_fingerprintChannel = nullptr;
        }
        if (m_keyboardChannel) {
            m_keyboardChannel->requestShutdown();
            m_keyboardChannel->deleteLater();
            m_keyboardChannel = nullptr;
        }
        m_state = Idle;
    }

    void AuthBroker::submitPassword(const QString &password) {
        if (m_state != Running) {
            qWarning() << "AuthBroker: password submitted without a running race, ignoring";
            return;
        }

        // the password targets the keyboard channel only (E2): the
        // fingerprint conversation is the sensor's
        if (m_keyboardChannel) {
            m_keyboardChannel->submit(password);
        } else {
            qWarning() << "AuthBroker: password submitted without a keyboard channel, ignoring";
        }
    }

    void AuthBroker::onStateChanged(AuthChannel *channel) {
        if (channel->state() == AuthChannel::Dead) {
            channelDied(channel);
            return;
        }

        // the loser ends up dead through its own shutdown; nothing
        // else is interpreted here — the channel reports, the broker
        // decides only on authentication and death
    }

    void AuthBroker::onAuthenticated(AuthChannel *channel, const QString &user) {
        // the guard of the double victory: the first state to this
        // slot wins, and once the race is resolved everything later
        // is noise — including the second channel authenticating in
        // the same fraction of second, and the states pushed by the
        // losers shutting down inside the grace period
        if (m_state == Resolved) {
            qWarning() << "AuthBroker: extra authentication after the race was resolved, ignoring";
            return;
        }

        declareWinner(channel, user);
    }

    void AuthBroker::onInfo(AuthChannel *channel, const QString &text, Auth::Info type) {
        Q_UNUSED(channel)
        Q_UNUSED(type)
        // shown verbatim: the truth on screen comes from the PAM
        // conversation, never from a second source
        Q_EMIT message(m_socket, text);
    }

    void AuthBroker::onError(AuthChannel *channel, const QString &text, Auth::Error type) {
        Q_UNUSED(channel)

        Q_EMIT message(m_socket, text);

        if (type == Auth::ERROR_AUTHENTICATION && m_state == Running && m_socket)
            Q_EMIT loginFailed(m_socket);
    }

    void AuthBroker::onSessionFinished(AuthChannel *channel, Auth::HelperExitStatus status) {
        // the winner's session is over (logout) or failed to start:
        // the display cycle restarts a whole greeter from scratch
        Q_UNUSED(channel);
        m_winner = nullptr;
        m_state = Idle;
        Q_EMIT sessionFinished(status);
    }

    void AuthBroker::spawnChannels() {
        // the keyboard channel always exists: SDDM always offers a
        // way to authenticate (the invariant of the open channel)
        if (!m_keyboardChannel)
            m_keyboardChannel = spawnChannel(AuthChannel::Keyboard);

        // the fingerprint channel exists when the system has
        // something to verify: a birth decision, never a display
        // decision
        if (!m_fingerprintChannel && fingerprintAvailable(m_user))
            m_fingerprintChannel = spawnChannel(AuthChannel::Fingerprint);
    }

    AuthChannel *AuthBroker::spawnChannel(AuthChannel::Type type) {
        // one method, one rule: the keyboard rule carries no
        // pam_fprintd (the password prompt is immediate), the
        // fingerprint rule is the current sddm stack
        QString service = (type == AuthChannel::Fingerprint)
                              ? QStringLiteral("sddm-fingerprint")
                              : QStringLiteral("sddm-password");

        AuthChannel *channel = new AuthChannel(type, service, m_display);

        connect(channel, &AuthChannel::stateChanged, this, &AuthBroker::onStateChanged);
        connect(channel, &AuthChannel::authenticated, this, &AuthBroker::onAuthenticated);
        connect(channel, &AuthChannel::info, this, &AuthBroker::onInfo);
        connect(channel, &AuthChannel::error, this, &AuthBroker::onError);
        connect(channel, &AuthChannel::sessionStarted, this, [this](AuthChannel *, bool success) {
            Q_EMIT sessionStarted(success);
        });
        connect(channel, &AuthChannel::sessionFinished, this, &AuthBroker::onSessionFinished);

        if (!channel->start(m_user, m_session)) {
            // stillborn: a non-event, it is already dead and already
            // reported as such to onStateChanged through the channels
            // of the broker; nothing else moves
            channel->deleteLater();
            return nullptr;
        }

        return channel;
    }

    void AuthBroker::channelDied(AuthChannel *channel) {
        // nothing is ever reborn during a teardown
        if (m_dead) {
            if (m_fingerprintChannel == channel)
                m_fingerprintChannel = nullptr;
            if (m_keyboardChannel == channel)
                m_keyboardChannel = nullptr;
            channel->deleteLater();
            return;
        }

        // the loser of a resolved race dies without consequences: it
        // was asked to
        if (m_state == Resolved) {
            if (m_fingerprintChannel == channel)
                m_fingerprintChannel = nullptr;
            if (m_keyboardChannel == channel)
                m_keyboardChannel = nullptr;
            channel->deleteLater();
            return;
        }

        // a channel is always open: a dead one is recreated at once.
        // The user never faces a screen where he cannot authenticate —
        // SDDM does not own the right to lock him out, the counters
        // and locks of the system are shown verbatim, they are not
        // implemented twice here.
        if (channel->type() == AuthChannel::Keyboard) {
            m_keyboardChannel = nullptr;
            channel->deleteLater();
            m_keyboardChannel = spawnChannel(AuthChannel::Keyboard);
        } else if (channel->type() == AuthChannel::Fingerprint) {
            m_fingerprintChannel = nullptr;
            channel->deleteLater();
            // reborn only if the method still has something to offer
            if (fingerprintAvailable(m_user))
                m_fingerprintChannel = spawnChannel(AuthChannel::Fingerprint);
        }
    }

    void AuthBroker::declareWinner(AuthChannel *channel, const QString &user) {
        m_state = Resolved;
        m_winner = channel;
        channel->markWinner();

        // the cookie must sit in the winner's bundle before the
        // authenticated reply carries it to the helper: this slot
        // runs synchronously, before the reply is composed
        m_display->channelAuthenticated(channel, user, m_socket);

        // the losers are shut down before the winner opens anything:
        // one go-ahead, one session
        if (m_fingerprintChannel && m_fingerprintChannel != channel) {
            m_fingerprintChannel->requestShutdown();
            m_fingerprintChannel->deleteLater();
            m_fingerprintChannel = nullptr;
        }
        if (m_keyboardChannel && m_keyboardChannel != channel) {
            m_keyboardChannel->requestShutdown();
            m_keyboardChannel->deleteLater();
            m_keyboardChannel = nullptr;
        }

        // the greeter may already be gone: reporting to a null socket
        // would crash the daemon (SocketWriter dereferences it)
        if (m_socket)
            Q_EMIT loginSucceeded(m_socket);
    }

    bool AuthBroker::fingerprintAvailable(const QString &user) const {
        // a birth decision only: does the system have enrolled
        // fingers for this user? What the greeter displays always
        // comes from the conversation itself, never from here
        QDBusInterface device(QStringLiteral("net.reactivated.fprint"),
                              QStringLiteral("/net/reactivated/fprint/Device"),
                              QStringLiteral("net.reactivated.Fprint.Device"),
                              QDBusConnection::systemBus());
        if (!device.isValid())
            return false;

        QDBusReply<QStringList> reply = device.call(QStringLiteral("ListEnrolledFingers"), user);
        return reply.isValid() && !reply.value().isEmpty();
    }
}
