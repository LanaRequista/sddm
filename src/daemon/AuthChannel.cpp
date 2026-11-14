/*
 * An authentication channel: one PAM conversation for one method.
 * See AuthChannel.h for the design.
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

#include "AuthChannel.h"
#include "Display.h"

#include <QDebug>

namespace SDDM {
    AuthChannel::AuthChannel(Type type, const QString &pamService, Display *display)
        : QObject(display)
        , m_type(type)
        , m_pamService(pamService)
        , m_auth(new Auth(this))
        , m_display(display)
        , m_state(Dead)
        , m_winner(false)
        , m_silenced(false)
    {
        m_auth->setVerbose(true);
        m_auth->setPamService(pamService);

        connect(m_auth, &Auth::requestChanged, this, [this] {
            // a request or an info message is a LIVE conversation: the
            // method engaged (the fprintd finger hint is an info, a
            // password prompt is a request) — the broker may judge a
            // later death honest, counting the window in the sensor
            // budget and reborn at once. An error alone does NOT
            // engage: a locked account answers with a bare faillock
            // error before anything ran, and honouring it would
            // relaunch a hot rebirth loop instead of the
            // deterministic 30 s pause. (The assignments were lost on
            // 11 nov. by the edit tool: every sensor death behaved
            // as deterministic — cold sensor, exhausted-budget
            // policies never triggering.)
            m_engaged = true;
            if (!m_silenced)
                Q_EMIT requestChanged(this);
            handleRequest();
        });
        connect(m_auth, &Auth::authentication, this, &AuthChannel::handleAuthentication);
        connect(m_auth, &Auth::finished, this, &AuthChannel::handleHelperFinished);
        connect(m_auth, &Auth::sessionStarted, this, [this](bool success) {
            if (success) {
                setState(SessionRunning);
                Q_EMIT sessionStarted(this, true);
            }
        });
        connect(m_auth, &Auth::info, this, [this](const QString &message, Auth::Info type) {
            m_engaged = true;
            if (!m_silenced)
                Q_EMIT info(this, message, type);
        });
        connect(m_auth, &Auth::error, this, [this](const QString &message, Auth::Error type) {
            if (!m_silenced)
                Q_EMIT error(this, message, type);
        });
    }

    AuthChannel::~AuthChannel() {
        // the helper is our child through m_auth; Auth::stop takes
        // care of terminating it, if any residue is left
        m_auth->stop();
    }

    AuthChannel::Type AuthChannel::type() const {
        return m_type;
    }

    AuthChannel::State AuthChannel::state() const {
        return m_state;
    }

    bool AuthChannel::engaged() const {
        return m_engaged;
    }

    bool AuthChannel::refused() const {
        return m_refused;
    }

    bool AuthChannel::timedOut() const {
        return m_timedOut;
    }

    void AuthChannel::setRefused() {
        m_refused = true;
    }

    void AuthChannel::setTimedOut() {
        m_timedOut = true;
    }

    Auth *AuthChannel::auth() const {
        return m_auth;
    }

    bool AuthChannel::start(const QString &user, const Session &session) {
        if (m_state == Conversing || m_state == Spawning || m_state == SessionRunning) {
            qWarning() << "AuthChannel:" << m_pamService << "already running, not starting a second conversation";
            return false;
        }

        m_user = user;
        m_session = session;
        m_silenced = false;
        setState(Spawning);

        // the display builds the session bundle: terminal,
        // environment, display server command; everything is fixed
        // here, at birth, and never touched again
        if (!m_display->startChannelConversation(m_auth, user, session)) {
            // stillborn: a non-event, nobody is told — the broker
            // would recreate it in a loop (the failure is
            // deterministic: invalid session, no rule, ...)
            silence();
            setState(Dead);
            return false;
        }

        setState(Conversing);
        return true;
    }

    void AuthChannel::requestShutdown() {
        // a channel being put down stops reporting: its last messages
        // are the symptoms of an execution, not of a failure
        silence();
        m_auth->cancel();
    }

    void AuthChannel::submit(const QString &password) {
        if (m_state != Conversing || !m_auth->isActive()) {
            qWarning() << "AuthChannel:" << m_pamService
                       << "received a password without a running conversation, ignoring";
            return;
        }

        // serve the held password prompt; every other prompt keeps
        // the policy of handleRequest(). Unlike handleRequest(), the
        // answer IS completed here: done() writes the response to
        // the helper — handleRequest() only knows how to HOLD a
        // password prompt, calling it here would hold the answer
        // forever and the submission would never reach PAM (the
        // 22:17 live-test bug: Enter received, strictly nothing sent)
        bool served = false;
        AuthRequest *request = m_auth->request();
        for (AuthPrompt *prompt : request->prompts()) {
            if (prompt->type() == AuthPrompt::LOGIN_PASSWORD) {
                prompt->setResponse(password.toLocal8Bit());
                served = true;
            }
        }

        if (!served) {
            qWarning() << "AuthChannel:" << m_pamService
                       << "received a password while no password prompt is held, ignoring";
            return;
        }

        request->done();
    }

    void AuthChannel::silence() {
        m_silenced = true;
    }

    void AuthChannel::markWinner() {
        m_winner = true;
    }

    void AuthChannel::setState(State state) {
        if (m_state == state)
            return;

        m_state = state;

        if (!m_silenced)
            Q_EMIT stateChanged(this);
    }

    void AuthChannel::handleRequest() {
        AuthRequest *request = m_auth->request();
        if (request->prompts().isEmpty())
            return;

        setState(Conversing);

        // the prompt policy of an interactive conversation, v1
        // semantics unchanged: the user prompt is answered from the
        // known user, the password prompt is held back until the
        // greeter submits a value, unknown prompts complete with an
        // empty response (they belong to modules that are not asking
        // anything, e.g. the finger stage of pam_fprintd)
        bool waitingForPassword = false;
        for (AuthPrompt *prompt : request->prompts()) {
            switch (prompt->type()) {
                case AuthPrompt::LOGIN_USER:
                    prompt->setResponse(m_user.toLocal8Bit());
                    break;
                case AuthPrompt::LOGIN_PASSWORD:
                    waitingForPassword = true;
                    break;
                default:
                    break;
            }
        }

        if (!waitingForPassword)
            request->done();
    }

    void AuthChannel::handleAuthentication(const QString &user, bool success) {
        if (!success || m_silenced) {
            // the helper exits with AUTH_ERROR right after this; the
            // channel dies with its conversation, the broker decides
            // what to do on the state change
            return;
        }

        // the conversation authenticated the user. The broker
        // handles this synchronously: it must set the cookie and
        // save its decision before the authenticated reply carries
        // the bundle to the helper (the reply is composed when this
        // signal returns)
        Q_EMIT authenticated(this, user);
    }

    void AuthChannel::handleHelperFinished(Auth::HelperExitStatus status) {
        if (m_winner) {
            // the winner's exit means its session is over (logout)
            // or failed to start: the display cycle restarts a whole
            // greeter from scratch (the decision of the 12 nov.)
            Q_EMIT sessionFinished(this, status);
            setState(Dead);
            return;
        }

        // Spawning: the helper died at birth, a non-event.
        // Conversing: the conversation ended on its own, either by
        // an authentication refusal or by a cancellation; either
        // way the channel is dead and says so, the broker interprets
        setState(Dead);
    }
}
