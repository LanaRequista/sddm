/***************************************************************************
* Copyright (c) 2015 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
* Copyright (c) 2013 Abdurrahman AVCI <abdurrahmanavci@gmail.com
*
* Permission is hereby granted, free of charge, to any person
* obtaining a copy of this software and associated documentation
* files (the "Software"), to deal in the Software without restriction,
* including without limitation the rights to use, copy, modify, merge,
* publish, distribute, sublicense, and/or sell copies of the Software,
* and to permit persons to whom the Software is furnished to do so,
* subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
* OR OTHER DEALINGS IN THE SOFTWARE.
*
***************************************************************************/

import QtQuick 2.0
import SddmComponents 2.0

Rectangle {
    id: container
    width: 1024
    height: 768

    LayoutMirroring.enabled: Qt.locale().textDirection == Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    property int sessionIndex: session.index

    // login name the interactive conversation is running for ("" = none);
    // the conversation keeps the fingerprint reader hot from the start
    property string conversationUser: ""

    // user whose conversation was REQUESTED but not started yet; rapid
    // selection changes (avatar bursts) coalesce here: only the last one
    // survives the debounce window, so one login attempt is made per
    // stable selection instead of one per click — each cancel-and-
    // restart cycle kills a helper and races a late success (19:43)
    property string pendingConversationUser: ""

    function requestConversation(userName) {
        if (userName === "")
            return;
        pendingConversationUser = userName;
        conversationDebounce.restart();
    }

    Timer {
        id: conversationDebounce
        interval: 500
        repeat: false
        onTriggered: {
            const userName = pendingConversationUser;
            pendingConversationUser = "";
            startConversation(userName);
        }
    }

    function startConversation(userName) {
        if (!primaryScreen || userName === "")
            return;
        if (conversationUser === userName)
            return;
        restartConversation(userName);
    }

    function restartConversation(userName) {
        if (!primaryScreen || userName === "")
            return;
        // cancel the running conversation, if any; the daemon blocks until
        // the helper is gone, so the new one is always accepted
        if (conversationUser !== "")
            sddm.cancelLogin();
        conversationUser = userName;
        sddm.loginInteractive(userName, sessionIndex);
    }

    Component.onCompleted: {
        startConversation(userModel.lastUser)
        // lastUser can be empty (fresh install, unreadable or absent state
        // file): fall back to the preselected user, once its delegate exists
        startupConversation.start()
    }

    Timer {
        id: startupConversation
        interval: 100
        repeat: false
        onTriggered: {
            if (conversationUser === "" && listView.currentItem)
                startConversation(listView.currentItem.loginName)
        }
    }

    onSessionIndexChanged: {
        // the session is fixed when the conversation starts: restart it
        // when the user picks another one
        if (conversationUser !== "")
            restartConversation(listView.currentItem.loginName);
    }

    TextConstants { id: textConstants }

    Connections {
        target: sddm
        function onLoginSucceeded() {
        }

        function onLoginFailed() {
            txtMessage.text = textConstants.loginFailed
            listView.currentItem.password = ""
            // the conversation is dead: forget it, so the next avatar
            // click (even on the same user) really starts a fresh one
            // (startConversation is idempotent only while one runs)
            conversationUser = ""
            // no automatic restart: every conversation restart is a REAL
            // PAM attempt, and chained failures lock the account through
            // pam_faillock. The user decides when to retry, by clicking
            // their avatar.
        }

        function onInformationMessage(message) {
            txtMessage.text = message
        }
    }

    Background {
        anchors.fill: parent
        source: "qrc:///theme/background.png"
        fillMode: Image.PreserveAspectCrop
        onStatusChanged: {
            if (status == Image.Error && source != config.defaultBackground) {
                source = config.defaultBackground
            }
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {
                listView.focus = true;
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        //visible: primaryScreen

        Component {
            id: userDelegate

            PictureBox {
                anchors.verticalCenter: parent.verticalCenter
                name: (model.realName === "") ? model.name : model.realName
                icon: model.icon
                showPassword: model.needsPassword

                property string loginName: model.name

                focus: (listView.currentIndex === index) ? true : false
                state: (listView.currentIndex === index) ? "active" : ""

                onLogin: {
                    if (conversationUser === model.name) {
                        // never submit an empty password to the running
                        // conversation: the held prompt would get an empty
                        // answer and an immediate PAM failure
                        if (password !== "")
                            sddm.loginPassword(password);
                    } else {
                        sddm.login(model.name, password, sessionIndex);
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        listView.currentIndex = index;
                        // every avatar click (or burst of clicks) goes
                        // through the debounced request: only the last
                        // stable selection gets a login attempt
                        requestConversation(model.name);
                        listView.focus = true;
                    }
                }
            }
        }

        Row {
            anchors.fill: parent
            //visible: primaryScreen

            Rectangle {
                width: parent.width / 2; height: parent.height
                color: "#00000000"

                Clock {
                    id: clock
                    anchors.centerIn: parent
                    color: "white"
                    timeFont.family: "Oxygen"
                }
            }

            Rectangle {
                width: parent.width / 2; height: parent.height
                color: "#22000000"
                clip: true

                Item {
                    id: usersContainer
                    width: parent.width; height: 300
                    anchors.verticalCenter: parent.verticalCenter

                    ImageButton {
                        id: prevUser
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: 10
                        source: "qrc:///theme/angle-left.png"
                        onClicked: listView.decrementCurrentIndex()

                        KeyNavigation.backtab: btnShutdown; KeyNavigation.tab: listView
                    }

                    ListView {
                        id: listView
                        height: parent.height
                        anchors.left: prevUser.right; anchors.right: nextUser.left
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: 10

                        clip: true
                        focus: true

                        spacing: 5

                        model: userModel
                        delegate: userDelegate
                        orientation: ListView.Horizontal
                        currentIndex: userModel.lastIndex

                        onCurrentIndexChanged: {
                            // keyboard navigation between users goes
                            // through the same debounced request as
                            // avatar clicks
                            if (currentItem && currentItem.loginName !== conversationUser)
                                requestConversation(currentItem.loginName);
                        }

                        KeyNavigation.backtab: prevUser; KeyNavigation.tab: nextUser
                    }

                    ImageButton {
                        id: nextUser
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: 10
                        source: "qrc:///theme/angle-right.png"
                        onClicked: listView.incrementCurrentIndex()
                        KeyNavigation.backtab: listView; KeyNavigation.tab: session
                    }
                }

                Text {
                    id: txtMessage
                    anchors.top: usersContainer.bottom;
                    anchors.margins: 20
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: "white"
                    text: textConstants.promptSelectUser
                    wrapMode: Text.WordWrap
                    width:parent.width - 60
                    font.pixelSize: 20
                }

                Text {
                    id: errMessage
                    anchors.top: txtMessage.bottom
                    anchors.margins: 20
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: "red"
                    text: "The current theme cannot be loaded due to the errors below, please select another theme.\n" + __sddm_errors
                    wrapMode: Text.WordWrap
                    width: parent.width - 60
                    font.pixelSize: 20
                    visible: __sddm_errors !== ""
                }
            }
        }

        Rectangle {
            id: actionBar
            anchors.top: parent.top;
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width; height: 40
            //visible: primaryScreen

            Row {
                anchors.left: parent.left
                anchors.margins: 5
                height: parent.height
                spacing: 5

                Text {
                    height: parent.height
                    anchors.verticalCenter: parent.verticalCenter

                    text: textConstants.session
                    font.pixelSize: 16
                    verticalAlignment: Text.AlignVCenter
                }

                ComboBox {
                    id: session
                    width: 245
                    anchors.verticalCenter: parent.verticalCenter

                    arrowIcon: "qrc:///theme/angle-down.png"

                    model: sessionModel
                    index: sessionModel.lastIndex

                    font.pixelSize: 14

                    KeyNavigation.backtab: nextUser; KeyNavigation.tab: layoutBox
                }

                Text {
                    height: parent.height
                    anchors.verticalCenter: parent.verticalCenter

                    visible: layoutBox.visible

                    text: textConstants.layout
                    font.pixelSize: 16
                    verticalAlignment: Text.AlignVCenter
                }

                LayoutBox {
                    id: layoutBox
                    width: 90
                    anchors.verticalCenter: parent.verticalCenter
                    font.pixelSize: 14

                    visible: keyboard.enabled && keyboard.layouts.length > 0

                    arrowIcon: "qrc:///theme/angle-down.png"

                    KeyNavigation.backtab: session; KeyNavigation.tab: btnShutdown
                }
            }

            Row {
                height: parent.height
                anchors.right: parent.right
                anchors.margins: 5
                spacing: 5

                ImageButton {
                    id: btnReboot
                    height: parent.height
                    source: "qrc:///theme/reboot.png"

                    visible: sddm.canReboot

                    onClicked: sddm.reboot()

                    KeyNavigation.backtab: layoutBox; KeyNavigation.tab: btnShutdown
                }

                ImageButton {
                    id: btnShutdown
                    height: parent.height
                    source: "qrc:///theme/shutdown.png"

                    visible: sddm.canPowerOff

                    onClicked: sddm.powerOff()

                    KeyNavigation.backtab: btnReboot; KeyNavigation.tab: prevUser
                }
            }
        }
    }
}
