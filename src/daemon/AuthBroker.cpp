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
#include "Configuration.h"
#include "Display.h"

#include <QDebug>
#include <QTimer>
#include <QLocalSocket>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>
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
        // once the race is decided nothing new may spawn here: the
        // winner is opening its session, the greeter is leaving.
        // First line of defense: a resolved race is sacred, it
        // cannot be stopped from below either (the stop() of a
        // user switch must never reach the winner)
        if (m_state == Resolved) {
            qWarning() << "AuthBroker: login request after a resolved race, ignoring";
            return;
        }

        // a different user ends the current channels first: a PAM
        // conversation belongs to one user and cannot be recycled
        if (user != m_user && (m_fingerprintChannel || m_keyboardChannel))
            stop();

        // confirming the user whose channels already run restarts
        // nothing (E3): the channels live, the greeter keeps its
        // field ready, the race stays as it is
        if (user == m_user && m_state == Running) {
            m_socket = socket;
            return;
        }

        m_user = user;
        m_session = session;
        m_socket = socket;
        m_state = Running;
        m_dead = false;
        // a new race: the sensor budget starts clean (the policy is
        // X windows per race, not per daemon), and each PAM sentence
        // of the sensor is allowed one display again
        m_fingerDeaths = 0;
        m_shownFingerMessages.clear();

        spawnChannels();
        publishStage();
    }

    void AuthBroker::stop() {
        // user switch, greeter cancellation or display teardown: the
        // channels die quietly (no failure counted, nothing
        // reported), the state returns to idle and nothing is
        // reborn until the next user choice
        m_dead = true;
        m_winner = nullptr;
        m_pendingPassword.clear();
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
        publishStage();
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
            return;
        }

        // an empty submission opens nothing: no channel is born
        // from an empty password
        if (password.isEmpty()) {
            qWarning() << "AuthBroker: empty password, not opening a keyboard channel";
            return;
        }

        // the airlock: no keyboard door is open, the submission opens
        // one. The password waits for the newborn channel to hold its
        // prompt (onRequestChanged delivers it); if the birth fails
        // nothing respawns by itself — the next Enter retries
        m_pendingPassword = password;
        AuthChannel *channel = spawnChannel(AuthChannel::Keyboard);
        if (!channel) {
            // stillborn: a non-event (deterministic failure), the
            // pending password dies with the channel
            m_pendingPassword.clear();
            qWarning() << "AuthBroker: keyboard channel stillborn, the next submission retries";
            return;
        }
        m_keyboardChannel = channel;
    }

    void AuthBroker::onRequestChanged(AuthChannel *channel) {
        if (channel != m_keyboardChannel || m_pendingPassword.isEmpty())
            return;

        // the channel just received a PAM request; handleRequest(),
        // which holds the password prompt, runs when the signal
        // returns — deliver from the next event loop turn so the
        // prompt is actually held when submit() serves it
        QTimer::singleShot(0, this, [this, channel] {
            if (channel != m_keyboardChannel || m_pendingPassword.isEmpty())
                return;
            if (channel->state() != AuthChannel::Conversing)
                return;
            const QString password = m_pendingPassword;
            m_pendingPassword.clear();
            channel->submit(password);
        });
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

    // Classification rides on native codes, never on labels: the
    // broker subscribes once to fprintd's verify D-Bus signal and the
    // verdict of every verify window comes from the native codes,
    // invariant across languages and pam_fprintd updates. The PAM
    // sentences still travel verbatim to the greeter for display —
    // they just never decide anything.
    bool AuthBroker::ensureVerifySubscription() {
        if (!m_verifyDevicePath.isEmpty())
            return true;

        // the device is resolved through the manager, never
        // hardcoded (the same road as fingerprintAvailable): the
        // bus name and object paths carry a capital F
        const QDBusConnection bus = QDBusConnection::systemBus();
        QDBusInterface manager(QStringLiteral("net.reactivated.Fprint"),
                               QStringLiteral("/net/reactivated/Fprint/Manager"),
                               QStringLiteral("net.reactivated.Fprint.Manager"),
                               bus);
        if (!manager.isValid()) {
            qWarning() << "AuthBroker: fprintd manager not reachable, no verify subscription";
            return false;
        }

        const QDBusReply<QDBusObjectPath> device = manager.call(QStringLiteral("GetDefaultDevice"));
        if (!device.isValid()) {
            qWarning() << "AuthBroker: no fingerprint device for the verify subscription:" << device.error().message();
            return false;
        }

        // the two dialects of the same verdict signal (see the slots'
        // comment): a Qt D-Bus connect matches on name AND derived
        // signature — with a wrong slot signature the connect
        // appears to succeed and the signal is silently never
        // delivered (no feedback, no budget, endless rebirth).
        // Both connects are attempted; an absent dialect simply never
        // delivers, the present one feeds the classifier
        const bool legacy = QDBusConnection::systemBus().connect(QStringLiteral("net.reactivated.Fprint"),
                                    device.value().path(),
                                    QStringLiteral("net.reactivated.Fprint.Device"),
                                    QStringLiteral("VerifyStatus"),
                                    this, SLOT(onVerifyStatus(QString,bool)));
        const bool modern = QDBusConnection::systemBus().connect(QStringLiteral("net.reactivated.Fprint"),
                                    device.value().path(),
                                    QStringLiteral("net.reactivated.Fprint.Device"),
                                    QStringLiteral("VerifyResult"),
                                    this, SLOT(onVerifyResult(bool,QString)));
        if (!legacy && !modern) {
            qWarning() << "AuthBroker: neither VerifyStatus nor VerifyResult subscription accepted on" << device.value().path();
            return false;
        }
        qDebug() << "AuthBroker: connected to the verify signal on" << device.value().path()
                 << "(legacy sb =" << legacy << ", modern bs =" << modern << ")";

        m_verifyDevicePath = device.value().path();
        return true;
    }

    // The codes of fprintd (libfprint's verify results), the only
    // classifier inputs of the engine:
    //  - verify-no-match: a REAL finger was analyzed and refused --
    //    the one honest death of a window (budget, refusal feedback);
    //  - verify-timeout: the window expired untouched -- a quiet
    //    non-event: no feedback, no budget, warm rebirth;
    //  - anything else (verify-retry-scan, verify-swipe-too-short,
    //    verify-finger-not-centered, ...): an in-window retry, the
    //    window is still alive, nothing is classified (the benign
    //    fallback is now structural: an unknown code counts nothing
    //    and lies to nobody, whatever the language of the PAM text).
    // The signal is exclusive with a live claim: while a sensor
    // channel of this broker runs, fprintd serves no other verify
    // (sudo included); the guards below keep a code arriving outside
    // a live window (foreign verify during a rebirth gap) from
    // marking anything at all.
    void AuthBroker::onVerifyStatus(const QString &result, bool done) {
        Q_UNUSED(done)
        classifyVerify(result);
    }

    void AuthBroker::onVerifyResult(bool result, const QString &error) {
        Q_UNUSED(result)
        classifyVerify(error);
    }

    void AuthBroker::classifyVerify(const QString &code) {
        // trace line: the codes the device really emits land in the
        // journal, so a dialect's exact wording is never assumed
        qDebug() << "AuthBroker: verify code =" << code;

        if (m_state != Running || !m_fingerprintChannel)
            return;

        if (code == QLatin1String("verify-no-match")) {
            // one feedback per refused window: the theme reacts to
            // every analyzed-and-refused swipe, live, not once per
            // conversation
            if (m_socket)
                Q_EMIT sensorVerifyFailed(m_socket);
            m_fingerprintChannel->setRefused();

            // the live budget: every refused window consumes one unit
            // HERE, at the moment the sensor says no — never later at
            // the conversation's death, where a pam_fprintd that
            // retries internally would bill three fingers as one.
            // The channel itself is never killed here: with
            // max-tries=1 the PAM conversation dies on its own right
            // after this swipe, and the existing channelDied logic
            // (refused + budget spent -> no rebirth, stage published)
            // does the switch — cancelling a PAM conversation
            // mid-dialog is never a safe move
            ++m_fingerDeaths;
        } else if (code == QLatin1String("verify-timeout")) {
            m_fingerprintChannel->setTimedOut();
        }
    }

    // the rebirth cycle must stay invisible: one display per sentence
    // per race -- the repeated hint of every reborn window never
    // reaches the greeter; what happens on the sensor is told by the
    // refusal feedback and the stage, not by a notification row
    // bouncing at every window
    bool AuthBroker::shownFingerAlready(const QString &text) {
        if (m_shownFingerMessages.contains(text))
            return true;
        m_shownFingerMessages.insert(text);
        return false;
    }

    void AuthBroker::onInfo(AuthChannel *channel, const QString &text, Auth::Info type) {
        Q_UNUSED(type)

        const bool fingerprintRoute = channel->type() == AuthChannel::Fingerprint && m_state == Running;
        if (fingerprintRoute && shownFingerAlready(text))
            return;

        // shown verbatim: the truth on screen comes from the PAM
        // conversation, never from a second source
        Q_EMIT message(m_socket, text);
    }

    void AuthBroker::onError(AuthChannel *channel, const QString &text, Auth::Error type) {
        const bool fingerprintRoute = channel->type() == AuthChannel::Fingerprint && m_state == Running;
        const bool silenceDisplay = fingerprintRoute && shownFingerAlready(text);

        // the message carries the PAM truth verbatim, whatever channel
        // spoke; the greeter displays it as it arrived (deduplicated
        // on the sensor road)
        if (!silenceDisplay)
            Q_EMIT message(m_socket, text);

        // a sensor error is not a refused submission -- the user may
        // be typing right now, and the theme's failure treatment
        // (field wipe, failure label) must not erase it: a
        // loginFailed fired on a sensor death would wipe the
        // password field mid-typing
        if (type == Auth::ERROR_AUTHENTICATION && m_state == Running && m_socket
                && channel->type() == AuthChannel::Keyboard)
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
        // the airlock: only the sensor opens a door at the start of
        // the race. The keyboard channel is born on the first
        // password submission (submitPassword) — an always-open
        // keyboard door would have to be cancelled on every sensor
        // victory, and PAM counts a cancelled conversation as a
        // failure (ghost failures in the system counter)

        // the fingerprint channel exists when the system has
        // something to verify: a birth decision, never a display
        // decision. The same gesture subscribes the broker to the
        // native VerifyStatus codes -- the classifier of every sensor
        // window (a subscription that fails narrows to the benign
        // fallback: deaths stay unclassified, nothing is counted, no
        // lie is told)
        ensureVerifySubscription();
        if (!m_fingerprintChannel && fingerprintAvailable(m_user))
            m_fingerprintChannel = spawnChannel(AuthChannel::Fingerprint);
    }

    void AuthBroker::publishStage() {
        // the only computation of the factor stage, always from the
        // live state -- UI truth is never a memory of a transition
        QString stage;
        if (m_state == Running)
            stage = m_fingerprintChannel
                    ? QStringLiteral("fingerprint") : QStringLiteral("password");
        Q_EMIT authStage(m_socket, stage);
    }

    QString AuthBroker::sensorPolicy() const {
        const QString raw = mainConfig.Fingerprint.SensorPolicy.get();
        if (raw == QLatin1String("Always") || raw == QLatin1String("Faillock"))
            return raw;
        if (raw != QLatin1String("ThreeAttempts"))
            qWarning() << "AuthBroker: unknown Fingerprint.SensorPolicy" << raw << ", falling back to ThreeAttempts";
        return QStringLiteral("ThreeAttempts");
    }

    int AuthBroker::sensorRetryWindows() const {
        return qBound(1, mainConfig.Fingerprint.SensorRetryWindows.get(), 10);
    }

    void AuthBroker::scheduleFingerprintRebirth() {
        // deterministic death: refused birth, locked account, missing
        // device — an immediate rebirth would spin in a hot loop;
        // the channel comes back when the cause has had time to clear
        const QString user = m_user;
        QTimer::singleShot(30 * 1000, this, [this, user] {
            if (m_dead || m_state != Running || m_fingerprintChannel || m_user != user)
                return;
            if (fingerprintAvailable(m_user)) {
                m_fingerprintChannel = spawnChannel(AuthChannel::Fingerprint);
                publishStage();
            }
        });
    }

    AuthChannel *AuthBroker::spawnChannel(AuthChannel::Type type) {
        // one method, one rule: the keyboard rule carries no
        // pam_fprintd (the password prompt is immediate), the
        // fingerprint rule is a standalone fprintd stack; the
        // Faillock policy swaps it for the strict variant whose
        // failures feed the system lockout through a counter of
        // its own (dir=/run/faillock-sensor)
        QString service;
        if (type == AuthChannel::Keyboard)
            service = QStringLiteral("sddm-password");
        else if (sensorPolicy() == QLatin1String("Faillock"))
            service = QStringLiteral("sddm-fingerprint-strict");
        else
            service = QStringLiteral("sddm-fingerprint");

        AuthChannel *channel = new AuthChannel(type, service, m_display);

        connect(channel, &AuthChannel::stateChanged, this, &AuthBroker::onStateChanged);
        connect(channel, &AuthChannel::authenticated, this, &AuthBroker::onAuthenticated);
        connect(channel, &AuthChannel::info, this, &AuthBroker::onInfo);
        connect(channel, &AuthChannel::error, this, &AuthBroker::onError);
        connect(channel, &AuthChannel::requestChanged, this, &AuthBroker::onRequestChanged);
        connect(channel, &AuthChannel::sessionStarted, this, [this](AuthChannel *, bool success) {
            Q_EMIT sessionStarted(success);
        });
        connect(channel, &AuthChannel::sessionFinished, this, &AuthBroker::onSessionFinished);

        if (!channel->start(m_user, m_session)) {
            // stillborn: a non-event and a silent one — the channel
            // muted itself (silence()) before dying, so no
            // stateChanged ever reaches the broker; the caller sees
            // the nullptr and owns the consequence (pending password
            // dropped, nothing reborn)
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

        // the rebirth policy, kind by kind: the sensor reborns after
        // every death unless its budget is spent; the keyboard does
        // not reborn on its own (the airlock rule: only a submission,
        // a human gesture, opens a new door). The user never faces a
        // screen where he cannot authenticate — SDDM does not own
        // the right to lock him out, the counters and locks of the
        // system are shown verbatim, they are not implemented twice
        // here.
        if (channel->type() == AuthChannel::Keyboard) {
            // the airlock closes again: a dead keyboard channel is
            // NOT recreated here — the death of a conversation is an
            // honest system event (failed password, shown lock) and
            // rebirth is a human gesture, the next Enter (the B2
            // rule). The pending password, if the channel died
            // before holding its prompt, dies with it
            m_pendingPassword.clear();
            m_keyboardChannel = nullptr;
            channel->deleteLater();
        } else if (channel->type() == AuthChannel::Fingerprint) {
            m_fingerprintChannel = nullptr;
            const bool refused = channel->refused();
            const bool engaged = channel->engaged();
            channel->deleteLater();

            // deterministic death: no sensor activity was ever seen
            // (locked account, refused birth, missing device) — an
            // immediate rebirth would spin in a hot loop; the
            // channel comes back when the cause has had time to
            // clear
            if (!engaged) {
                qWarning() << "AuthBroker: fingerprint channel died without any sensor activity, rebirth in 30 s";
                scheduleFingerprintRebirth();
                publishStage();
                return;
            }

            // a REFUSED finger is the only honest death: it alone
            // consumes the budget. A window that timed out
            // untouched is a quiet non-event — no feedback, no
            // budget — and it reborns at once: the sensor stays
            // warm, the stage never flickers, watching the greeter
            // looks like nothing ever happens
            if (!refused) {
                if (fingerprintAvailable(m_user))
                    m_fingerprintChannel = spawnChannel(AuthChannel::Fingerprint);
                publishStage();
                return;
            }

            // The budget is consumed LIVE by the classifier
            // (classifyVerify), one refused window at the moment the
            // sensor says no — the conversation's death never counts:
            // a pam_fprintd that retries internally bills each finger
            // once, exactly what the user experienced. Here we only
            // honor the state left by the classifier: a refused
            // conversation whose budget is gone does not reborn
            if (sensorPolicy() == QLatin1String("ThreeAttempts")
                    && m_fingerDeaths >= sensorRetryWindows()) {
                qWarning() << "AuthBroker: sensor budget exhausted"
                           << "(" << m_fingerDeaths << "refused windows), only the keyboard channel remains";
                publishStage();
                return;
            }

            if (fingerprintAvailable(m_user))
                m_fingerprintChannel = spawnChannel(AuthChannel::Fingerprint);
            publishStage();
        }
    }

    void AuthBroker::declareWinner(AuthChannel *channel, const QString &user) {
        m_state = Resolved;
        m_winner = channel;
        channel->markWinner();
        // the race is over: a submitted password waiting for a
        // newborn keyboard channel dies here — the new race gets a
        // fresh submission
        m_pendingPassword.clear();
        publishStage();

        // announce success to the greeter BEFORE the session exists:
        // an announcement made after the launch could never render,
        // because the session's compositor takes the DRM master and
        // the greeter's outputs die the same second. No hold, no
        // animation: whatever plays here would delay every login by
        // its full duration — the winner proceeds the moment the
        // announcement is out
        if (m_socket)
            Q_EMIT loginSucceeded(m_socket);

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
    }

    bool AuthBroker::fingerprintAvailable(const QString &user) const {
        // a birth decision only: does the system have enrolled fingers
        // for this user? What the greeter displays always comes from
        // the conversation itself, never from here
        //
        // the bus name and object paths carry a capital F
        // ("net.reactivated.Fprint"); the device is resolved through
        // the manager, never hardcoded
        const QDBusConnection bus = QDBusConnection::systemBus();
        QDBusInterface manager(QStringLiteral("net.reactivated.Fprint"),
                               QStringLiteral("/net/reactivated/Fprint/Manager"),
                               QStringLiteral("net.reactivated.Fprint.Manager"),
                               bus);
        if (!manager.isValid()) {
            qWarning() << "AuthBroker: fprintd manager not reachable, no fingerprint channel";
            return false;
        }

        const QDBusReply<QDBusObjectPath> device = manager.call(QStringLiteral("GetDefaultDevice"));
        if (!device.isValid()) {
            qWarning() << "AuthBroker: no fingerprint device:" << device.error().message();
            return false;
        }

        QDBusInterface sensor(QStringLiteral("net.reactivated.Fprint"), device.value().path(),
                               QStringLiteral("net.reactivated.Fprint.Device"),
                               bus);
        if (!sensor.isValid()) {
            qWarning() << "AuthBroker: fingerprint device not reachable, no fingerprint channel";
            return false;
        }

        const QDBusReply<QStringList> fingers = sensor.call(QStringLiteral("ListEnrolledFingers"), user);
        if (!fingers.isValid()) {
            // an error here simply means "nothing to verify for this
            // user" (no prints enrolled): no channel is born for him
            qWarning() << "AuthBroker: ListEnrolledFingers:" << fingers.error().message();
            return false;
        }

        return !fingers.value().isEmpty();
    }
}
