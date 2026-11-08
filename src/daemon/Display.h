/***************************************************************************
* Copyright (c) 2014-2015 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
* Copyright (c) 2014 Martin Bříza <mbriza@redhat.com>
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

#ifndef SDDM_DISPLAY_H
#define SDDM_DISPLAY_H

#include <QObject>
#include <QPointer>
#include <QDir>

#include "Auth.h"
#include "Session.h"

class QLocalSocket;

namespace SDDM {
    class Authenticator;
    class DisplayServer;
    class Seat;
    class SocketServer;
    class Greeter;

    class Display : public QObject {
        Q_OBJECT
        Q_DISABLE_COPY(Display)
    public:
        enum DisplayServerType {
            X11DisplayServerType,
            X11UserDisplayServerType,
            WaylandDisplayServerType
        };
        Q_ENUM(DisplayServerType)

        static DisplayServerType defaultDisplayServerType();
        explicit Display(Seat *parent, DisplayServerType serverType);
        ~Display();

        DisplayServerType displayServerType() const;
        DisplayServer *displayServer() const;

        int terminalId() const;

        const QString &name() const;

        QString sessionType() const;
        QString reuseSessionId() const { return m_reuseSessionId; }

        Seat *seat() const;

    public slots:
        bool start();
        void stop();

        void login(QLocalSocket *socket,
                   const QString &user, const QString &password,
                   const Session &session);
        void loginInteractive(QLocalSocket *socket,
                              const QString &user, const Session &session);
        void loginPassword(QLocalSocket *socket, const QString &password);
        void cancelLogin(QLocalSocket *socket);
        void displayServerStarted();

    signals:
        void stopped();
        void displayServerFailed();

        void loginFailed(QLocalSocket *socket);
        void loginSucceeded(QLocalSocket *socket);

    private:
        QString findGreeterTheme() const;
        bool findSessionEntry(const QStringList &dirPaths, const QString &name) const;

        // start an authentication; in an interactive conversation the
        // password prompts are held back until the greeter sends one,
        // legacy logins and autologin submit what they were given upfront
        bool startAuth(const QString &user, const QString &password,
                       const Session &session, bool interactive = false);

        // fill the prompts of the pending request from the known user and
        // the queued password, and complete the request when no password
        // prompt is left unanswered
        void tryCompleteAuthRequest();

        // start the interactive conversation queued while a cancellation
        // was running (CancelLogin and LoginInteractive arrive in the same
        // burst, and the cancel blocks the daemon on nested event loops)
        void startPendingInteractive();

        void startSocketServerAndGreeter();
        bool handleAutologinFailure();

        DisplayServerType m_displayServerType = X11DisplayServerType;

        bool m_started { false };

        int m_terminalId = -1;
        int m_sessionTerminalId = 0;

        QString m_passPhrase;
        bool m_passwordQueued { false };
        // the running conversation is interactive (started by the greeter
        // without a password): its password prompts are never answered
        // with anything but a password the greeter actually sent
        bool m_interactive { false };
        bool m_cancelling { false };
        // the current login attempt succeeded: the helper is opening the
        // user session, no cancellation or replacement conversation may
        // touch it; reset when a new attempt starts or the helper exits
        bool m_authSucceeded { false };
        // last session accepted by startAuth(): a password typed after a
        // failed conversation restarts one for the same user and session
        Session m_lastSession;

        // interactive conversation requested while a cancellation is
        // running, started once the old helper is gone
        bool m_pendingInteractive { false };
        QString m_pendingUser;
        QString m_pendingPassword;
        Session m_pendingSession;
        QPointer<QLocalSocket> m_pendingSocket;
        QString m_sessionName;
        QString m_reuseSessionId;

        Session m_autologinSession;

        Auth *m_auth { nullptr };
        DisplayServer *m_displayServer { nullptr };
        Seat *m_seat { nullptr };
        SocketServer *m_socketServer { nullptr };
        QPointer<QLocalSocket> m_socket;
        Greeter *m_greeter { nullptr };

    private slots:
        void slotRequestChanged();
        void slotAuthenticationFinished(const QString &user, bool success);
        void slotSessionStarted(bool success);
        void slotHelperFinished(Auth::HelperExitStatus status);
        void slotAuthInfo(const QString &message, Auth::Info info);
        void slotAuthError(const QString &message, Auth::Error error);
    };
}

#endif // SDDM_DISPLAY_H
