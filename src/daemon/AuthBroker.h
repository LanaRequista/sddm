/*
 * The authentication broker: the single authority coordinating the
 * authentication channels of the user on screen.
 *
 * It spawns one channel per applicable method (fingerprint, keyboard),
 * listens to their state, adjudicates the race (first authenticated
 * channel wins, no statistics), shuts the losers down and gives the
 * single go-ahead. Channels never coordinate anything and never talk
 * to each other; the greeter never sees them.
 *
 * The keyboard channel is LAZY (the airlock): it is born on the
 * first password submission, not at the opening of the race — a race
 * won by the sensor closes no open PAM door and generates no ghost
 * failure in the keyboard's faillock counter. The submitted password
 * waits in the broker until the newborn channel holds its prompt.
 *
 * Two stable properties, the project invariants:
 *  - the keyboard can always be BORN: a dead keyboard channel is not
 *    recreated on its own — only a submission (a human gesture)
 *    revives it; the screen never locks anyone out;
 *  - no security policy lives here: counters, delays and locks are
 *    the system's (PAM), ours are only shown verbatim.
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

#ifndef SDDM_AUTH_BROKER_H
#define SDDM_AUTH_BROKER_H

#include <QObject>
#include <QPointer>

#include "Auth.h"
#include "AuthChannel.h"
#include "Session.h"

class QLocalSocket;

namespace SDDM {
    class Display;

    class AuthBroker : public QObject {
        Q_OBJECT
    public:
        enum State {
            Idle,      // no channel runs
            Running,   // the channels converse, the race is open
            Resolved   // a channel won; every later message is ignored
        };
        Q_ENUM(State)

        explicit AuthBroker(Display *display);

        State state() const;
        bool hasRunningSession() const;

        // the user shown by the greeter changed (or is confirmed at
        // display): spawn the applicable channels. Confirming the
        // user already running is a no-op — nothing restarts for
        // free, the burst lesson. A different user shuts the current
        // channels down first: one PAM conversation belongs to one
        // user, it cannot be recycled.
        void userChosen(const QString &user, const Session &session, QLocalSocket *socket);

        // the greeter asks to stop: everything dies quietly, without
        // any failure reported (user switching, escape)
        void stop();

        // a password was submitted: it goes to the keyboard channel
        // only — the fingerprint conversation is the sensor's. With
        // no keyboard channel alive (the airlock), the submission
        // spawns one and the password waits for its held prompt
        void submitPassword(const QString &password);

    signals:
        // the greeter protocol, unchanged: the outcome and the
        // messages to display are reported to the display
        void loginSucceeded(QLocalSocket *socket);
        void loginFailed(QLocalSocket *socket);
        void message(QLocalSocket *socket, const QString &message);
        // the factor stage of the running race, the motor's truth:
        // "fingerprint" (sensor channel available), "password"
        // (keyboard only), "" (no race); published at every transition
        void authStage(QLocalSocket *socket, const QString &stage);

        // the winner's session started (the greeter may go) or
        // finished (logout: the display cycle takes over)
        void sessionStarted(bool success);
        void sessionFinished(Auth::HelperExitStatus status);

    private slots:
        void onStateChanged(AuthChannel *channel);
        void onAuthenticated(AuthChannel *channel, const QString &user);
        void onInfo(AuthChannel *channel, const QString &text, Auth::Info type);
        void onError(AuthChannel *channel, const QString &text, Auth::Error type);
        void onRequestChanged(AuthChannel *channel);
        void onSessionFinished(AuthChannel *channel, Auth::HelperExitStatus status);

    private:
        void spawnChannels();
        AuthChannel *spawnChannel(AuthChannel::Type type);
        void channelDied(AuthChannel *channel);
        void declareWinner(AuthChannel *channel, const QString &user);
        bool fingerprintAvailable(const QString &user) const;
        // the single computation of the factor stage from the live
        // state: called after any transition (race start, channel
        // birth or death, victory) so the greeter always holds the
        // truth, never a memory of a transition
        void publishStage();
        // the sensor availability policy of [Fingerprint] SensorPolicy,
        // validated (anything unknown falls back to ThreeAttempts)
        QString sensorPolicy() const;
        int sensorRetryWindows() const;
        // a fingerprint channel died without any sensor activity
        // (refused birth, locked account, missing device): recreate
        // it slowly, or the broker spins in a hot rebirth loop
        void scheduleFingerprintRebirth();

        Display *m_display;
        QPointer<AuthChannel> m_fingerprintChannel;
        QPointer<AuthChannel> m_keyboardChannel;
        // the channel that won the race; its exit ends the cycle
        QPointer<AuthChannel> m_winner;

        QString m_user;
        Session m_session;
        QPointer<QLocalSocket> m_socket;

        State m_state = Idle;
        // teardown in progress (display stopping): dead channels are
        // not recreated during the dismantling
        bool m_dead = false;
        // failed sensor conversations (verify windows) of the current
        // race — the ThreeAttempts budget
        int m_fingerDeaths = 0;
        // the password of an airlock submission, waiting for the
        // keyboard channel to hold its prompt; empty outside the
        // birth-to-submission window
        QString m_pendingPassword;
    };
}

#endif // SDDM_AUTH_BROKER_H
