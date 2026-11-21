/*
 * An authentication channel: one PAM conversation for one method
 * (fingerprint, keyboard), isolated from its siblings and from the
 * greeter. It owns its own Auth instance (socket, id, helper process)
 * and its session bundle, fixed at birth and never shared.
 *
 * The channel never coordinates anything: it observes its
 * conversation, answers its prompts from the greeter's input and
 * pushes its state to the broker the moment it changes. The broker
 * interprets; the channel reports.
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

#ifndef SDDM_AUTH_CHANNEL_H
#define SDDM_AUTH_CHANNEL_H

#include <QObject>

#include "Auth.h"
#include "Session.h"

namespace SDDM {
    class Display;

    class AuthChannel : public QObject {
        Q_OBJECT
    public:
        // one channel per authentication method; the PAM service
        // (rule) carrying the method is fixed at construction
        enum Type {
            Fingerprint,
            Keyboard
        };
        Q_ENUM(Type)

        enum State {
            Spawning,       // the helper process is being started
            Conversing,     // the PAM conversation is running
            SessionRunning, // the channel's helper opened the user session
            Dead            // the helper is gone: the conversation is over
        };
        Q_ENUM(State)

        AuthChannel(Type type, const QString &pamService, Display *display);
        ~AuthChannel();

        Type type() const;
        State state() const;

        // true when this channel ever saw activity of its method (a
        // PAM request, an info message) — a channel dying without
        // ever engaging died of a deterministic cause (refused birth,
        // locked account, missing device): the broker pauses instead
        // of recreating it in a hot loop. An error alone does NOT
        // engage (a faillock denial must keep the slow-rebirth pause)
        bool engaged() const;

        // why a fingerprint channel died, told by the broker from the
        // conversation's last messages (a mismatch vs a timeout)
        bool refused() const;
        bool timedOut() const;
        void setRefused();
        void setTimedOut();

        // the private Auth instance of this conversation, for the
        // display to attach the X11 cookie on victory
        Auth *auth() const;

        // engenders the helper and starts the conversation; the
        // bundle (session environment) is fixed here, at birth, and
        // never modified afterwards
        bool start(const QString &user, const Session &session);

        // losing channel, or user switch: the existing primitive,
        // clean signal first, grace period, kill only if the helper
        // cannot obey (blocked inside a PAM module)
        void requestShutdown();

        // serves the pending password prompt with the value the
        // greeter submitted; holds the prompt back until then
        void submit(const QString &password);

        // one last message arrived after the race was decided:
        // the channel stops reporting anything
        void silence();

        // the broker declares this channel the winner: from now on
        // its exit means the session is over (logout) or failed to
        // start, and the display cycle takes over
        void markWinner();

    signals:
        // pushed the moment the state changes; the broker interprets
        void stateChanged(SDDM::AuthChannel *channel);

        // the conversation authenticated the user; the broker decides
        // whether this channel won the race
        void authenticated(SDDM::AuthChannel *channel, const QString &user);

        // everything the greeter may display, forwarded verbatim
        void info(SDDM::AuthChannel *channel, const QString &message, SDDM::Auth::Info type);
        void error(SDDM::AuthChannel *channel, const QString &message, SDDM::Auth::Error type);
        void requestChanged(SDDM::AuthChannel *channel);

        // the session of this channel's helper STARTED; meaningful
        // only for the winner. Reported on success only: a session
        // that failed to start is reported through sessionFinished
        // (HelperExitStatus), when the helper exits
        void sessionStarted(SDDM::AuthChannel *channel, bool success);
        void sessionFinished(SDDM::AuthChannel *channel, Auth::HelperExitStatus status);

    private:
        void setState(State state);
        void handleRequest();
        void handleAuthentication(const QString &user, bool success);
        void handleHelperFinished(Auth::HelperExitStatus status);

        Type m_type;
        QString m_pamService;
        Auth *m_auth;
        Display *m_display;

        QString m_user;
        Session m_session;
        State m_state;

        // the broker declared this channel the winner
        bool m_winner;

        // any activity ever received from this channel's helper
        bool m_engaged = false;

        // the fingerprint channel died because a real finger was
        // REFUSED (a mismatch), as opposed to a verify window timing
        // out untouched. Set by the broker from fprintd's NATIVE
        // D-Bus codes (verify-no-match / verify-timeout) -- the
        // engine classifies machine codes, never translated PAM
        // sentences
        bool m_refused = false;

        // the fingerprint channel's last window expired without a
        // finger ever being refused
        bool m_timedOut = false;

        // a channel being shut down stops reporting: its last words
        // would be the symptoms of an execution, not of a failure
        bool m_silenced;
    };
}

#endif // SDDM_AUTH_CHANNEL_H
